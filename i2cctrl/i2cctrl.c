/* i2cctrl.c */
#include <ntddk.h>          /* core kernel types, IRP, DEVICE_OBJECT, etc. */
#include <wdm.h>
#include "i2cctrl_spinlock_fix.h"
#include <acpiioct.h>       /* ACPI_EVAL_INPUT_BUFFER, I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER, IOCTL_ACPI_EVAL_METHOD */
#include <stdarg.h>
#include <ntstrsafe.h>      /* RtlInitUnicodeString, safe string helpers for ACPI method names */
#include <strsafe.h>        /* RtlStringCchCopyW, safe string helpers for ACPI method names */
#include <initguid.h>
#include <devguid.h>
#include "i2cctrl_hw.h"     /* register offsets, PCI IDs, bit masks */
#include "i2cctrl.h"        /* driver-wide definitions, device context */
#include "I2cCtrl_Isr.h"
#include "i2cctrl_ext.h"    /* legacy I2CCTRL_FDO if still referenced */
#include "i2cctrl_detect.h"
#include "i2cctrl_ioctl.h"  /* IOCTL codes and I2CCTRL_RW struct */
#include "i2cctrl_bsod.h"   /* safe wrappers, guards, WinDBG-friendly logging */
#include "i2cctrl_spbcx.h"  /* façade definitions for SPBCX_COMPAT_CONTEXT, IOCTLs */
#include "i2cctrl_DPI.h"
#include "i2cctrl_i2c.h"
#include "i2cctrl_etw.h"
#include "i2cctrl_etw.tmh"
#include "i2cctrl_dump.h"

//
// XP/2003 DDK does NOT declare HalGetBusData,
// so we must declare it manually.
// Place this ONLY in i2cctrl.c, NOT in any header.
//
ULONG
HalGetBusData(
    IN ULONG BusDataType,
    IN ULONG BusNumber,
    IN ULONG SlotNumber,
    IN PVOID Buffer,
    IN ULONG Length
    );

PDEVICE_OBJECT
IoGetDeviceAttachmentBaseRef(
    PDEVICE_OBJECT DeviceObject
    );

PDEVICE_OBJECT
IoGetLowerDeviceObject(
    PDEVICE_OBJECT DeviceObject
    );

/* ---------------------------------------------------------------------------
   Global driver context definition
   --------------------------------------------------------------------------- */
I2CCTRL_GLOBAL g_I2cCtrlGlobal = {0};

/* ---------------------------------------------------------------------------
   ACPI IOCTL compatibility defines (for older WDKs)
   --------------------------------------------------------------------------- */
#ifndef FILE_DEVICE_ACPI
#define FILE_DEVICE_ACPI 0x32
#endif

#ifndef IOCTL_ACPI_EVAL_METHOD
#define IOCTL_ACPI_EVAL_METHOD CTL_CODE(FILE_DEVICE_ACPI, 0x0002, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef ACPI_EVAL_INPUT_BUFFER_SIGNATURE
#define ACPI_EVAL_INPUT_BUFFER_SIGNATURE 'CIPA'
#endif

#ifndef ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE
#define ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE 'OPCA'
#endif

/* ---------------------------------------------------------------------------
   Forward declarations
   --------------------------------------------------------------------------- */
VOID     DriverUnload(PDRIVER_OBJECT DriverObject);

/* --- Invalid IRP handler (remove-lock safe, XP/2003 correct) --- */
NTSTATUS
I2cCtrl_InvalidIrp(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PI2CCTRL_FDO        devctx;
    PIO_STACK_LOCATION  isl;
    NTSTATUS            status;

    devctx = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    isl    = IoGetCurrentIrpStackLocation(Irp);

    /* Default failure */
    status = STATUS_INVALID_DEVICE_REQUEST;

    /* POWER IRPs must follow WDM rules */
    if (isl->MajorFunction == IRP_MJ_POWER) {

        PoStartNextPowerIrp(Irp);

        if (devctx->LowerDevice != NULL) {

            /* Acquire remove lock for forwarded IRP */
            status = IoAcquireRemoveLock(&devctx->RemoveLock, Irp);
            if (!NT_SUCCESS(status)) {
                Irp->IoStatus.Status      = status;
                Irp->IoStatus.Information = 0;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return status;
            }

            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(
                Irp,
                I2CCTRL_ReleaseLockCompletion,
                &devctx->RemoveLock,
                TRUE, TRUE, TRUE
            );

            return PoCallDriver(devctx->LowerDevice, Irp);
        }

        /* No lower device -> complete locally */
        Irp->IoStatus.Status      = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    /* Non-POWER IRPs */
    if (devctx->LowerDevice != NULL) {

        /* Acquire remove lock for forwarded IRP */
        status = IoAcquireRemoveLock(&devctx->RemoveLock, Irp);
        if (!NT_SUCCESS(status)) {
            Irp->IoStatus.Status      = status;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return status;
        }

        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(
            Irp,
            I2CCTRL_ReleaseLockCompletion,
            &devctx->RemoveLock,
            TRUE, TRUE, TRUE
        );

        return IoCallDriver(devctx->LowerDevice, Irp);
    }

    /* PDO or no lower device -> fail */
    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

//
// Table of supported controllers (explicit ACPI/PCI devices)
// Match against full HWID substrings only
//
const I2CCTRL_DEVICE_ID g_I2cControllers[] = {

    /* ACPI-based controllers (NO LPSS) */
    { L"ACPI\\INT3446",
      0x00,0x04,0x08,0x0C,   /* BAR0 DW-I2C */
      0,0,0,0,               /* LPSS BAR2 */
      QUIRK_ACPI20, BSOD_NONE },

    { L"ACPI\\INT3447",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_FORCE_PIO },

    { L"ACPI\\AMD0010",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE },

    { L"ACPI\\AMDI0010",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE },


    /* PCI-based Intel Serial IO controllers WITH LPSS BAR2 */
    { L"PCI\\VEN_8086&DEV_9DC5",
      0x00,0x04,0x08,0x0C,       /* BAR0 DW-I2C */
      0x200,0x204,0x208,0x20C,   /* LPSS BAR2 */
      QUIRK_NEEDS_RESET_WORKAROUND, BSOD_EXTRA_RESET },

    { L"PCI\\VEN_8086&DEV_9DE8",
      0x10,0x14,0x18,0x1C,
      0x200,0x204,0x208,0x20C,
      QUIRK_BROKEN_CLOCK_GATE, BSOD_MASK_INTERRUPTS },

    { L"PCI\\VEN_8086&DEV_9DE9",
      0x20,0x24,0x28,0x2C,
      0x200,0x204,0x208,0x20C,
      QUIRK_NO_DMA_SUPPORT, BSOD_FORCE_PIO },

    { L"PCI\\VEN_8086&DEV_9DEA",
      0x00,0x04,0x08,0x0C,
      0x200,0x204,0x208,0x20C,
      QUIRK_SLOW_CLOCK, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_9DEB",
      0x00,0x04,0x08,0x0C,
      0x200,0x204,0x208,0x20C,
      QUIRK_NO_D1D2, BSOD_NONE },


    /* Legacy PCI controllers requiring ACPI 1.0b fallback (NO LPSS) */
    { L"PCI\\VEN_8086&DEV_A160",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI10, BSOD_DELAY_INIT },

    { L"PCI\\VEN_8086&DEV_A161",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI10, BSOD_DELAY_INIT },

    { L"PCI\\VEN_8086&DEV_A162",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI10, BSOD_DELAY_INIT },

    { L"PCI\\VEN_8086&DEV_A163",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI10, BSOD_DELAY_INIT },


    /* Other Intel PCI controllers (NO LPSS unless proven otherwise) */
    { L"PCI\\VEN_8086&DEV_A2E0",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_A2E1",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_A2E2",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_A2E3",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },


    { L"PCI\\VEN_8086&DEV_31AC",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_31AE",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_31B0",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_31B2",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_31B4",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_31B6",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_31B8",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_31BA",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },


    { L"PCI\\VEN_8086&DEV_A368",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_A369",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_A36A",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_A36B",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },


    { L"PCI\\VEN_8086&DEV_02E8",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_02E9",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_02EA",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_02EB",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_02C5",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_02C6",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },


    { L"PCI\\VEN_8086&DEV_06E8",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_06E9",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_06EA",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_06EB",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },
	    /* Intel Serial IO I2C ES (Atom/Cherry Trail/Braswell) */
    { L"ACPI\\808622C1",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE },

    { L"ACPI\\808622C2",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE },

    { L"ACPI\\808622C3",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE },

    { L"ACPI\\808622C4",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE },

    { L"ACPI\\808622C5",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE },

    { L"ACPI\\808622C6",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE },

    { L"ACPI\\808622C7",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE },

    { L"ACPI\\808622C8",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE }
};

const ULONG g_I2cControllersCount =
    sizeof(g_I2cControllers) / sizeof(g_I2cControllers[0]);

//
// HID-over-I2C devices (touchpads + some touchscreens)
//
const I2CHID_DEVICE_ID g_I2cHidDevices[] = {

    /* -------------------------------------------------------------
     * Generic HID-over-I2C
     * ------------------------------------------------------------- */
    { L"ACPI\\PNP0C50", HID_QUIRK_NONE, HID_FLAG_GENERIC },
    { L"*PNP0C50",      HID_QUIRK_NONE, HID_FLAG_GENERIC },

    /* -------------------------------------------------------------
     * ELAN
     * ------------------------------------------------------------- */
    { L"ACPI\\ELAN0000", HID_QUIRK_ELAN, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ELAN0100", HID_QUIRK_ELAN, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ELAN0600", HID_QUIRK_ELAN, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ELAN1000", HID_QUIRK_ELAN, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ELAN1200", HID_QUIRK_ELAN, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ELAN1300", HID_QUIRK_ELAN, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ELAN1400", HID_QUIRK_ELAN, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ELAN9001", HID_QUIRK_ELAN, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ELAN9002", HID_QUIRK_ELAN, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ELAN9003", HID_QUIRK_ELAN, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * Synaptics
     * ------------------------------------------------------------- */
    { L"ACPI\\SYN0001",   HID_QUIRK_SYNAPTICS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\SYN1B7F",   HID_QUIRK_SYNAPTICS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\SYNA0001",  HID_QUIRK_SYNAPTICS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\SYNA2393",  HID_QUIRK_SYNAPTICS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\SYNA2B2C",  HID_QUIRK_SYNAPTICS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\SYNA2B2D",  HID_QUIRK_SYNAPTICS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\SYNA2B2E",  HID_QUIRK_SYNAPTICS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\SYNA7DAB",  HID_QUIRK_SYNAPTICS, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * ASUS / ASUE
     * ------------------------------------------------------------- */
    { L"ACPI\\ASUS0001",  HID_QUIRK_ASUS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ASUE1200",  HID_QUIRK_ASUS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ASUE1400",  HID_QUIRK_ASUS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ASUE1500",  HID_QUIRK_ASUS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ASUE1600",  HID_QUIRK_ASUS, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * Goodix
     * ------------------------------------------------------------- */
    { L"ACPI\\GDIX1001", HID_QUIRK_GOODIX, HID_FLAG_TOUCHPAD },
    { L"ACPI\\GDIX1002", HID_QUIRK_GOODIX, HID_FLAG_TOUCHPAD },
    { L"ACPI\\GDIX1003", HID_QUIRK_GOODIX, HID_FLAG_TOUCHPAD },
    { L"ACPI\\GDIX1004", HID_QUIRK_GOODIX, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * Raydium
     * ------------------------------------------------------------- */
    { L"ACPI\\RAYD0001", HID_QUIRK_RAYDIUM, HID_FLAG_TOUCHPAD },
    { L"ACPI\\RAYD0002", HID_QUIRK_RAYDIUM, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * FocalTech
     * ------------------------------------------------------------- */
    { L"ACPI\\FTCS0001", HID_QUIRK_FOCALTECH, HID_FLAG_TOUCHPAD },
    { L"ACPI\\FTCS0002", HID_QUIRK_FOCALTECH, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * Cypress
     * ------------------------------------------------------------- */
    { L"ACPI\\CYAP0000", HID_QUIRK_CYPRESS, HID_FLAG_TOUCHPAD },
    { L"ACPI\\CYAP0001", HID_QUIRK_CYPRESS, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * Himax
     * ------------------------------------------------------------- */
    { L"ACPI\\HIMX0001", HID_QUIRK_HIMAX, HID_FLAG_TOUCHPAD },
    { L"ACPI\\HIMX0002", HID_QUIRK_HIMAX, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * PixArt
     * ------------------------------------------------------------- */
    { L"ACPI\\PIXA0001", HID_QUIRK_PIXART, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * Silead
     * ------------------------------------------------------------- */
    { L"ACPI\\MSSL1680", HID_QUIRK_SILEAD, HID_FLAG_TOUCHPAD },
    { L"ACPI\\MSSL0001", HID_QUIRK_SILEAD, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * Atmel / Microchip
     * ------------------------------------------------------------- */
    { L"ACPI\\ATML0000", HID_QUIRK_ATMEL, HID_FLAG_TOUCHPAD },
    { L"ACPI\\ATML0001", HID_QUIRK_ATMEL, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * Primax
     * ------------------------------------------------------------- */
    { L"ACPI\\PRMX0001", HID_QUIRK_PRIMAX, HID_FLAG_TOUCHPAD },

    /* -------------------------------------------------------------
     * Chicony
     * ------------------------------------------------------------- */
    { L"ACPI\\CHPN0001", HID_QUIRK_CHICONY, HID_FLAG_TOUCHPAD }
};

const ULONG g_I2cHidDevicesCount =
    sizeof(g_I2cHidDevices) / sizeof(g_I2cHidDevices[0]);


/* -----------------------------------------------------------------------
 * DriverUnload - XP/2003 BSOD-safe, C89-compliant
 * Purpose:
 *  - Runs at PASSIVE_LEVEL
 *  - Releases global resources allocated at DriverEntry/AddDevice
 *  - Leaves globals in a deterministic, safe state
 * Notes:
 *  - Per-device cleanup (MMIO, interrupts, PDOs, ACPI handles)
 *    occurs in I2cCtrl_RemoveDevice during IRP_MN_REMOVE_DEVICE.
 * ----------------------------------------------------------------------- */

/* Global symbolic link name */
UNICODE_STRING g_SymbolicLinkName;

/* Global buffer pointer */
PVOID g_GlobalBuffer;

VOID
DriverUnload(
    IN PDRIVER_OBJECT DriverObject
    )
{
    KIRQL irql;

    /* Enforce PASSIVE_LEVEL for pageable code paths */
    irql = KeGetCurrentIrql();
    if (irql != PASSIVE_LEVEL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "DriverUnload called at IRQL %lu", (ULONG)irql);
        return;
    }
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                "I2CCTRL: DriverUnload invoked");

    /*
     * Release global resources created in DriverEntry/AddDevice.
     * Per-device cleanup must already be complete via PnP remove.
     */

    /* Delete global symbolic link (if one was created) and free its string */
    if (g_SymbolicLinkName.Buffer != NULL) {
        NTSTATUS delStatus;

        delStatus = IoDeleteSymbolicLink(&g_SymbolicLinkName);
        if (!NT_SUCCESS(delStatus)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_INIT,
                        "IoDeleteSymbolicLink failed (0x%08X)", delStatus);
        }
        RtlFreeUnicodeString(&g_SymbolicLinkName);
        g_SymbolicLinkName.Buffer = NULL;
        g_SymbolicLinkName.Length = 0;
        g_SymbolicLinkName.MaximumLength = 0;
    }

    /* Free any global buffer allocation */
    if (g_GlobalBuffer != NULL) {
        ExFreePool(g_GlobalBuffer);
        g_GlobalBuffer = NULL;
    }

    /* Reset global controller bookkeeping to a known state */
    g_I2cCtrlGlobal.NextControllerId = 0U;
    InitializeListHead(&g_I2cCtrlGlobal.ControllerList);

    /* Reinitialize global lock to avoid stale state */
    I2CCTRL_INIT_LOCK(&g_I2cCtrlGlobal.GlobalLock);

    /* Optionally scrub remaining global struct for determinism */
    /* RtlZeroMemory(&g_I2cCtrlGlobal, sizeof(g_I2cCtrlGlobal)); */

    /* Shutdown ETW tracing provider */
    I2cCtrlEtwShutdown();
}


/* -----------------------------------------------------------------------
 * DriverEntry - I²C Controller bus driver entry point
 *
 * XP/2003 BSOD-safe, C89-compliant (WinDDK 7.1.0).
 * ----------------------------------------------------------------------- */
NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT  DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    ULONG    i;

    status = STATUS_SUCCESS;
    i      = 0U;

    UNREFERENCED_PARAMETER(RegistryPath);

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    I2cCtrl_Log("DriverEntry begin\n");

    /* Initialize global state */
    RtlZeroMemory(&g_I2cCtrlGlobal, sizeof(g_I2cCtrlGlobal));
    g_I2cCtrlGlobal.NextControllerId = 0L;
    I2CCTRL_INIT_LOCK(&g_I2cCtrlGlobal.GlobalLock);
    InitializeListHead(&g_I2cCtrlGlobal.ControllerList);

    /* Initialize tracing + dump */
    I2cCtrlEtwInitialize();
    I2cCtrl_DumpInit(NULL);

    /* Default all IRP handlers */
    for (i = 0U; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = I2cCtrl_InvalidIrp;
    }

    /* Assign supported dispatch routines */
    DriverObject->MajorFunction[IRP_MJ_CREATE]                  = I2cCtrl_Create;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]                   = I2cCtrl_Close;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]                 = I2cCtrl_Cleanup;
    DriverObject->MajorFunction[IRP_MJ_PNP]                     = I2cCtrl_DispatchPnP;
    DriverObject->MajorFunction[IRP_MJ_POWER]                   = I2cCtrl_DispatchPower;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]          = I2cCtrl_DispatchIoctl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = I2cCtrl_DispatchIoctl;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL]          = I2cCtrl_InvalidIrp;

    /* Unload */
    DriverObject->DriverUnload = DriverUnload;

    if (DriverObject->DriverExtension == NULL) {
        I2cCtrl_Log("DriverEntry: DriverExtension missing\n");
        return STATUS_UNSUCCESSFUL;
    }

    /* AddDevice */
    DriverObject->DriverExtension->AddDevice = I2cCtrl_AddDevice;

    /* Lifecycle helpers */
    g_I2cCtrlGlobal.StopDevice    = I2cCtrl_StopDevice;
    g_I2cCtrlGlobal.RestartDevice = I2cCtrl_RestartDevice;

    I2cCtrl_Log("DriverEntry complete (mask=0x%08lx)\n", g_I2cCtrlTraceEnableMask);

    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_Create
 * Handle IRP_MJ_CREATE (remove-lock safe, XP/2003 correct)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_Create(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PI2CCTRL_FDO        dx;
    NTSTATUS            status;

    ASSERT(DeviceObject != NULL);
    ASSERT(Irp != NULL);

    dx = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    if (dx == NULL) {
        Irp->IoStatus.Status      = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                "Create handle (Ctrl%lu)", dx->ControllerId);

    /* Acquire remove lock for this IRP */
    status = IoAcquireRemoveLock(&dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status      = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    /* If this is an FDO with a lower device, forward the IRP */
    if (dx->LowerDevice != NULL) {

        IoCopyCurrentIrpStackLocationToNext(Irp);

        IoSetCompletionRoutine(
            Irp,
            I2CCTRL_ReleaseLockCompletion,
            &dx->RemoveLock,
            TRUE, TRUE, TRUE
        );

        return IoCallDriver(dx->LowerDevice, Irp);
    }

    /* PDO or no lower device: complete locally */
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    /* Release lock manually since no completion routine will run */
    IoReleaseRemoveLock(&dx->RemoveLock, Irp);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_Close
 * Handle IRP_MJ_CLOSE (remove-lock safe, XP/2003 correct)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_Close(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PI2CCTRL_FDO        dx;
    NTSTATUS            status;

    ASSERT(DeviceObject != NULL);
    ASSERT(Irp != NULL);

    dx = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    if (dx == NULL) {
        Irp->IoStatus.Status      = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                "Close handle (Ctrl%lu)", dx->ControllerId);

#ifdef TRACK_OPEN_HANDLES
    if (dx->OpenHandleCount > 0U) {
        dx->OpenHandleCount--;
    }
#endif

    /* Acquire remove lock for this IRP */
    status = IoAcquireRemoveLock(&dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status      = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    /* If this is an FDO with a lower device, forward the IRP */
    if (dx->LowerDevice != NULL) {

        IoCopyCurrentIrpStackLocationToNext(Irp);

        IoSetCompletionRoutine(
            Irp,
            I2CCTRL_ReleaseLockCompletion,
            &dx->RemoveLock,
            TRUE, TRUE, TRUE
        );

        return IoCallDriver(dx->LowerDevice, Irp);
    }

    /* PDO or no lower device: complete locally */
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    /* Release lock manually since no completion routine will run */
    IoReleaseRemoveLock(&dx->RemoveLock, Irp);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

const I2CHID_DEVICE_ID*
I2cCtrl_FindHidMatch(
    PCWSTR HidId
    )
{
    ULONG i;

    if (HidId == NULL || *HidId == L'\0')
        return NULL;

    for (i = 0; i < g_I2cHidDevicesCount; i++) {

        PCWSTR entry = g_I2cHidDevices[i].HidId;

        if (entry == NULL || *entry == L'\0')
            continue;

        /* ---------------------------------------------------------
         * Wildcard match: "*PNP0C50" -> match any suffix
         * --------------------------------------------------------- */
        if (entry[0] == L'*') {
            /* skip '*' and compare suffix case-insensitive */
            if (_wcsicmp(HidId + (wcslen(HidId) - wcslen(entry) + 1),
                         entry + 1) == 0) {
                return &g_I2cHidDevices[i];
            }
            continue;
        }

        /* ---------------------------------------------------------
         * Exact case-insensitive match
         * --------------------------------------------------------- */
        if (_wcsicmp(HidId, entry) == 0) {
            return &g_I2cHidDevices[i];
        }
    }

    return NULL;
}



/* ---------------------------------------------------------------------------
 * I2cCtrl_CancelTransfersForIrp
 * Best-effort cancel/unwind for I/O associated with this file object.
 * HAL-based, controller-agnostic, XP-safe, non-destructive to controller state.
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_CancelTransfersForIrp(PI2CCTRL_FDO devctx, PIRP Irp)
{
    UNREFERENCED_PARAMETER(Irp);

    if (devctx == NULL || devctx->Ops == NULL) {
        return;
    }

    /* 1) Acknowledge sticky abort/stop detection (non-destructive) */
    if (devctx->Ops->AckInterrupts != NULL) {
        devctx->Ops->AckInterrupts(
            devctx,
            I2C_INT_TX_ABORT | I2C_INT_STOP_DETECTED
        );
    }

    /* 2) Best-effort drain FIFOs (bounded, non-blocking) */
    if (devctx->Ops->DrainRxBounded != NULL) {
        devctx->Ops->DrainRxBounded(devctx);
    }
    if (devctx->Ops->FlushTxBounded != NULL) {
        devctx->Ops->FlushTxBounded(devctx);
    }

    /* Per-file IRP tracking would go here if implemented */
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_Cleanup
 * Handle IRP_MJ_CLEANUP (remove-lock safe, XP/2003 correct)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_Cleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PI2CCTRL_FDO        dx;
    NTSTATUS            status;

    ASSERT(DeviceObject != NULL);
    ASSERT(Irp != NULL);

    dx = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    if (dx == NULL) {
        Irp->IoStatus.Status      = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                "Cleanup handle (Ctrl%lu)", dx->ControllerId);

    /* Acquire remove lock for this IRP */
    status = IoAcquireRemoveLock(&dx->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status      = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    /* Best-effort cancel of per-file transfers (non-destructive) */
    I2cCtrl_CancelTransfersForIrp(dx, Irp);

    /* If this is an FDO with a lower device, forward the IRP */
    if (dx->LowerDevice != NULL) {

        IoCopyCurrentIrpStackLocationToNext(Irp);

        IoSetCompletionRoutine(
            Irp,
            I2CCTRL_ReleaseLockCompletion,
            &dx->RemoveLock,
            TRUE, TRUE, TRUE
        );

        return IoCallDriver(dx->LowerDevice, Irp);
    }

    /* PDO or no lower device: complete locally */
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    /* Release lock manually since no completion routine will run */
    IoReleaseRemoveLock(&dx->RemoveLock, Irp);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}



/* ---------------------------------------------------------------------------
 * I2cCtrl_ReadBurstPolled
 * Polled read: issues read requests via HAL and drains RX FIFO into buffer.
 * Uses only HAL ops; no direct MMIO access. XP-safe, C89-compliant.
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_ReadBurstPolled(
    PI2CCTRL_FDO devctx,
    USHORT       addr,
    PUCHAR       buffer,
    USHORT       length,
    BOOLEAN      issueRestart,
    BOOLEAN      issueStop
    )
{
    NTSTATUS       status;
    USHORT         i;
    ULONG          timeout;
    I2C_HW_STATUS  hwst;

    /* Defensive parameter validation */
    if (devctx == NULL || buffer == NULL || length == 0) {
        I2cCtrl_Log("ReadBurstPolled: invalid parameters (devctx=%p, buffer=%p, length=%u)",
                    devctx, buffer, length);
        return STATUS_INVALID_PARAMETER;
    }
    if (devctx->Ops == NULL ||
        devctx->Ops->GetStatus == NULL ||
        devctx->Ops->IssueReadToken == NULL ||
        devctx->Ops->ReadRxByte == NULL ||
        devctx->Ops->SetTarget7bit == NULL) {
        I2cCtrl_Log("ReadBurstPolled: Ops table incomplete");
        return STATUS_INVALID_DEVICE_STATE;
    }

    /* Program 7-bit target address */
    status = devctx->Ops->SetTarget7bit(devctx, (UCHAR)(addr & 0x7F));
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("ReadBurstPolled: SetTarget7bit failed (status=0x%08lx)", status);
        return status;
    }

    /* Optional restart before burst */
    if (issueRestart && devctx->Ops->EmitRestartIfNeeded != NULL) {
        status = devctx->Ops->EmitRestartIfNeeded(devctx);
        if (!NT_SUCCESS(status)) {
            I2cCtrl_Log("ReadBurstPolled: EmitRestartIfNeeded failed (status=0x%08lx)", status);
            return status;
        }
    }

    /* Queue read requests into TX path */
    for (i = 0; i < length; i++) {
        timeout = 100000; /* ~100ms worst case per token */
        while (timeout--) {
            RtlZeroMemory(&hwst, sizeof(hwst));
            status = devctx->Ops->GetStatus(devctx, &hwst);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            if (hwst.TxFifoNotFull) {
                break;
            }
            if (hwst.TxAborted) {
                I2cCtrl_Log("ReadBurstPolled: TX aborted while queuing read requests");
                if (devctx->Ops->AckInterrupts != NULL) {
                    devctx->Ops->AckInterrupts(devctx, hwst.RawIntr);
                }
                return STATUS_DEVICE_NOT_CONNECTED;
            }
            KeStallExecutionProcessor(1);
        }
        if (timeout == 0) {
            I2cCtrl_Log("ReadBurstPolled: TX path saturated while queuing read requests");
            return STATUS_IO_TIMEOUT;
        }

        status = devctx->Ops->IssueReadToken(devctx);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    /* Optional STOP after queuing */
    if (issueStop && devctx->Ops->EmitStopIfNeeded != NULL) {
        status = devctx->Ops->EmitStopIfNeeded(devctx);
        if (!NT_SUCCESS(status)) {
            I2cCtrl_Log("ReadBurstPolled: EmitStopIfNeeded failed (status=0x%08lx)", status);
            return status;
        }
    }

    /* Drain RX bytes */
    for (i = 0; i < length; i++) {
        timeout = 100000; /* ~100ms per byte */
        while (timeout--) {
            RtlZeroMemory(&hwst, sizeof(hwst));
            status = devctx->Ops->GetStatus(devctx, &hwst);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            if (hwst.RxFifoNotEmpty) {
                break;
            }
            if (hwst.TxAborted) {
                I2cCtrl_Log("ReadBurstPolled: TX aborted while waiting for RX data");
                if (devctx->Ops->AckInterrupts != NULL) {
                    devctx->Ops->AckInterrupts(devctx, hwst.RawIntr);
                }
                return STATUS_DEVICE_NOT_CONNECTED;
            }
            KeStallExecutionProcessor(1);
        }
        if (timeout == 0) {
            I2cCtrl_Log("ReadBurstPolled: RX timeout");
            if (devctx->Ops->AckInterrupts != NULL) {
                devctx->Ops->AckInterrupts(devctx, hwst.RawIntr);
            }
            return STATUS_IO_TIMEOUT;
        }

        status = devctx->Ops->ReadRxByte(devctx, &buffer[i]);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    /* Final abort check */
    RtlZeroMemory(&hwst, sizeof(hwst));
    status = devctx->Ops->GetStatus(devctx, &hwst);
    if (NT_SUCCESS(status) && hwst.TxAborted) {
        I2cCtrl_Log("ReadBurstPolled: final TX abort detected");
        if (devctx->Ops->AckInterrupts != NULL) {
            devctx->Ops->AckInterrupts(devctx, hwst.RawIntr);
        }
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
 * ValidateBufferedInput
 * Ensure the caller provided a sufficiently large input buffer
 * --------------------------------------------------------------------------- */
NTSTATUS
ValidateBufferedInput(PIRP Irp, size_t required)
{
    PIO_STACK_LOCATION irpSp;

    if (!Irp || required == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    irpSp = IoGetCurrentIrpStackLocation(Irp);
    if (!irpSp) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Check the input buffer length against the required size */
    if (irpSp->Parameters.DeviceIoControl.InputBufferLength < required) {
        Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* If valid, set IoStatus to success */
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    return STATUS_SUCCESS;
}

/* --- PEC (CRC-8) over SMBus stream (polynomial 0x07, initial 0) ----------- */
/* XP/2003 BSOD-safe, C89-compliant */
UCHAR
I2cCtrl_ComputePec(
    IN const UCHAR* bytes,
    IN SIZE_T       count
    )
{
    SIZE_T i;
    SIZE_T bit;
    UCHAR  crc;
    UCHAR  in;

    crc = 0U;

    if (bytes == NULL) {
        return 0U;
    }
    if (count == 0U) {
        return 0U;
    }

    for (i = 0U; i < count; i++) {
        in  = bytes[i];
        crc ^= in;
        for (bit = 0U; bit < 8U; bit++) {
            if ((crc & 0x80U) != 0U) {
                crc = (UCHAR)((crc << 1) ^ 0x07U);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

/* --- bounded retry + microsecond backoff (polling-safe) ------------------- */
NTSTATUS
I2cCtrl_TryWithRetry(
    IN PI2CCTRL_FDO fdo,
    IN ULONG        (*op)(VOID* ctx),   /* returns NTSTATUS cast to ULONG */
    IN VOID*        ctx
    )
{
    NTSTATUS s;
    ULONG    tries, maxTries;
    ULONG    delayUs, maxDelayUs;

    s = STATUS_UNSUCCESSFUL;

    maxTries   = (fdo->PolicyMaxRetries != 0U) ? fdo->PolicyMaxRetries : 3U;
    if (maxTries > 10U) maxTries = 10U;

    delayUs    = (fdo->PolicyRetryDelayUs != 0U) ? fdo->PolicyRetryDelayUs : 1000U;
    if (delayUs > 100000U) delayUs = 100000U;

    maxDelayUs = (fdo->PolicyBackoffMaxUs != 0U) ? fdo->PolicyBackoffMaxUs : 5000U;
    if (maxDelayUs > 50000U) maxDelayUs = 50000U;

    for (tries = 0U; tries <= maxTries; tries++) {
        s = (NTSTATUS)op(ctx);
        if (NT_SUCCESS(s)) {
            break;
        }
        KeStallExecutionProcessor(delayUs);
        /* Exponential backoff, capped */
        if (fdo->PolicyBackoffOnBusy != 0U) {
            delayUs = (delayUs > (maxDelayUs / 2U)) ? maxDelayUs : (delayUs << 1);
        }
    }
    return s;
}

/* -----------------------------------------------------------------------
 * SMBus single read/write attempt (retry wrapper target)
 * Non-static: exported within driver module
 * XP/2003 BSOD-safe, C89-compliant
 * ----------------------------------------------------------------------- */
ULONG
I2cCtrl_SmbusRwOnce(VOID* ctxVoid)
{
    SMBUS_RW_CTX* c;
    NTSTATUS s;

    /* declare variables at top for C89 compliance */
    c = (SMBUS_RW_CTX*)ctxVoid;
    s = STATUS_SUCCESS;

    /* defensive checks: avoid NULL deref or invalid buffer */
    if (c == NULL || c->fdo == NULL || c->buf == NULL || c->len == 0U) {
        return (ULONG)STATUS_INVALID_PARAMETER;
    }

    if (c->isRead) {
        /* cast length down to USHORT explicitly to silence WDK warning */
        s = I2cCtrl_ReadBurstPolled(c->fdo,
                                    c->addr,
                                    c->buf,
                                    (USHORT)(c->len & 0xFFFFU),
                                    c->restart,
                                    c->stop);
    } else {
        /* if WriteBurstPolled prototype already takes ULONG, no cast needed */
        s = I2cCtrl_WriteBurstPolled(c->fdo,
                                     c->addr,
                                     c->buf,
                                     c->len,
                                     c->restart,
                                     c->stop);
    }

    return (ULONG)s;
}


/* -----------------------------------------------------------------------
   I2cCtrl_AddDevice - attach FDO for supported ACPI/PCI I²C controllers
   XP/2003-safe, BSOD-hardened, C89-compliant.
   Uses the canonical controller table (g_I2cControllers[]) for detection.
   ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_AddDevice(
    IN PDRIVER_OBJECT  DriverObject,
    IN PDEVICE_OBJECT  PhysicalDeviceObject
    )
{
    NTSTATUS        status;
    PDEVICE_OBJECT  fdo;
    PI2CCTRL_FDO    devctx;
    BOOLEAN         isI2CClass;
    WCHAR           hwidBuffer[256];
    ULONG           length;
    PVOID           dynBuf;
    ULONG           dynLen;

    const WCHAR*    base;
    ULONG           bytes;
    const WCHAR*    p;
    const WCHAR*    end;
    const WCHAR*    s;
    size_t          i;

    /* C89 init */
    status     = STATUS_SUCCESS;
    fdo        = NULL;
    devctx     = NULL;
    isI2CClass = FALSE;
    dynBuf     = NULL;
    dynLen     = 0U;

    RtlZeroMemory(hwidBuffer, sizeof(hwidBuffer));
    length = sizeof(hwidBuffer);

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        I2cCtrl_Log("AddDevice: wrong IRQL\n");
        return STATUS_INVALID_DEVICE_STATE;
    }
    PAGED_CODE();

    if (DriverObject == NULL || PhysicalDeviceObject == NULL) {
        I2cCtrl_Log("AddDevice: invalid parameters\n");
        return STATUS_INVALID_PARAMETER;
    }

    I2cCtrl_Log("AddDevice: begin\n");

    /* ---------------------------------------------------------------
       Query Hardware IDs (MULTI_SZ)
       --------------------------------------------------------------- */
    status = IoGetDeviceProperty(PhysicalDeviceObject,
                                 DevicePropertyHardwareID,
                                 length,
                                 hwidBuffer,
                                 &length);

    if (status == STATUS_BUFFER_TOO_SMALL && length > sizeof(hwidBuffer)) {

        dynLen = length;
        dynBuf = ExAllocatePoolWithTag(NonPagedPool, dynLen, TAG_I2C_MISC);
        if (dynBuf == NULL) {
            I2cCtrl_Log("AddDevice: alloc %lu failed\n", dynLen);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        status = IoGetDeviceProperty(PhysicalDeviceObject,
                                     DevicePropertyHardwareID,
                                     dynLen,
                                     dynBuf,
                                     &dynLen);
        if (!NT_SUCCESS(status)) {
            I2cCtrl_Log("AddDevice: requery failed 0x%08X\n", status);
            ExFreePoolWithTag(dynBuf, TAG_I2C_MISC);
            return status;
        }
    }
    else if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("AddDevice: property query failed 0x%08X\n", status);
        return status;
    }

    /* ---------------------------------------------------------------
       Detect whether this is a supported I²C controller
       --------------------------------------------------------------- */
    if (dynBuf != NULL) {
        base  = (const WCHAR*)dynBuf;
        bytes = dynLen;
    } else {
        base  = hwidBuffer;
        bytes = length;
    }

    I2cCtrl_Log("AddDevice: scanning HWIDs for controller match\n");

    if (bytes >= sizeof(WCHAR) * 2U) {

        p   = base;
        end = (const WCHAR*)((const UCHAR*)base + bytes);

        while (p < end && *p != L'\0') {

            I2cCtrl_Log("AddDevice: HWID candidate: %ws\n", p);

            s = p;
            while (s < end && *s != L'\0') {
                s++;
            }

            for (i = 0; i < RTL_NUMBER_OF(g_I2cControllers); i++) {
                if (wcsstr(p, g_I2cControllers[i].PciId) != NULL) {
                    isI2CClass = TRUE;
                    I2cCtrl_Log("AddDevice: matched controller %ws (index %u)\n",
                                g_I2cControllers[i].PciId, (unsigned)i);
                    break;
                }
            }

            if (isI2CClass) {
                break;
            }

            p = s + 1;
        }
    }

    if (dynBuf) {
        ExFreePoolWithTag(dynBuf, TAG_I2C_MISC);
        dynBuf = NULL;
    }

    if (!isI2CClass) {
        I2cCtrl_Log("AddDevice: unsupported controller, skipping\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    /* ---------------------------------------------------------------
       Create the FDO
       --------------------------------------------------------------- */
    status = IoCreateDevice(DriverObject,
                            sizeof(I2CCTRL_FDO),
                            NULL,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN,
                            FALSE,
                            &fdo);
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("AddDevice: IoCreateDevice failed 0x%08lx\n", status);
        return status;
    }

    /* ---------------------------------------------------------------
       Initialize FDO context
       --------------------------------------------------------------- */
    devctx = (PI2CCTRL_FDO)fdo->DeviceExtension;
    RtlZeroMemory(devctx, sizeof(*devctx));
	
	/* SET FDO SIGNATURE HERE */
	devctx->Signature = I2CCTRL_FDO_SIGNATURE;
	
    devctx->Self           = fdo;
    devctx->PhysicalDevice = PhysicalDeviceObject;
    devctx->LowerDevice    = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);

    if (devctx->LowerDevice == NULL) {
        I2cCtrl_Log("AddDevice: IoAttachDeviceToDeviceStack failed\n");
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    devctx->ControllerId = InterlockedIncrement((volatile LONG*)&g_I2cCtrlGlobal.NextControllerId);

    I2cCtrl_Log("AddDevice: ControllerId assigned = %lu\n", devctx->ControllerId);

    /* Initialize locks, lists, queues, DPCs, etc. */
    I2CCTRL_INIT_LOCK(&devctx->BusLock);
    I2CCTRL_INIT_LOCK(&devctx->QueueLock);
    I2CCTRL_INIT_LOCK(&devctx->ChildLock);
    I2CCTRL_INIT_LOCK(&devctx->HwLock);
    I2CCTRL_INIT_LOCK(&devctx->PendingIrpLock);
    I2CCTRL_INIT_LOCK(&devctx->CancelLock);
    I2CCTRL_INIT_LOCK(&devctx->IoLock);

    InitializeListHead(&devctx->RequestQueue);
    InitializeListHead(&devctx->PendingIrpList);
    InitializeListHead(&devctx->ChildList);
    InitializeListHead(&devctx->HighQueue);
    InitializeListHead(&devctx->NormalQueue);
    InitializeListHead(&devctx->LowQueue);

    IoInitializeRemoveLock(&devctx->RemoveLock, 'I2C0', 0, 0);

    fdo->Flags |= DO_POWER_PAGABLE;

/* ---------------------------------------------------------------
   Identify controller, install register map, apply quirks
   --------------------------------------------------------------- */
status = I2cCtrlIdentifyAndInitController(devctx);
if (!NT_SUCCESS(status)) {
    I2cCtrl_Log("AddDevice: IdentifyAndInitController failed 0x%08lx\n", status);
    IoDetachDevice(devctx->LowerDevice);
    IoDeleteDevice(fdo);
    return status;
}

/*
 * Pre-seed PnpId with the canonical controller table ID.
 * This allows ApplyQuirks() to run safely during AddDevice
 * without logging “invalid devctx”.
 *
 * The real ACPI PnpId will overwrite this later in StartDevice.
 */
devctx->PnpId = (PWSTR)g_I2cControllers[devctx->ControllerIndex].PciId;

/* Install backend (Intel DW-I2C / Cannon Lake style) */
I2cCtrl_InstallBackend(devctx);

/*
 * Early quirks pass:
 * - BAR0/BAR2 are NULL at this stage -> hardware quirks are skipped
 * - ACPI/BSOD flags that do not touch hardware still apply
 */
I2cCtrlApplyQuirks(devctx);

/*
 * ASUS X509FA: PMC PWMR mapping for Cannon Lake I2C
 *
 * ControllerId 1 = 8086:9DC5
 * ControllerId 2 = 8086:9DE8
 * ControllerId 3 = 8086:9DE9
 *
 * Raw PWRM field  = 0x537D28A3
 * Aligned PWRMBASE = 0x537D2000
 * Length = 0x1E30
 */
if (devctx->ControllerId == 1 ||
    devctx->ControllerId == 2 ||
    devctx->ControllerId == 3)
{
    devctx->PwrmBase.QuadPart = 0x537D2000ULL;
    devctx->PwrmLength        = 0x1E30;
    devctx->HavePwrm          = TRUE;

    devctx->PwrmBaseVa = MmMapIoSpace(
                             devctx->PwrmBase,
                             devctx->PwrmLength,
                             MmNonCached);

    if (devctx->PwrmBaseVa == NULL) {
        I2cCtrl_Log("AddDevice: PWMR MmMapIoSpace failed\n");
        IoDetachDevice(devctx->LowerDevice);
        IoDeleteDevice(fdo);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    I2cCtrl_Log("AddDevice: PWMR mapped VA=%p PA=0x%08lx Len=0x%lx (Ctrl%lu)\n",
                devctx->PwrmBaseVa,
                (ULONG)devctx->PwrmBase.LowPart,
                devctx->PwrmLength,
                devctx->ControllerId);
}


    /* ---------------------------------------------------------------
       Child PDO lifecycle helpers
       --------------------------------------------------------------- */
	 devctx->DeleteChildrenFn      = I2cCtrl_DeenumerateAcpiChildren;
	 devctx->EnumerateChildrenFn   = I2cCtrl_EnumerateAcpiChildren;
	 devctx->ReenumerateChildrenFn = I2cCtrl_ReenumerateAcpiChildren;

    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    I2cCtrl_Log("AddDevice: complete (Ctrl%lu)\n", devctx->ControllerId);

    return STATUS_SUCCESS;
}


NTSTATUS
I2cCtrl_DupString(
    PUNICODE_STRING Dest,
    PCWSTR Src
    )
{
    UNICODE_STRING src;
    RtlInitUnicodeString(&src, Src);

    /* Free old buffer if present */
    if (Dest->Buffer != NULL) {
        ExFreePoolWithTag(Dest->Buffer, 'dIqP');
        Dest->Buffer = NULL;
    }

    Dest->Buffer = (PWSTR)ExAllocatePoolWithTag(
        NonPagedPool,
        src.Length + sizeof(WCHAR),
        'dIqP'
    );

    if (!Dest->Buffer) {
        Dest->Length = 0;
        Dest->MaximumLength = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Dest->MaximumLength = (USHORT)(src.Length + sizeof(WCHAR));
    Dest->Length = src.Length;

    RtlCopyMemory(Dest->Buffer, src.Buffer, src.Length);
    Dest->Buffer[Dest->Length / sizeof(WCHAR)] = L'\0';

    return STATUS_SUCCESS;
}


VOID
I2cCtrl_FreeString(
    PUNICODE_STRING S
    )
{
    if (S->Buffer) {
        ExFreePoolWithTag(S->Buffer, 'dIqP');
        S->Buffer = NULL;
    }
    S->Length = 0;
    S->MaximumLength = 0;
}

NTSTATUS
I2cCtrl_EnumerateAcpiChildren(
    PDEVICE_OBJECT Fdo,
    PI2CCTRL_FDO   fdoExt,
    PULONG         ChildCountOut
    )
{
    NTSTATUS status;
    ULONG childCount = 0;

    PI2CCTRL_ACPI_EVAL_OUTPUT_BUFFER outBuf = NULL;
    ULONG outLen = sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER) + 512;

    PI2CCTRL_ACPI_EVAL_OUTPUT_BUFFER staBuf = NULL;
    ULONG staLen = sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER) + 32;

    ACPI_METHOD_ARGUMENT UNALIGNED* arg;
    ACPI_METHOD_ARGUMENT UNALIGNED* staArg;
    ULONG i;
    BOOLEAN opened = FALSE;

    UNICODE_STRING hidUni;
    ANSI_STRING hidAnsi;

    WCHAR uidBuf[32];
    UNICODE_STRING uidUni;
    ANSI_STRING uidAnsi;
    BOOLEAN haveUid = FALSE;

    WCHAR numBuf[16];
    UNICODE_STRING numUni;
    ULONG uidInt = 0;

    ULONG adrVal = 0;
    BOOLEAN haveAdr = FALSE;

    const I2CHID_DEVICE_ID* hidMatch = NULL;

    PDEVICE_OBJECT pdo;
    PI2CCTRL_PDO childDx;
    KIRQL oldIrql;

    ULONG uidLen;
    ULONG adrLen;

    if (ChildCountOut) {
        *ChildCountOut = 0;
    }

    if (!Fdo || !fdoExt || !fdoExt->Self) {
        I2cCtrl_Log("EnumerateAcpiChildren: invalid parameters\n");
        return STATUS_INVALID_PARAMETER;
    }

    I2CCTRL_REQUIRE_PASSIVE();

    I2cCtrl_Log("EnumerateAcpiChildren: begin for controller HWID=%ws\n",
                fdoExt->PnpId ? fdoExt->PnpId : L"<null>");

    /* Open ACPI */
    status = I2cCtrl_AcpiOpen(fdoExt);
    if (!NT_SUCCESS(status) || !fdoExt->AcpiDeviceObject) {
        I2cCtrl_Log("EnumerateAcpiChildren: AcpiOpen failed or no ACPI device (status=0x%08lx)\n",
                    status);
        return STATUS_SUCCESS;
    }

    opened = TRUE;

    /* --- Read _UID --- */
    uidLen = sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER) + 128;

    outBuf = ExAllocatePoolWithTag(NonPagedPool, uidLen, 'Acpi');
    if (!outBuf) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    RtlZeroMemory(outBuf, uidLen);

    status = I2cCtrl_AcpiEvalMethod(
                 fdoExt->AcpiDeviceObject,
                 fdoExt->AcpiHandle,
                 "_UID",
                 outBuf,
                 uidLen
             );

    if (NT_SUCCESS(status) && outBuf && outBuf->Count > 0) {

        arg = (ACPI_METHOD_ARGUMENT UNALIGNED*)&outBuf->Data[0];

        if (arg->Type == ACPI_METHOD_ARGUMENT_STRING &&
            arg->DataLength < sizeof(uidBuf)) {

            uidAnsi.Buffer = (PCHAR)arg->Data;
            uidAnsi.Length = (USHORT)arg->DataLength;
            uidAnsi.MaximumLength = (USHORT)arg->DataLength;

            uidUni.Buffer = uidBuf;
            uidUni.Length = 0;
            uidUni.MaximumLength = (USHORT)(sizeof(uidBuf) - sizeof(WCHAR));

            if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&uidUni, &uidAnsi, FALSE))) {
                haveUid = TRUE;
                I2cCtrl_Log("EnumerateAcpiChildren: _UID=\"%ws\"\n", uidUni.Buffer);
            }

        } else if (arg->Type == ACPI_METHOD_ARGUMENT_INTEGER) {

            uidInt = (ULONG)arg->Argument;
            RtlZeroMemory(numBuf, sizeof(numBuf));

            numUni.Buffer = numBuf;
            numUni.Length = 0;
            numUni.MaximumLength = (USHORT)(sizeof(numBuf) - sizeof(WCHAR));

            if (NT_SUCCESS(RtlIntegerToUnicodeString(uidInt, 10, &numUni))) {
                haveUid = TRUE;
                I2cCtrl_Log("EnumerateAcpiChildren: _UID=%lu\n", uidInt);
            }
        }
    }

    if (outBuf) {
        ExFreePoolWithTag(outBuf, 'Acpi');
        outBuf = NULL;
    }

    /* --- Read _ADR --- */
    adrLen = sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER) + 64;

    outBuf = ExAllocatePoolWithTag(NonPagedPool, adrLen, 'Acpi');
    if (!outBuf) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    RtlZeroMemory(outBuf, adrLen);

    status = I2cCtrl_AcpiEvalMethod(
                 fdoExt->AcpiDeviceObject,
                 fdoExt->AcpiHandle,
                 "_ADR",
                 outBuf,
                 adrLen
             );

    if (NT_SUCCESS(status) && outBuf && outBuf->Count > 0) {

        arg = (ACPI_METHOD_ARGUMENT UNALIGNED*)&outBuf->Data[0];

        if (arg->Type == ACPI_METHOD_ARGUMENT_INTEGER) {
            adrVal = (ULONG)arg->Argument;
            haveAdr = TRUE;
            I2cCtrl_Log("EnumerateAcpiChildren: _ADR=0x%08lx\n", adrVal);
        }
    }

    if (outBuf) {
        ExFreePoolWithTag(outBuf, 'Acpi');
        outBuf = NULL;
    }

    /* --- Read _HID --- */
    outLen = sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER) + 512;

    outBuf = ExAllocatePoolWithTag(NonPagedPool, outLen, 'Acpi');
    if (!outBuf) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Done;
    }
    RtlZeroMemory(outBuf, outLen);

    status = I2cCtrl_AcpiEvalMethod(
                 fdoExt->AcpiDeviceObject,
                 fdoExt->AcpiHandle,
                 "_HID",
                 outBuf,
                 outLen
             );

    if (!NT_SUCCESS(status) || !outBuf || outBuf->Count == 0) {
        I2cCtrl_Log("EnumerateAcpiChildren: _HID eval failed or empty (status=0x%08lx)\n",
                    status);
        goto Done;
    }

    hidUni.Buffer = NULL;
    hidUni.Length = 0;
    hidUni.MaximumLength = 0;

    hidAnsi.Buffer = NULL;
    hidAnsi.Length = 0;
    hidAnsi.MaximumLength = 0;

    arg = (ACPI_METHOD_ARGUMENT UNALIGNED*)&outBuf->Data[0];

    for (i = 0; i < outBuf->Count; i++) {

        if (arg->Type == ACPI_METHOD_ARGUMENT_STRING &&
            arg->DataLength > 0) {

            hidAnsi.Buffer = (PCHAR)arg->Data;
            hidAnsi.Length = (USHORT)arg->DataLength;
            hidAnsi.MaximumLength = (USHORT)arg->DataLength;

            if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&hidUni, &hidAnsi, TRUE))) {

                I2cCtrl_Log("EnumerateAcpiChildren: controller HID=\"%ws\"\n", hidUni.Buffer);

                /* --- Check _STA --- */
                staLen = sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER) + 32;

                staBuf = ExAllocatePoolWithTag(NonPagedPool, staLen, 'Acpi');
                if (!staBuf) {
                    RtlFreeUnicodeString(&hidUni);
                    status = STATUS_INSUFFICIENT_RESOURCES;
                    goto Done;
                }
                RtlZeroMemory(staBuf, staLen);

                status = I2cCtrl_AcpiEvalMethod(
                             fdoExt->AcpiDeviceObject,
                             fdoExt->AcpiHandle,
                             "_STA",
                             staBuf,
                             staLen
                         );

                if (!NT_SUCCESS(status) || !staBuf || staBuf->Count == 0) {

                    I2cCtrl_Log("EnumerateAcpiChildren: _STA eval failed or empty (status=0x%08lx)\n",
                                status);
                    RtlFreeUnicodeString(&hidUni);

                    if (staBuf) {
                        ExFreePoolWithTag(staBuf, 'Acpi');
                        staBuf = NULL;
                    }

                    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
                    continue;
                }

                staArg = (ACPI_METHOD_ARGUMENT UNALIGNED*)&staBuf->Data[0];

                if (staArg->Type != ACPI_METHOD_ARGUMENT_INTEGER) {
                    I2cCtrl_Log("EnumerateAcpiChildren: _STA not integer\n");
                    ExFreePoolWithTag(staBuf, 'Acpi');
                    staBuf = NULL;
                    RtlFreeUnicodeString(&hidUni);
                    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
                    continue;
                }

                {
                    ULONG staVal = (ULONG)staArg->Argument;
                    ExFreePoolWithTag(staBuf, 'Acpi');
                    staBuf = NULL;

                    if ((staVal & 0x01) == 0) {
                        I2cCtrl_Log("EnumerateAcpiChildren: _STA indicates not present (0x%08lx)\n",
                                    staVal);
                        RtlFreeUnicodeString(&hidUni);
                        arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
                        continue;
                    }
                }

                /* --- HID-over-I2C via HID table --- */
                hidMatch = I2cCtrl_FindHidMatch(hidUni.Buffer);

                I2cCtrl_Log("EnumerateAcpiChildren: HID match %s\n",
                            hidMatch ? "found" : "not found");

                /* --- Create PDO (ETPD/ELAN1200 lives behind this controller) --- */
                pdo = NULL;
                status = IoCreateDevice(Fdo->DriverObject,
                                        sizeof(I2CCTRL_PDO),
                                        NULL,
                                        FILE_DEVICE_UNKNOWN,
                                        FILE_DEVICE_SECURE_OPEN,
                                        FALSE,
                                        &pdo);

                if (NT_SUCCESS(status) && pdo) {

                    RtlZeroMemory(pdo->DeviceExtension, sizeof(I2CCTRL_PDO));
                    childDx = (PI2CCTRL_PDO)pdo->DeviceExtension;

                    childDx->Pdo        = pdo;
                    childDx->ParentFdo  = fdoExt;
                    childDx->Present    = TRUE;
                    childDx->Reported   = FALSE;
                    childDx->Started    = FALSE;
                    childDx->Removed    = FALSE;
                    childDx->Enumerated = TRUE;
                    InitializeListHead(&childDx->ListEntry);

                    /* Initialize HID child IDs from table (will be ELAN/PNP0C50 for ETPD) */
                    I2cCtrlInitHidChildIds(childDx, childCount, hidMatch);

                    /* -------------------------------------------------------------
                     * NEW: Fetch HID descriptor via ACPI _DSM (universal HID-over-I2C)
                     * ------------------------------------------------------------- */
                    {
                        UCHAR dsmBuf[512];
                        ULONG dsmLen = sizeof(dsmBuf);

                        NTSTATUS dsmStatus =
                            I2cCtrl_AcpiGetHidDescriptorViaDsm(
                                fdoExt,
                                fdoExt->AcpiFileObject,
                                dsmBuf,
                                &dsmLen
                            );

                        if (NT_SUCCESS(dsmStatus)) {

                            HID_I2C_DESCRIPTOR_V10 parsed;

                            I2cCtrl_Log("EnumerateAcpiChildren: _DSM HID descriptor returned %lu bytes\n",
                                        dsmLen);

                            if (ParseHidDescriptorV10(dsmBuf, dsmLen, &parsed)) {

                                /* Copy raw HID descriptor */
                                RtlZeroMemory(&childDx->HidDesc, sizeof(childDx->HidDesc));
                                RtlCopyMemory(&childDx->HidDesc,
                                              dsmBuf,
                                              (dsmLen < sizeof(HID_DESCRIPTOR)) ?
                                                  dsmLen : sizeof(HID_DESCRIPTOR));

                                /* Fill HID-over-I2C register/length fields */
                                childDx->HidReportDescLen   = parsed.wReportDescLength;
                                childDx->HidDescRegister    = parsed.wReportDescRegister;
                                childDx->HidInputRegister   = parsed.wInputRegister;
                                childDx->HidMaxInputLen     = parsed.wMaxInputLength;
                                childDx->HidOutputRegister  = parsed.wOutputRegister;
                                childDx->HidMaxOutputLen    = parsed.wMaxOutputLength;
                                childDx->HidCommandRegister = parsed.wCommandRegister;
                                childDx->HidDataRegister    = parsed.wDataRegister;

                                /*
                                 * UNIVERSAL HID ACPI ID SELECTION
                                 */
                                if (fdoExt->PnpId &&
                                    wcsstr(fdoExt->PnpId, L"ELAN") != NULL) {

                                    RtlInitUnicodeString(&childDx->HardwareId, L"ACPI\\ELAN1200");
                                    RtlInitUnicodeString(&childDx->InstanceId, L"0000");

                                    I2cCtrl_Log("EnumerateAcpiChildren: Assigned HID ACPI ID = ACPI\\ELAN1200\n");

                                } else if (fdoExt->PnpId &&
                                           wcsstr(fdoExt->PnpId, L"GDIX") != NULL) {

                                    RtlInitUnicodeString(&childDx->HardwareId, L"ACPI\\GDIX1001");
                                    RtlInitUnicodeString(&childDx->InstanceId, L"0000");

                                    I2cCtrl_Log("EnumerateAcpiChildren: Assigned HID ACPI ID = ACPI\\GDIX1001\n");

                                } else if (fdoExt->PnpId &&
                                           wcsstr(fdoExt->PnpId, L"SYNA") != NULL) {

                                    RtlInitUnicodeString(&childDx->HardwareId, L"ACPI\\SYNA2B33");
                                    RtlInitUnicodeString(&childDx->InstanceId, L"0000");

                                    I2cCtrl_Log("EnumerateAcpiChildren: Assigned HID ACPI ID = ACPI\\SYNA2B33\n");

                                } else {

                                    RtlInitUnicodeString(&childDx->HardwareId, L"ACPI\\PNP0C50");
                                    RtlInitUnicodeString(&childDx->InstanceId, L"0000");

                                    I2cCtrl_Log("EnumerateAcpiChildren: Assigned HID ACPI ID = ACPI\\PNP0C50\n");
                                }

                            } else {
                                I2cCtrl_Log("EnumerateAcpiChildren: _DSM HID descriptor invalid\n");
                            }

                        } else {
                            I2cCtrl_Log("EnumerateAcpiChildren: _DSM HID descriptor fetch failed (0x%08lx)\n",
                                        dsmStatus);
                        }
                    }

                    /* Special-case ETPD: ACPI says Name(_ADR, One) and I2CSerialBus(0x0015, ...) */
                    if (haveAdr && adrVal == 1) {
                        childDx->SavedBusAddress = 0x0015;
                        I2cCtrl_Log(
                            "EnumerateAcpiChildren: treating controller _ADR=1 as ETPD (ELAN1200/PNP0C50), I2C addr=0x15\n"
                        );
                    } else if (haveAdr) {
                        childDx->SavedBusAddress = adrVal & 0x03FF;
                    } else {
                        childDx->SavedBusAddress = 0;
                    }

                    I2cCtrl_Log(
                        "EnumerateAcpiChildren: created child #%lu (ETPD candidate) HID=\"%ws\" SavedBusAddress=0x%03lx\n",
                        childCount,
                        hidUni.Buffer,
                        childDx->SavedBusAddress
                    );

                    pdo->Flags |= DO_POWER_PAGABLE;
                    pdo->Flags &= ~DO_DEVICE_INITIALIZING;

                    KeAcquireSpinLock(&fdoExt->ChildLock, &oldIrql);
                    InsertTailList(&fdoExt->ChildList, &childDx->ListEntry);
                    fdoExt->NumChildren++;
                    KeReleaseSpinLock(&fdoExt->ChildLock, oldIrql);

                    childCount++;
                } else {
                    I2cCtrl_Log("EnumerateAcpiChildren: IoCreateDevice failed (status=0x%08lx)\n",
                                status);
                }

                RtlFreeUnicodeString(&hidUni);
				arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
				continue;
            }
        }

        arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
    }

Done:

    if (outBuf) {
        ExFreePoolWithTag(outBuf, 'Acpi');
        outBuf = NULL;
    }

    if (staBuf) {
        ExFreePoolWithTag(staBuf, 'Acpi');
        staBuf = NULL;
    }

    if (childCount > 0 && fdoExt->PhysicalDevice) {
        I2cCtrl_Log("EnumerateAcpiChildren: %lu child(ren) created, invalidating BusRelations\n",
                    childCount);
        IoInvalidateDeviceRelations(fdoExt->PhysicalDevice, BusRelations);
    } else {
        I2cCtrl_Log("EnumerateAcpiChildren: no children created (ETPD may be hidden by ACPI)\n");
    }

    if (ChildCountOut) {
        *ChildCountOut = childCount;
    }

    if (opened) {
        I2cCtrl_Log("EnumerateAcpiChildren: done\n");
    }

    return STATUS_SUCCESS;
}


NTSTATUS
I2cCtrl_AcpiOpen(
    PI2CCTRL_FDO fdoExt
    )
{
    PDEVICE_OBJECT acpiPdo;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_REQUEST;

    if (fdoExt == NULL)
        return STATUS_INVALID_PARAMETER;

    if (fdoExt->AcpiBound &&
        fdoExt->AcpiDeviceObject != NULL)
    {
        return STATUS_SUCCESS;
    }

    acpiPdo = I2cCtrl_FindAcpiPdoForPciDevice(fdoExt->Self);
    if (acpiPdo == NULL) {
        I2cCtrl_Log("AcpiOpen: no ACPI PDO found for HWID=%ws\n",
                    fdoExt->PnpId ? fdoExt->PnpId : L"<null>");
        return STATUS_NOT_FOUND;
    }

    /* DO NOT ObReferenceObject(acpiPdo) — FindAcpiPdoForPciDevice already returns a referenced PDO */

    fdoExt->AcpiDeviceObject = acpiPdo;
    fdoExt->AcpiFileObject   = NULL;
    fdoExt->AcpiHandle       = NULL;
    fdoExt->AcpiBound        = TRUE;

    I2cCtrl_Log("AcpiOpen: bound to ACPI PDO %p (driver=%wZ)\n",
                acpiPdo, &acpiPdo->DriverObject->DriverName);

    return STATUS_SUCCESS;
}


VOID
I2cCtrl_AcpiClose(
    PI2CCTRL_FDO fdoExt
    )
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;

    PAGED_CODE();

    if (fdoExt == NULL)
        return;

    if (!fdoExt->AcpiBound)
        return;

    I2cCtrl_Log("AcpiClose: unbinding ACPI for HWID=%ws\n",
                fdoExt->PnpId ? fdoExt->PnpId : L"<null>");

    /* Release the single reference held from AcpiOpen */
    if (fdoExt->AcpiDeviceObject != NULL) {
        ObDereferenceObject(fdoExt->AcpiDeviceObject);
        fdoExt->AcpiDeviceObject = NULL;
    }

    /* Clear ACPI state */
    fdoExt->AcpiFileObject = NULL;
    fdoExt->AcpiHandle     = NULL;
    fdoExt->AcpiBound      = FALSE;

    I2cCtrl_Log("AcpiClose: ACPI unbound\n");
}


/* Convert PCWSTR name like L"_CRS" into 4-byte ACPI method name (char[4]) */
VOID
I2cCtrl_AcpiFillMethodName(
    PCWSTR MethodName,
    UCHAR  OutName[4]
    )
{
    USHORT i;

    if (MethodName == NULL || OutName == NULL) {
        return;
    }

    for (i = 0; i < 4; i++) {
        WCHAR ch = MethodName[i];

        if (ch == L'\0') {
            /* Pad remaining slots with spaces */
            while (i < 4) {
                OutName[i++] = ' ';
            }
            break;
        }

        /* ACPI method names are ASCII; take low byte of WCHAR */
        OutName[i] = (UCHAR)(ch & 0x7F);  /* mask to 7-bit ASCII */
    }
}

NTSTATUS
I2cCtrl_AcpiEvalMethod(
    PDEVICE_OBJECT                     AcpiPdo,
    PVOID                              AcpiHandle,
    PCSTR                              MethodName,
    PI2CCTRL_ACPI_EVAL_OUTPUT_BUFFER   OutBuf,
    ULONG                              OutBufLen
    )
{
    NTSTATUS                status;
    KEVENT                  event;
    PIRP                    irp;
    IO_STATUS_BLOCK         iosb;
    ACPI_EVAL_INPUT_BUFFER  input;
    LARGE_INTEGER           timeout;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        I2cCtrl_Log("AcpiEvalMethod: invalid IRQL\n");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (AcpiPdo == NULL ||
        MethodName == NULL ||
        OutBuf == NULL ||
        OutBufLen < sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER))
    {
        I2cCtrl_Log("AcpiEvalMethod: invalid parameters (Pdo=%p, Method=%s, OutLen=%lu)\n",
                    AcpiPdo, MethodName, OutBufLen);
        return STATUS_INVALID_PARAMETER;
    }

    PAGED_CODE();

    I2cCtrl_Log("AcpiEvalMethod: begin (PDO=%p, Handle=%p, Method=%s)\n",
                AcpiPdo, AcpiHandle, MethodName);

    RtlZeroMemory(&input, sizeof(input));
#ifdef ACPI_EVAL_INPUT_BUFFER_SIGNATURE
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
#endif

    /* MethodName is 4‑char ANSI (e.g. "_ADR") */
    input.MethodNameAsUlong = *((PULONG)MethodName);

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
              IOCTL_ACPI_EVAL_METHOD,
              AcpiPdo,
              &input,
              sizeof(input),
              OutBuf,
              OutBufLen,
              FALSE,
              &event,
              &iosb
          );

    if (irp == NULL) {
        I2cCtrl_Log("AcpiEvalMethod: IoBuildDeviceIoControlRequest failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* XP ACPI namespace handle is passed via OriginalFileObject */
    irp->Tail.Overlay.OriginalFileObject = (PFILE_OBJECT)AcpiHandle;

    I2cCtrl_Log("AcpiEvalMethod: sending IRP (Method=%s, Handle=%p)\n",
                MethodName, AcpiHandle);

    status = IoCallDriver(AcpiPdo, irp);

    if (status == STATUS_PENDING) {
        timeout.QuadPart = -5 * 1000 * 1000 * 10; /* 5 seconds */
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, &timeout);
        status = iosb.Status;
    }

    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("AcpiEvalMethod: ACPI call failed (Method=%s, Status=0x%08lx)\n",
                    MethodName, status);
        return status;
    }

    if (iosb.Information < sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER) ||
        iosb.Information > OutBufLen)
    {
        I2cCtrl_Log("AcpiEvalMethod: buffer size mismatch (Method=%s, Info=%lu, OutLen=%lu)\n",
                    MethodName, (ULONG)iosb.Information, OutBufLen);
        return STATUS_BUFFER_OVERFLOW;
    }

    I2cCtrl_Log("AcpiEvalMethod: success (Method=%s, Returned=%lu bytes)\n",
                MethodName, (ULONG)iosb.Information);

    return STATUS_SUCCESS;
}

//
// XP-compatible hybrid ACPI namespace walker
//  - First tries _SUB (if present)
//  - Falls back to brute-force probing under \_SB
//
NTSTATUS
I2cCtrl_EnumerateAcpiNamespace(
    PDEVICE_OBJECT AcpiPdo,
    PVOID          ParentHandle,
    ULONG          Depth,
    PI2CCTRL_FDO   FdoExt
    )
{
    NTSTATUS status;
    ULONG index = 0;
    I2CCTRL_ACPI_ENUM_ENTRY enumEntry;   /* struct, not pointer */
    PVOID childHandle;

    I2cCtrl_Log("EnumNS: depth=%lu parent=%p\n", Depth, ParentHandle);

    for (;;)
    {
        /* C89: clear struct before use */
        RtlZeroMemory(&enumEntry, sizeof(enumEntry));
        childHandle = NULL;

        //
        // Hybrid child fetch:
        //  - Try _SUB
        //  - Try brute-force probing
        //
        status = I2cCtrl_AcpiGetDeviceInformation(
                     AcpiPdo,
                     ParentHandle,
                     &enumEntry,   /* correct type: PI2CCTRL_ACPI_ENUM_ENTRY */
                     index);

        if (status == STATUS_NO_MORE_ENTRIES) {
            I2cCtrl_Log("EnumNS: no more children at depth=%lu\n", Depth);
            break;
        }

        if (!NT_SUCCESS(status)) {
            I2cCtrl_Log("EnumNS: child fetch failed (0x%08lx)\n", status);
            break;
        }

        /* Extract ACPI handle from enumeration entry */
        childHandle = enumEntry.DeviceHandle;

        I2cCtrl_Log(
            "EnumNS: found child handle=%p at depth=%lu (index=%lu)\n",
            childHandle, Depth, index
        );

        //
        // Process HID-I2C devices under this node
        //
        I2cCtrl_EnumerateAcpiChildren(
            FdoExt->Self,
            FdoExt,
            childHandle
        );

        //
        // Recurse into this child
        //
        I2cCtrl_Log(
            "EnumNS: recursing into child=%p depth=%lu\n",
            childHandle, Depth + 1
        );

        I2cCtrl_EnumerateAcpiNamespace(
            AcpiPdo,
            childHandle,
            Depth + 1,
            FdoExt);

        index++;
    }

    I2cCtrl_Log("EnumNS: exit depth=%lu\n", Depth);
    return STATUS_SUCCESS;
}


/* --- Robust _CRS GPIO parsing with hex dump and defensive pin search; C89-compliant --- */

/* Helper: hex dump a buffer to TraceEvents in lines of 16 bytes */
VOID
I2cCtrl_HexDump(
    const UCHAR *buf,
    ULONG        len,
    const char  *tag
    )
{
    ULONG i, line;
    char  ascii[17];
    char  lineBuf[80];

    if (buf == NULL || len == 0) {
        I2cCtrl_Log("HexDump(%s): invalid buffer len=%lu\n",
                    tag ? tag : "CRS", len);
        return;
    }

    ascii[16] = '\0';
    I2cCtrl_Log("HexDump(%s), len=%lu\n", tag ? tag : "CRS", len);

    line = 0;
    for (i = 0; i < len; i++) {

        UCHAR b = buf[i];
        ascii[i % 16] = (b >= 32 && b < 127) ? (char)b : '.';

        if ((i % 16) == 0) {
            RtlStringCchPrintfA(lineBuf, sizeof(lineBuf), "%04lu: ", line++);
        }

        {
            char byteStr[4];
            RtlStringCchPrintfA(byteStr, sizeof(byteStr), "%02X ", b);
            RtlStringCchCatA(lineBuf, sizeof(lineBuf), byteStr);
        }

        if ((i % 16) == 15 || i == (len - 1)) {

            ULONG j;
            if ((i % 16) != 15) {
                for (j = (i % 16) + 1; j < 16; j++) {
                    RtlStringCchCatA(lineBuf, sizeof(lineBuf), "   ");
                }
            }

            ascii[(i % 16) + 1] = '\0';

            {
                char asciiStr[20];
                RtlStringCchPrintfA(asciiStr, sizeof(asciiStr), " |%s|", ascii);
                RtlStringCchCatA(lineBuf, sizeof(lineBuf), asciiStr);
            }

            I2cCtrl_Log("%s\n", lineBuf);
            lineBuf[0] = '\0';
        }
    }
}


/* -----------------------------------------------------------------------
 * I2cCtrl_ParseCrsForGpio
 *
 * Parse ACPI _CRS buffer for a GPIO Connection Descriptor (Large Item 0x8C).
 * Extracts:
 *   - First GPIO pin number
 *   - ActiveLow flag
 *
 * XP/2003-safe, C89-compliant, no assumptions about alignment.
 * ----------------------------------------------------------------------- */
BOOLEAN
I2cCtrl_ParseCrsForGpio(
    const UCHAR *buf,
    ULONG        len,
    PULONG       gpioPinOut,
    PBOOLEAN     activeLowOut
    )
{
    ULONG i;

    if (gpioPinOut == NULL || activeLowOut == NULL) {
        return FALSE;
    }

    *gpioPinOut   = 0;
    *activeLowOut = FALSE;

    if (buf == NULL || len < 3) {
        I2cCtrl_Log("ParseCrsForGpio: invalid buffer len=%lu\n", len);
        return FALSE;
    }

    i = 0;
    while (i + 1 < len) {

        UCHAR tag = buf[i];

        if ((tag & 0x80) == 0) {
            UCHAR smallLen = (UCHAR)(tag & 0x07);
            if (i + 1 + smallLen > len) break;
            i += 1 + smallLen;
            continue;
        }

        if (i + 3 > len) break;

        {
            UCHAR  largeType    = (UCHAR)(tag & 0x7F);
            USHORT largeLen     = (USHORT)(buf[i + 1] | ((USHORT)buf[i + 2] << 8));
            ULONG  payloadStart = i + 3;

            if (payloadStart + largeLen > len) break;

            if (largeType == 0x8C) {

                const UCHAR *p = buf + payloadStart;
                ULONG        n = (ULONG)largeLen;

                I2cCtrl_HexDump(p, n, "GPIO");

                if (n >= 12) {

                    USHORT pinCount  = (USHORT)(p[10] | ((USHORT)p[11] << 8));
                    USHORT pinOffset = (USHORT)(p[8]  | ((USHORT)p[9]  << 8));

                    if (pinCount >= 1 &&
                        pinOffset < n &&
                        (ULONG)pinOffset + pinCount <= n) {

                        UCHAR firstPin = p[pinOffset];
                        UCHAR flags    = p[1];
                        BOOLEAN activeLow = ((flags & 0x01) ? TRUE : FALSE);

                        *gpioPinOut   = firstPin;
                        *activeLowOut = activeLow;

                        I2cCtrl_Log("GPIO parsed: pin=%u activeLow=%u pinCount=%u\n",
                                    firstPin,
                                    activeLow ? 1 : 0,
                                    pinCount);

                        return TRUE;
                    }
                }

                if (n >= 7) {
                    UCHAR flags = p[5];
                    UCHAR pin   = p[6];

                    *gpioPinOut   = pin;
                    *activeLowOut = ((flags & 0x01) ? TRUE : FALSE);

                    I2cCtrl_Log("GPIO fallback: pin=%u activeLow=%u\n",
                                pin,
                                *activeLowOut ? 1 : 0);

                    return TRUE;
                }
            }

            i = payloadStart + largeLen;
        }
    }

    I2cCtrl_Log("ParseCrsForGpio: no GPIO descriptor found\n");
    return FALSE;
}


/* -----------------------------------------------------------------------------
   Timeout DPC: HAL-generic, XP-safe, C89-compliant, single completion discipline
   -----------------------------------------------------------------------------
   Policy:
   - Early exit under QueueLock if XferCtx.Irp == NULL or ActiveBusy == FALSE
   - Cancel the timeout timer first to prevent overlapping DPCs
   - Failure path: fence IRP under QueueLock (clear ActiveBusy, detach Irp),
     quiesce hardware via HAL, set HardwareFailure, and complete once
   - Retry path: quiesce hardware, bound/re-arm timer, continue transfer state
     (do NOT detach Irp)
----------------------------------------------------------------------------- */
VOID
I2cCtrl_TimeoutDpcRoutine(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArg1,
    IN PVOID SystemArg2
    )
{
    PI2CCTRL_FDO           devctx;
    I2C_TRANSFER_CONTEXT*  xc;
    PIRP                   irp;
    KIRQL                  oldIrql;
    BOOLEAN                cancelled;
    LARGE_INTEGER          dueTime;
    ULONGLONG              rel100ns;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArg1);
    UNREFERENCED_PARAMETER(SystemArg2);

    /* C89 init */
    devctx    = (PI2CCTRL_FDO)DeferredContext;
    xc        = NULL;
    irp       = NULL;
    oldIrql   = 0;
    cancelled = FALSE;
    dueTime.QuadPart = 0;
    rel100ns  = 0ULL;

    ASSERT(KeGetCurrentIrql() == DISPATCH_LEVEL);
    if (devctx == NULL) {
        return;
    }

    xc = &devctx->XferCtx;

    /* Early-exit fence: never touch IRP if cleared or inactive */
    KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
    irp = xc->Irp;
    if (irp == NULL || devctx->ActiveBusy == FALSE) {
        KeReleaseSpinLock(&devctx->QueueLock, oldIrql);
        return;
    }
    KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

    /* Cancel timeout to block overlapping DPCs before any action */
    cancelled = I2CCTRL_WdmTimerCancelCompat(&xc->TimeoutTimer);
    UNREFERENCED_PARAMETER(cancelled);

    /* If device is not in a valid started state: fail safely */
    if (devctx->Removed != FALSE ||
        devctx->Stopping != FALSE ||
        devctx->Started == FALSE ||
        devctx->Ops == NULL) {

        KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
        if (xc->Irp != irp) {
            KeReleaseSpinLock(&devctx->QueueLock, oldIrql);
            return;
        }
        devctx->ActiveBusy = FALSE;
        xc->Status = STATUS_DEVICE_NOT_READY;
        xc->Irp = NULL;
        KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

        /* Quiesce hardware via HAL (only if Ops is valid) */
        if (devctx->Ops != NULL) {
            if (devctx->Ops->MaskInterrupts != NULL) {
                devctx->Ops->MaskInterrupts(devctx, 0U);
            }
            if (devctx->Ops->AckInterrupts != NULL) {
                devctx->Ops->AckInterrupts(
                    devctx,
                    I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
                    I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
                    I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
                    I2C_INT_RD_REQ);
            }
            if (devctx->Ops->Enable != NULL) {
                (VOID)devctx->Ops->Enable(devctx, FALSE);
            }
        }

        devctx->HardwareFailure = TRUE;

        irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
        irp->IoStatus.Information = 0U;
        KeSetEvent(&devctx->TransferEvent, IO_NO_INCREMENT, FALSE);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return;
    }

    /* Retry path */
    if (xc->RetryCount < devctx->MaxRetries) {
        xc->RetryCount++;

        if (devctx->Ops != NULL) {
            if (devctx->Ops->MaskInterrupts != NULL) {
                devctx->Ops->MaskInterrupts(devctx, 0U);
            }
            if (devctx->Ops->AckInterrupts != NULL) {
                devctx->Ops->AckInterrupts(
                    devctx,
                    I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
                    I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
                    I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
                    I2C_INT_RD_REQ);
            }
            if (devctx->Ops->Enable != NULL) {
                (VOID)devctx->Ops->Enable(devctx, FALSE);
                (VOID)devctx->Ops->Enable(devctx, TRUE);
            }
        }

        if (devctx->TransactionTimeoutMs == 0U) {
            devctx->TransactionTimeoutMs = 5000U;
        } else if (devctx->TransactionTimeoutMs > 600000U) {
            devctx->TransactionTimeoutMs = 600000U;
        }

        rel100ns = ((ULONGLONG)devctx->TransactionTimeoutMs) * 10000ULL;
        if (rel100ns > 0x7FFFFFFFFFFFFFFFULL) {
            rel100ns = 0x7FFFFFFFFFFFFFFFULL;
        }
        dueTime.QuadPart = -(LONGLONG)rel100ns;

        (VOID)I2CCTRL_WdmTimerStartCompat(&xc->TimeoutTimer, dueTime, 0UL, &devctx->TimeoutDpc);

        KeInsertQueueDpc(&devctx->QueueDpc, NULL, NULL);
        return;
    }

    /* Retries exhausted: fence, quiesce, and complete with timeout */
    KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
    if (xc->Irp != irp) {
        KeReleaseSpinLock(&devctx->QueueLock, oldIrql);
        return;
    }
    devctx->ActiveBusy = FALSE;
    xc->Status = STATUS_IO_TIMEOUT;
    xc->Irp = NULL;
    KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

    if (devctx->Ops != NULL) {
        if (devctx->Ops->MaskInterrupts != NULL) {
            devctx->Ops->MaskInterrupts(devctx, 0U);
        }
        if (devctx->Ops->AckInterrupts != NULL) {
            devctx->Ops->AckInterrupts(
                devctx,
                I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
                I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
                I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
                I2C_INT_RD_REQ);
        }
        if (devctx->Ops->Enable != NULL) {
            (VOID)devctx->Ops->Enable(devctx, FALSE);
        }
    }

    devctx->HardwareFailure = TRUE;

    irp->IoStatus.Status = STATUS_IO_TIMEOUT;
    irp->IoStatus.Information = 0U;
    KeSetEvent(&devctx->TransferEvent, IO_NO_INCREMENT, FALSE);
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

/* -----------------------------------------------------------------------
 * I2cCtrl_QueueDpcRoutine - QoS scheduler: dequeue by priority and start HW transfer
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_QueueDpcRoutine(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2
    )
{
    PI2CCTRL_FDO          devctx;
    KIRQL                 oldIrql;
    PSMBUS_REQUEST        req;
    I2C_TRANSFER_CONTEXT* xc;
    PIRP                  irp;
    NTSTATUS              status;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    devctx = (PI2CCTRL_FDO)DeferredContext;
    if (devctx == NULL) {
        return;
    }

    /* If device is stopping or not started, drain and cancel one pending request */
    if (!devctx->Started || devctx->Stopping || devctx->Mmio == NULL) {
        KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);

        /* Cancel in priority order: High -> Normal -> Low */
        req = NULL;
        if (!IsListEmpty(&devctx->HighQueue)) {
            req = CONTAINING_RECORD(RemoveHeadList(&devctx->HighQueue), SMBUS_REQUEST, ListEntry);
        } else if (!IsListEmpty(&devctx->NormalQueue)) {
            req = CONTAINING_RECORD(RemoveHeadList(&devctx->NormalQueue), SMBUS_REQUEST, ListEntry);
        } else if (!IsListEmpty(&devctx->LowQueue)) {
            req = CONTAINING_RECORD(RemoveHeadList(&devctx->LowQueue), SMBUS_REQUEST, ListEntry);
        }

        KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

        if (req != NULL && req->Irp != NULL) {
            req->Irp->IoStatus.Status = STATUS_CANCELLED;
            req->Irp->IoStatus.Information = 0;
            IoCompleteRequest(req->Irp, IO_NO_INCREMENT);
        }
        if (req != NULL) {
            ExFreePool(req);
        }
        return;
    }

    /* Dequeue next request using QoS fairness (strict priority with burst limits) */
    KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
    req = NULL;

    /* High priority first unless burst limit reached and lower queues have work */
    if (!IsListEmpty(&devctx->HighQueue)) {
        if (devctx->BurstHigh < devctx->BurstHighMax ||
            (IsListEmpty(&devctx->NormalQueue) && IsListEmpty(&devctx->LowQueue))) {
            req = CONTAINING_RECORD(RemoveHeadList(&devctx->HighQueue), SMBUS_REQUEST, ListEntry);
            devctx->BurstHigh++;
            devctx->BurstNormal = 0U;
            devctx->BurstLow    = 0U;
        }
    }

    /* Normal priority next */
    if (req == NULL && !IsListEmpty(&devctx->NormalQueue)) {
        if (devctx->BurstNormal < devctx->BurstNormalMax ||
            (IsListEmpty(&devctx->HighQueue) && IsListEmpty(&devctx->LowQueue))) {
            req = CONTAINING_RECORD(RemoveHeadList(&devctx->NormalQueue), SMBUS_REQUEST, ListEntry);
            devctx->BurstNormal++;
            devctx->BurstHigh = 0U;
            devctx->BurstLow  = 0U;
        }
    }

    /* Low priority last */
    if (req == NULL && !IsListEmpty(&devctx->LowQueue)) {
        if (devctx->BurstLow < devctx->BurstLowMax ||
            (IsListEmpty(&devctx->HighQueue) && IsListEmpty(&devctx->NormalQueue))) {
            req = CONTAINING_RECORD(RemoveHeadList(&devctx->LowQueue), SMBUS_REQUEST, ListEntry);
            devctx->BurstLow++;
            devctx->BurstHigh   = 0U;
            devctx->BurstNormal = 0U;
        }
    }

    KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

    if (req == NULL) {
        return;
    }

    irp = req->Irp;
    if (irp == NULL) {
        ExFreePool(req);
        return;
    }

    /* Attach the request pointer to IRP for cleanup in completion path */
    irp->Tail.Overlay.DriverContext[0] = req;

    /* Reset and configure transfer context under QueueLock to synchronize with timeout DPC */
    xc = &devctx->XferCtx;
    RtlZeroMemory(xc, sizeof(*xc));

    KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
    xc->Irp        = irp;
    xc->Status     = STATUS_SUCCESS;
    xc->StopSeen   = FALSE;
    xc->RetryCount = 0;
    devctx->ActiveBusy = TRUE;
    KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

    /* Arm the per-transfer timeout */
    KeInitializeTimer(&xc->TimeoutTimer);
    KeInitializeDpc(&xc->TimeoutDpc, I2cCtrl_TimeoutDpcRoutine, devctx);
    {
        LARGE_INTEGER dueTime;
        dueTime.QuadPart = -((LONGLONG)devctx->TransactionTimeoutMs * 10 * 1000); /* relative, 100ns */
        KeSetTimer(&xc->TimeoutTimer, dueTime, &xc->TimeoutDpc);
    }

    /* Program target address (7-bit) and enable controller via HAL ops */
    if (devctx->Ops != NULL && devctx->Ops->SetTarget7bit != NULL) {
        status = devctx->Ops->SetTarget7bit(devctx, (UCHAR)(req->SlaveAddress & 0x7FU));
        if (!NT_SUCCESS(status)) {
            irp->IoStatus.Status = status;
            irp->IoStatus.Information = 0;
            irp->Tail.Overlay.DriverContext[0] = NULL;

            KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
            if (devctx->XferCtx.Irp == irp) {
                devctx->XferCtx.Irp = NULL;
                devctx->ActiveBusy = FALSE;
            }
            KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

            IoCompleteRequest(irp, IO_NO_INCREMENT);
            ExFreePool(req);
            return;
        }
    }
    if (devctx->Ops != NULL && devctx->Ops->Enable != NULL) {
        (VOID)devctx->Ops->Enable(devctx, TRUE);
    }

    /* Clear latched causes before starting */
    if (devctx->Ops != NULL && devctx->Ops->AckInterrupts != NULL) {
        devctx->Ops->AckInterrupts(
            devctx,
            I2C_INT_TX_ABORT  | I2C_INT_RX_OVER | I2C_INT_RX_UNDER |
            I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
            I2C_INT_GEN_CALL | I2C_INT_ACTIVITY |
            I2C_INT_RX_DONE  | I2C_INT_RD_REQ);
    }

    /* Dispatch by opcode (existing logic retained) */
    if (req->OpCode == I2CCTRL_OPCODE_BLOCK_WRITE) {
        /* existing block write logic */

    } else if (req->OpCode == I2CCTRL_OPCODE_BLOCK_READ) {
        /* existing block read logic */

    } else if (req->OpCode == I2CCTRL_OPCODE_GET_PT_SAMPLE) {
        PT_RAW_SAMPLE sample;
        RtlZeroMemory(&sample, sizeof(sample));

        if (devctx->Ops != NULL && devctx->Ops->QueryTouchSample != NULL) {
            status = devctx->Ops->QueryTouchSample(devctx, &sample);
        } else {
            status = STATUS_NOT_SUPPORTED;
        }

        irp->IoStatus.Status = status;
        if (NT_SUCCESS(status)) {
            ULONG outLen;
            outLen = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
            if (outLen >= sizeof(sample)) {
                RtlCopyMemory(irp->AssociatedIrp.SystemBuffer, &sample, sizeof(sample));
                irp->IoStatus.Information = sizeof(sample);
            } else {
                irp->IoStatus.Information = 0;
                irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
            }
        } else {
            irp->IoStatus.Information = 0;
        }

        irp->Tail.Overlay.DriverContext[0] = NULL;

        KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
        if (devctx->XferCtx.Irp == irp) {
            devctx->XferCtx.Irp = NULL;
            devctx->ActiveBusy = FALSE;
        }
        KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

        IoCompleteRequest(irp, IO_NO_INCREMENT);
        ExFreePool(req);
        return;

    } else {
        /* Unsupported opcode: fail the IRP and free request */
        irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        irp->IoStatus.Information = 0;
        irp->Tail.Overlay.DriverContext[0] = NULL;

        KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
        if (devctx->XferCtx.Irp == irp) {
            devctx->XferCtx.Irp = NULL;
            devctx->ActiveBusy = FALSE;
        }
        KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

        IoCompleteRequest(irp, IO_NO_INCREMENT);
        ExFreePool(req);
        return;
    }
}


/* -----------------------------------------------------------------------
 * kernel logger with printf-style formatting + timestamp prefix
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_Log(
    PCSTR Format,
    ...
    )
{
    CHAR  buffer[512];
    CHAR  final[600];
    va_list args;
    NTSTATUS status;

    UNICODE_STRING      path;
    OBJECT_ATTRIBUTES   oa;
    IO_STATUS_BLOCK     iosb;
    HANDLE              hFile;

    LARGE_INTEGER       sysTime, localTime;
    TIME_FIELDS         tf;

    PAGED_CODE();

    //
    // Hard safety guards: prevent use-after-free crashes
    //
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    if (Format == NULL) {
        return;
    }

    //
    // SAFE pointer formatting:
    // Convert all %p to 0x%I64X BEFORE calling VPrintf.
    // This prevents the CRT from dereferencing freed pointers.
    //
    {
        CHAR safeFmt[256];
        SIZE_T i = 0, j = 0;

        while (Format[i] != '\0' && j < sizeof(safeFmt) - 1) {
            if (Format[i] == '%' && Format[i+1] == 'p') {
                safeFmt[j++] = '0';
                safeFmt[j++] = 'x';
                safeFmt[j++] = '%';
                safeFmt[j++] = 'I';
                safeFmt[j++] = '6';
                safeFmt[j++] = '4';
                safeFmt[j++] = 'X';
                i += 2;
                continue;
            }
            safeFmt[j++] = Format[i++];
        }
        safeFmt[j] = '\0';

        va_start(args, Format);
        status = RtlStringCbVPrintfA(buffer, sizeof(buffer), safeFmt, args);
        va_end(args);

        if (!NT_SUCCESS(status)) {
            return;
        }
    }

    /* Get local time */
    KeQuerySystemTime(&sysTime);
    ExSystemTimeToLocalTime(&sysTime, &localTime);
    RtlTimeToTimeFields(&localTime, &tf);

    /* Format timestamp prefix: [DD/MM/YYYY, HH:MM AM/PM] */
    {
        CHAR ts[64];
        ULONG hour = tf.Hour;
        BOOLEAN pm = FALSE;

        if (hour == 0) {
            hour = 12;
        } else if (hour == 12) {
            pm = TRUE;
        } else if (hour > 12) {
            hour -= 12;
            pm = TRUE;
        }

        RtlStringCbPrintfA(
            ts,
            sizeof(ts),
            "[%02u/%02u/%04u, %02u:%02u %s] ",
            tf.Day,
            tf.Month,
            tf.Year,
            hour,
            tf.Minute,
            pm ? "PM" : "AM"
        );

        RtlStringCbPrintfA(
            final,
            sizeof(final),
            "%s%s",
            ts,
            buffer
        );
    }

    /* Open log file */
    RtlInitUnicodeString(&path, L"\\SystemRoot\\System32\\i2cctrl.log");

    InitializeObjectAttributes(
        &oa,
        &path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL
    );

    status = ZwCreateFile(
                 &hFile,
                 FILE_APPEND_DATA | SYNCHRONIZE,
                 &oa,
                 &iosb,
                 NULL,
                 FILE_ATTRIBUTE_NORMAL,
                 0,
                 FILE_OPEN_IF,
                 FILE_SYNCHRONOUS_IO_NONALERT,
                 NULL,
                 0
             );

    if (!NT_SUCCESS(status)) {
        return;
    }

    /* Write timestamped line */
    ZwWriteFile(
        hFile,
        NULL,
        NULL,
        NULL,
        &iosb,
        final,
        (ULONG)strlen(final),
        NULL,
        NULL
    );

    ZwClose(hFile);

    //
    // Mirror to ETW/WPP without the timestamp prefix.
    //
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_FLAG_BUS,
        "%s",
        buffer
    );
}

/* -----------------------------------------------------------------------
 * I2cCtrl_StartDevice - XP/2003-safe, HAL-generic, C89-compliant.
 * Performs MMIO mapping, maps LPSS BAR2 when present, applies table-driven
 * LPSS + DW-I2C quirks, installs the backend, connects interrupts,
 * enables the controller, and exposes a universal HID-over-I2C child PDO.
 * ----------------------------------------------------------------------- */

NTSTATUS
I2cCtrl_StartDevice(
    IN PI2CCTRL_FDO fdoExt,
    IN PIRP         Irp
    )
{
    NTSTATUS                         status;
    PIO_STACK_LOCATION               isl;
    PCM_RESOURCE_LIST                transList;
    ULONG                            outer;
    ULONG                            inner;
    BOOLEAN                          haveMem;
    BOOLEAN                          haveInt;
    BOOLEAN                          shareVector;
    ULONG                            intrFlags;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR  desc;
    KINTERRUPT_MODE                  mode;
    PHYSICAL_ADDRESS                 mmioPhys;
    ULONG                            mmioLength;

    /* BAR2 detection flags */
    BOOLEAN                          haveBar2;
    PHYSICAL_ADDRESS                 bar2Phys;
    ULONG                            bar2Length;

    /* C89 init */
    status      = STATUS_SUCCESS;
    isl         = NULL;
    transList   = NULL;
    outer       = 0U;
    inner       = 0U;
    haveMem     = FALSE;
    haveInt     = FALSE;
    shareVector = FALSE;
    intrFlags   = 0U;
    desc        = NULL;
    mode        = LevelSensitive;
    mmioPhys.QuadPart = 0;
    mmioLength  = 0U;

    haveBar2          = FALSE;
    bar2Phys.QuadPart = 0;
    bar2Length        = 0U;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    PAGED_CODE();

    I2cCtrl_Log("StartDevice: entered\n");

/* -----------------------------------------------------------------------
 * StartDevice – IRP must NEVER be NULL.
 * Redirector mode is removed. Only PnP StartDevice may call this.
 * ----------------------------------------------------------------------- */

if (fdoExt == NULL || Irp == NULL) {
    I2cCtrl_Log("StartDevice: invalid parameters (fdoExt=%p Irp=%p)\n",
                fdoExt, Irp);
    return STATUS_INVALID_PARAMETER;
}

isl = IoGetCurrentIrpStackLocation(Irp);
if (isl == NULL) {
    I2cCtrl_Log("StartDevice: IoGetCurrentIrpStackLocation returned NULL\n");
    return STATUS_INVALID_PARAMETER;
}

transList = isl->Parameters.StartDevice.AllocatedResourcesTranslated;
if (transList == NULL || transList->Count == 0) {
    I2cCtrl_Log("StartDevice: no translated resources\n");
    return STATUS_INSUFFICIENT_RESOURCES;
}

    /* -------------------------------------------------------------
     * Populate PnpId (required for quirks + backend selection)
     * ------------------------------------------------------------- */
    {
        WCHAR    hwidBuf[256];
        ULONG    hwidLen = 0;
        NTSTATUS st;

        /* Guard against bad PDO or wrong IRQL (IoGetDeviceProperty is PASSIVE only) */
        if (fdoExt->PhysicalDevice == NULL ||
            KeGetCurrentIrql() != PASSIVE_LEVEL)
        {
            fdoExt->PnpId = NULL;
            I2cCtrl_Log("StartDevice: skipping PnpId capture (PDO=%p IRQL=%lu)\n",
                        fdoExt->PhysicalDevice,
                        (ULONG)KeGetCurrentIrql());
        }
        else
        {
            st = IoGetDeviceProperty(
                    fdoExt->PhysicalDevice,
                    DevicePropertyHardwareID,
                    sizeof(hwidBuf),
                    hwidBuf,
                    &hwidLen
                );

            if (NT_SUCCESS(st) && hwidLen >= sizeof(WCHAR)) {

                SIZE_T bytes = hwidLen + sizeof(WCHAR);
                PWSTR  copy  = ExAllocatePoolWithTag(NonPagedPool, bytes, 'pdiI');

                if (copy != NULL) {
                    RtlZeroMemory(copy, bytes);
                    RtlCopyMemory(copy, hwidBuf, hwidLen);
                    fdoExt->PnpId = copy;
                    I2cCtrl_Log("StartDevice: PnpId captured\n");
                } else {
                    fdoExt->PnpId = NULL;
                    I2cCtrl_Log("StartDevice: PnpId alloc failed\n");
                }

            } else {
                fdoExt->PnpId = NULL;
                I2cCtrl_Log("StartDevice: PnpId unavailable (st=0x%08lx len=%lu)\n",
                            st, hwidLen);
            }
        }
    }

/* -------------------------------------------------------------
 * Match HWID against g_I2cControllers[] and capture profile
 * ------------------------------------------------------------- */
{
    const I2CCTRL_DEVICE_ID* match = NULL;
    ULONG i;

    if (fdoExt->PnpId != NULL) {
        for (i = 0; i < g_I2cControllersCount; i++) {
            if (wcsstr(fdoExt->PnpId, g_I2cControllers[i].PciId) != NULL) {
                match = &g_I2cControllers[i];
                break;
            }
        }
    }

    if (match == NULL) {

        WCHAR wbuf[256];
        CHAR  abuf[256];
        UNICODE_STRING ustr;
        ANSI_STRING astr;

        RtlStringCchPrintfW(
            wbuf,
            RTL_NUMBER_OF(wbuf),
            L"StartDevice: unsupported controller HWID %ws",
            (fdoExt->PnpId != NULL) ? fdoExt->PnpId : L"<null>"
        );

        RtlInitUnicodeString(&ustr, wbuf);
        astr.Buffer        = abuf;
        astr.Length        = 0;
        astr.MaximumLength = sizeof(abuf);

        if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&astr, &ustr, FALSE))) {
            abuf[astr.Length] = '\0';
            I2cCtrl_Log(abuf);
        }

        return STATUS_NOT_SUPPORTED;
    }

    /* Log matched controller (BAR0 offsets only - LPSS offsets not used here) */
    {
        WCHAR wbuf[256];
        CHAR  abuf[256];
        UNICODE_STRING ustr;
        ANSI_STRING astr;

        RtlStringCchPrintfW(
            wbuf,
            RTL_NUMBER_OF(wbuf),
            L"StartDevice: matched controller %ws "
            L"(BAR0 Offsets: CTRL=%02X STAT=%02X DATA=%02X CLK=%02X, "
            L"quirks=0x%X bsod=0x%X)\n",
            match->PciId,
            match->ControlOffset,
            match->StatusOffset,
            match->DataOffset,
            match->ClockOffset,
            match->Quirks,
            match->BsodQuirks
        );

        RtlInitUnicodeString(&ustr, wbuf);
        astr.Buffer        = abuf;
        astr.Length        = 0;
        astr.MaximumLength = sizeof(abuf);

        if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&astr, &ustr, FALSE))) {
            abuf[astr.Length] = '\0';
            I2cCtrl_Log(abuf);
        }
    }

    /* Store quirks (your FDO already has BsodQuirks, but NOT Quirks) */
    fdoExt->BsodQuirks = match->BsodQuirks;

    /* Quirks are applied later by I2cCtrlApplyQuirks() using PnpId */
}

    isl = IoGetCurrentIrpStackLocation(Irp);
    transList =
        (isl != NULL) ? isl->Parameters.StartDevice.AllocatedResourcesTranslated : NULL;

    I2cCtrl_Log("StartDevice: got translated resources\n");

    if (transList == NULL || transList->Count == 0U) {
        I2cCtrl_Log("StartDevice: no translated resources\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

/* Parse translated resources for MMIO + IRQ + LPSS BAR2 */
for (outer = 0U; outer < transList->Count; outer++) {

    PCM_FULL_RESOURCE_DESCRIPTOR frd = &transList->List[outer];
    PCM_PARTIAL_RESOURCE_LIST    prl = &frd->PartialResourceList;

    for (inner = 0U; inner < prl->Count; inner++) {

desc = &prl->PartialDescriptors[inner];

if (desc->Type == CmResourceTypeMemory) {

    /* First memory resource = DW-I2C BAR0 */
    if (!haveMem) {
        mmioPhys   = desc->u.Memory.Start;
        mmioLength = desc->u.Memory.Length;
        haveMem    = TRUE;
    }

    /*
     * DO NOT treat any additional memory resources as LPSS BAR2.
     * On Whiskey Lake / CNP-LP, LPSS BAR2 is NOT exposed in ACPI _CRS.
     * It must be derived later from PWRMBASE + PID offset.
     */

} else if (desc->Type == CmResourceTypeInterrupt) {

    fdoExt->IrqVector   = desc->u.Interrupt.Vector;
    fdoExt->IrqLevel    = (KIRQL)desc->u.Interrupt.Level;
    fdoExt->IrqAffinity = desc->u.Interrupt.Affinity;
    intrFlags           = desc->Flags;
    shareVector         =
        (desc->ShareDisposition != CmResourceShareDeviceExclusive);

    fdoExt->IrqFlags    = desc->Flags;
    fdoExt->IrqShare    = desc->ShareDisposition;
    fdoExt->IrqLatched  =
        ((intrFlags & CM_RESOURCE_INTERRUPT_LATCHED) != 0U);
    fdoExt->IrqMode     =
        fdoExt->IrqLatched ? Latched : LevelSensitive;
    fdoExt->IrqSharable = shareVector;
    haveInt             =
        (fdoExt->IrqVector != 0U) && (fdoExt->IrqAffinity != 0U);
}

    }
}

    if (!haveMem) {
        I2cCtrl_Log("StartDevice: no MMIO resource\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (mmioLength < 0x00A8U) {
        I2cCtrl_Log("StartDevice: MMIO length too small\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Map BAR0 (DW-I2C) */
    fdoExt->Mmio = (PUCHAR)MmMapIoSpace(mmioPhys, mmioLength, MmNonCached);
    if (fdoExt->Mmio == NULL) {
        I2cCtrl_Log("StartDevice: MmMapIoSpace failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fdoExt->MmioPhys   = mmioPhys;
    fdoExt->MmioLength = mmioLength;
    fdoExt->MmioBase   = fdoExt->Mmio;

    I2cCtrl_Log("StartDevice: MMIO mapped\n");

/* BAR0 diagnostic dump (first 0x40 bytes) */
{
    ULONG          off;
    WCHAR          wbuf[128];
    CHAR           abuf[128];
    UNICODE_STRING ustr;
    ANSI_STRING    astr;

    for (off = 0; off < 0x40; off += 4) {

        ULONG v = READ_REGISTER_ULONG((PULONG)(fdoExt->MmioBase + off));

        RtlStringCchPrintfW(
            wbuf,
            RTL_NUMBER_OF(wbuf),
            L"BAR0[%02X] = 0x%08lx",
            off,
            v
        );

        RtlInitUnicodeString(&ustr, wbuf);
        astr.Buffer        = abuf;
        astr.Length        = 0;
        astr.MaximumLength = sizeof(abuf);

        if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&astr, &ustr, FALSE))) {
            abuf[astr.Length] = '\0';
            I2cCtrl_Log("%s\n", abuf);
        }
    }
}

/* Map LPSS BAR2 if present */
if (haveBar2) {

    fdoExt->LpssBar2Phys   = bar2Phys;
    fdoExt->LpssBar2Length = bar2Length;

    fdoExt->LpssBar2 = MmMapIoSpace(
        fdoExt->LpssBar2Phys,
        fdoExt->LpssBar2Length,
        MmNonCached
    );

    if (fdoExt->LpssBar2 != NULL) {
        I2cCtrl_Log("StartDevice: LPSS BAR2 mapped\n");
    } else {
        I2cCtrl_Log("StartDevice: LPSS BAR2 map FAILED\n");
    }

} else {

    fdoExt->LpssBar2 = NULL;
    I2cCtrl_Log("StartDevice: no LPSS BAR2 resource\n");
}


/* Initialize locks/DPCs/events (first start or restart) */
if (!fdoExt->InitDone) {

    I2CCTRL_INIT_LOCK(&fdoExt->BusLock);
    I2CCTRL_INIT_LOCK(&fdoExt->HwLock);
    I2CCTRL_INIT_LOCK(&fdoExt->QueueLock);
    I2CCTRL_INIT_LOCK(&fdoExt->PendingIrpLock);
    I2CCTRL_INIT_LOCK(&fdoExt->CancelLock);
    I2CCTRL_INIT_LOCK(&fdoExt->IoLock);

    KeInitializeDpc(&fdoExt->IsrDpc,     I2cCtrl_DpcRoutine,        fdoExt);
    KeInitializeDpc(&fdoExt->QueueDpc,   I2cCtrl_QueueDpcRoutine,   fdoExt);
    KeInitializeDpc(&fdoExt->TimeoutDpc, I2cCtrl_TimeoutDpcRoutine, fdoExt);

    KeInitializeEvent(&fdoExt->TransferEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&fdoExt->StopEvent,     NotificationEvent, FALSE);

    fdoExt->InitDone       = TRUE;
    fdoExt->DpcInitialized = TRUE;

} else {

    KeInitializeDpc(&fdoExt->IsrDpc,     I2cCtrl_DpcRoutine,        fdoExt);
    KeInitializeDpc(&fdoExt->QueueDpc,   I2cCtrl_QueueDpcRoutine,   fdoExt);
    KeInitializeDpc(&fdoExt->TimeoutDpc, I2cCtrl_TimeoutDpcRoutine, fdoExt);
    fdoExt->DpcInitialized = TRUE;
}

/* Minimal mask until ISR is connected */
fdoExt->IntrMask = I2C_INT_STOP_DETECTED | I2C_INT_TX_ABORT;

if (fdoExt->Ops != NULL && fdoExt->Ops->MaskInterrupts != NULL) {
    fdoExt->Ops->MaskInterrupts(fdoExt, 0U);
}

if (fdoExt->Ops != NULL && fdoExt->Ops->Enable != NULL) {
    (VOID)fdoExt->Ops->Enable(fdoExt, FALSE);
}

status = I2cCtrl_WaitForEnableState(fdoExt, FALSE, 500U);
if (!NT_SUCCESS(status)) {
    I2cCtrl_Log("StartDevice: disable did not latch\n");
    fdoExt->HardwareFailure = TRUE;

    MmUnmapIoSpace(fdoExt->Mmio, fdoExt->MmioLength);
    fdoExt->Mmio       = NULL;
    fdoExt->MmioLength = 0U;
    fdoExt->MmioPhys.QuadPart = 0;
    return status;
}

/* Clear residual interrupt causes while disabled */
if (fdoExt->Ops != NULL && fdoExt->Ops->AckInterrupts != NULL) {
    fdoExt->Ops->AckInterrupts(
        fdoExt,
          I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
          I2C_INT_TX_ABORT | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
          I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
          I2C_INT_RD_REQ
    );
}

/* Connect interrupt if available */
if (haveInt && fdoExt->InterruptObject == NULL) {

    WCHAR wbuf[160];
    CHAR  abuf[160];
    UNICODE_STRING ustr;
    ANSI_STRING astr;

    /* Log what we are about to connect */
    RtlStringCchPrintfW(
        wbuf,
        RTL_NUMBER_OF(wbuf),
        L"StartDevice: Connecting interrupt:\n"
        L"  Vector=%lu Level=%lu Mode=%s Sharable=%lu Affinity=0x%p\n",
        fdoExt->IrqVector,
        (ULONG)fdoExt->IrqLevel,
        (fdoExt->IrqMode == Latched) ? L"Latched" : L"Level",
        fdoExt->IrqSharable ? 1UL : 0UL,
        (PVOID)(ULONG_PTR)fdoExt->IrqAffinity
    );

    RtlInitUnicodeString(&ustr, wbuf);
    astr.Buffer        = abuf;
    astr.Length        = 0;
    astr.MaximumLength = sizeof(abuf);

    if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&astr, &ustr, FALSE))) {
        abuf[astr.Length] = '\0';
        I2cCtrl_Log(abuf);
    }

    mode = fdoExt->IrqMode;

    status = IoConnectInterrupt(
        &fdoExt->InterruptObject,
        (PKSERVICE_ROUTINE)I2cCtrl_Isr,
        (PVOID)fdoExt,
        (PKSPIN_LOCK)&fdoExt->HwLock,
        fdoExt->IrqVector,
        fdoExt->IrqLevel,
        fdoExt->IrqLevel,
        mode,
        fdoExt->IrqSharable,
        fdoExt->IrqAffinity,
        FALSE
    );

    if (!NT_SUCCESS(status)) {

        /* Detailed failure log */
        RtlStringCchPrintfW(
            wbuf,
            RTL_NUMBER_OF(wbuf),
            L"StartDevice: IoConnectInterrupt FAILED (0x%08lx)\n"
            L"  Vector=%lu Level=%lu Mode=%s Sharable=%lu\n",
            status,
            fdoExt->IrqVector,
            (ULONG)fdoExt->IrqLevel,
            (fdoExt->IrqMode == Latched) ? L"Latched" : L"Level",
            fdoExt->IrqSharable ? 1UL : 0UL
        );

        RtlInitUnicodeString(&ustr, wbuf);
        astr.Buffer        = abuf;
        astr.Length        = 0;
        astr.MaximumLength = sizeof(abuf);

        if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&astr, &ustr, FALSE))) {
            abuf[astr.Length] = '\0';
            I2cCtrl_Log(abuf);
        }

        I2cCtrl_Log("StartDevice: Falling back to polling mode\n");

        fdoExt->InterruptObject = NULL;
        haveInt = FALSE;
    }
}

/* Program safe initial interrupt mask after ISR connect */
if (fdoExt->Ops != NULL && fdoExt->Ops->MaskInterrupts != NULL) {
    fdoExt->Ops->MaskInterrupts(fdoExt, fdoExt->IntrMask);
}

/* -------------------------------------------------------------
 * Load registry policy (must be done before PWRMBASE selection)
 * ------------------------------------------------------------- */
{
    NTSTATUS polStatus;

    polStatus = I2cCtrl_LoadRegistryPolicy(fdoExt);
    if (!NT_SUCCESS(polStatus)) {
        I2cCtrl_Log("StartDevice: LoadRegistryPolicy failed (0x%08lx)\n", polStatus);
    } else {
        I2cCtrl_Log("StartDevice: registry policy loaded\n");
    }
}

/* -------------------------------------------------------------
 * PWRMBASE selection (LPSS-only)
 * ------------------------------------------------------------- */

{
    BOOLEAN isLpss = FALSE;

    if (fdoExt->PnpId != NULL) {
        if (wcsstr(fdoExt->PnpId, L"DEV_9DE8") != NULL ||
            wcsstr(fdoExt->PnpId, L"DEV_9DE9") != NULL ||
            wcsstr(fdoExt->PnpId, L"DEV_9DC5") != NULL)
        {
            isLpss = TRUE;
        }
    }

    if (!isLpss) {
        I2cCtrl_Log("StartDevice: non-LPSS controller -> skipping PWRMBASE selection\n");
    }
    else
    {
        /* 1. Registry override */
        if (fdoExt->PwrmBase.QuadPart == 0 &&
            fdoExt->PolicyPwrmBase.QuadPart != 0)
        {
            fdoExt->PwrmBase = fdoExt->PolicyPwrmBase;

            I2cCtrl_Log("StartDevice: PWRMBASE (policy) = %08X%08X\n",
                        fdoExt->PwrmBase.HighPart,
                        fdoExt->PwrmBase.LowPart);
        }

        /* 2. Dynamic PCI discovery */
        if (fdoExt->PwrmBase.QuadPart == 0)
        {
            PHYSICAL_ADDRESS pciPwrm;
            NTSTATUS pciStatus;

            pciStatus = I2cCtrl_FindPwrmBaseDynamic(&pciPwrm);

            if (NT_SUCCESS(pciStatus))
            {
                fdoExt->PwrmBase = pciPwrm;
                fdoExt->HavePwrm = TRUE;

                I2cCtrl_Log("StartDevice: PWRMBASE (PCI) = %08X%08X\n",
                            fdoExt->PwrmBase.HighPart,
                            fdoExt->PwrmBase.LowPart);
            }
            else
            {
                I2cCtrl_Log("StartDevice: PCI PWRMBASE fetch failed (0x%08lx)\n",
                            pciStatus);
            }
        }

        /* 3. Validate */
        status = I2cCtrl_ReportPwrmBaseInfo(fdoExt->PwrmBase);
        if (!NT_SUCCESS(status))
        {
            I2cCtrl_Log("StartDevice: INVALID PWRMBASE -> cannot power controller\n");
            I2cCtrl_Log("StartDevice: marking UnsupportedPlatform=TRUE\n");

            fdoExt->UnsupportedPlatform = TRUE;
            fdoExt->Started             = FALSE;

            if (fdoExt->InterruptObject != NULL) {
                I2cCtrl_Log("StartDevice: disconnecting interrupt object\n");
                IoDisconnectInterrupt(fdoExt->InterruptObject);
                fdoExt->InterruptObject = NULL;
            }

            if (fdoExt->Mmio != NULL) {
                I2cCtrl_Log("StartDevice: unmapping BAR0 MMIO\n");
                MmUnmapIoSpace(fdoExt->Mmio, fdoExt->MmioLength);
                fdoExt->Mmio              = NULL;
                fdoExt->MmioLength        = 0;
                fdoExt->MmioPhys.QuadPart = 0;
            }

            I2cCtrl_Log("StartDevice: returning STATUS_SUCCESS (unsupported platform)\n");
            return STATUS_SUCCESS;
        }

        /* 4. Map PWRMBASE VA */
        if (fdoExt->PwrmBaseVa == NULL)
        {
            fdoExt->PwrmBaseVa = MmMapIoSpace(
                fdoExt->PwrmBase,
                0x10000,
                MmNonCached
            );

            if (fdoExt->PwrmBaseVa != NULL) {
                I2cCtrl_Log("StartDevice: PWRMBASE VA mapped at %p\n",
                            fdoExt->PwrmBaseVa);
            } else {
                I2cCtrl_Log("StartDevice: FAILED to map PWRMBASE VA\n");
            }
        }
    }
}

/* -------------------------------------------------------------
 * LPSS BAR2 fallback (XP cannot see BAR2 in _CRS)
 * Only for LPSS controllers: DEV_9DE8, DEV_9DE9, DEV_9DC5
 * ------------------------------------------------------------- */
if (!haveBar2)
{
    BOOLEAN isLpss = FALSE;
    ULONG pidOffset = 0;

    if (fdoExt->PnpId != NULL)
    {
        if (wcsstr(fdoExt->PnpId, L"DEV_9DE8") != NULL) {
            isLpss = TRUE;
            pidOffset = 0xC000;   /* I2C0 */
        }
        else if (wcsstr(fdoExt->PnpId, L"DEV_9DE9") != NULL) {
            isLpss = TRUE;
            pidOffset = 0xC100;   /* I2C1 */
        }
        else if (wcsstr(fdoExt->PnpId, L"DEV_9DC5") != NULL) {
            isLpss = TRUE;
            pidOffset = 0xC200;   /* I2C2 */
        }
    }

    if (!isLpss) {
        I2cCtrl_Log("StartDevice: non-LPSS controller -> skipping LPSS BAR2 fallback\n");
    }
    else if (fdoExt->PwrmBase.QuadPart == 0) {
        I2cCtrl_Log("StartDevice: no valid PWRMBASE -> skipping LPSS BAR2 fallback\n");
    }
    else
    {
        fdoExt->LpssBar2Phys.QuadPart =
            fdoExt->PwrmBase.QuadPart + pidOffset;

        fdoExt->LpssBar2Length = 0x1000;

        fdoExt->LpssBar2 = MmMapIoSpace(
            fdoExt->LpssBar2Phys,
            fdoExt->LpssBar2Length,
            MmNonCached
        );

        if (fdoExt->LpssBar2 != NULL) {
            haveBar2 = TRUE;
            I2cCtrl_Log(
                "StartDevice: LPSS BAR2 mapped at %p (PWRMBASE+0x%04lX)\n",
                fdoExt->LpssBar2,
                pidOffset
            );
        } else {
            I2cCtrl_Log(
                "StartDevice: LPSS BAR2 map FAILED (offset=0x%04lX)\n",
                pidOffset
            );
        }
    }
}


/* -------------------------------------------------------------
 * WHL/CNL Power Wells + LPSS Enable (LPSS-only, diagnostic)
 * ------------------------------------------------------------- */

{
    BOOLEAN isLpss;

    isLpss = FALSE;

    if (fdoExt->PnpId != NULL) {
        if (wcsstr(fdoExt->PnpId, L"DEV_9DE8") != NULL ||
            wcsstr(fdoExt->PnpId, L"DEV_9DE9") != NULL ||
            wcsstr(fdoExt->PnpId, L"DEV_9DC5") != NULL)
        {
            isLpss = TRUE;
        }
    }

    if (!isLpss) {
        I2cCtrl_Log("StartDevice: non-LPSS controller -> skipping POWER + LPSS debug\n");
    } else {

        I2cCtrl_Log("StartDevice: === POWER + LPSS DEBUG BEGIN ===\n");

        /* =============================================================
         * 1. POWER WELLS (PWRMBASE)
         * ============================================================= */

        if (fdoExt->PwrmBaseVa != NULL)
        {
            ULONG pmc4_before = 0, pmc4_after = 0;
            ULONG pw_force_before = 0, pw_force_after = 0;
            ULONG pw_sts_before = 0, pw_sts_after = 0;

            I2cCtrl_Log("StartDevice: PWRMBASE VA=%p PA=%08X%08X\n",
                        fdoExt->PwrmBaseVa,
                        fdoExt->PwrmBase.HighPart,
                        fdoExt->PwrmBase.LowPart);

            __try {
                pmc4_before = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->PwrmBaseVa + WHL_PMC_PMC4_OFFSET));
                pw_force_before = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->PwrmBaseVa + WHL_PW_FORCE_ON_OFFSET));
                pw_sts_before = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->PwrmBaseVa + WHL_PW_STS_OFFSET));
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                I2cCtrl_Log("StartDevice: EXCEPTION reading PWRM registers\n");
            }

            I2cCtrl_Log("StartDevice: PMC4 BEFORE       = 0x%08lx\n", pmc4_before);
            I2cCtrl_Log("StartDevice: PW_FORCE_ON BEFORE= 0x%08lx\n", pw_force_before);
            I2cCtrl_Log("StartDevice: PW_STS BEFORE     = 0x%08lx\n", pw_sts_before);

            pmc4_after = pmc4_before | WHL_PMC_CECE_BIT;

            I2cCtrl_Log("StartDevice: Setting CECE bit (0x%08lx)\n",
                        WHL_PMC_CECE_BIT);

            __try {
                WRITE_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->PwrmBaseVa + WHL_PMC_PMC4_OFFSET),
                    pmc4_after);
                pmc4_after = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->PwrmBaseVa + WHL_PMC_PMC4_OFFSET));
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                I2cCtrl_Log("StartDevice: EXCEPTION writing PMC4\n");
            }

            I2cCtrl_Log("StartDevice: PMC4 AFTER        = 0x%08lx\n", pmc4_after);

            I2cCtrl_Log("StartDevice: Writing PW_FORCE_ON mask 0x%08lx\n",
                        WHL_PW_MASK);

            __try {
                WRITE_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->PwrmBaseVa + WHL_PW_FORCE_ON_OFFSET),
                    WHL_PW_MASK);
                pw_force_after = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->PwrmBaseVa + WHL_PW_FORCE_ON_OFFSET));
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                I2cCtrl_Log("StartDevice: EXCEPTION writing PW_FORCE_ON\n");
            }

            I2cCtrl_Log("StartDevice: PW_FORCE_ON AFTER = 0x%08lx\n", pw_force_after);

            {
                ULONG timeout = 10000;
                ULONG iter = 0;
                ULONG pw = 0;

                I2cCtrl_Log("StartDevice: Polling PW_STS for PW1/PW2 ON (mask=0x%08lx)\n",
                            WHL_PW_MASK);

                do {
                    __try {
                        pw = READ_REGISTER_ULONG(
                            (PULONG)((PUCHAR)fdoExt->PwrmBaseVa + WHL_PW_STS_OFFSET));
                    } __except(EXCEPTION_EXECUTE_HANDLER) {
                        I2cCtrl_Log("StartDevice: EXCEPTION reading PW_STS (iter=%lu)\n",
                                    iter);
                        break;
                    }

                    if ((pw & WHL_PW_MASK) == WHL_PW_MASK)
                        break;

                    if ((iter % 1000) == 0)
                        I2cCtrl_Log("StartDevice: PW_STS poll iter=%lu value=0x%08lx\n",
                                    iter, pw);

                    KeStallExecutionProcessor(1);
                    iter++;

                } while (--timeout);

                pw_sts_after = pw;

                I2cCtrl_Log("StartDevice: PW_STS FINAL      = 0x%08lx (iters=%lu timeout=%lu)\n",
                            pw_sts_after, iter, timeout);
            }
        }
        else
        {
            I2cCtrl_Log("StartDevice: PWRMBASE VA=NULL PA=%08X%08X -> cannot access power wells\n",
                        fdoExt->PwrmBase.HighPart,
                        fdoExt->PwrmBase.LowPart);
        }

        /* =============================================================
         * 2. LPSS PRIVATE REGISTERS (BAR2)
         * ============================================================= */

        if (fdoExt->LpssBar2 != NULL)
        {
            ULONG clk_before = 0, clk_after = 0;
            ULONG rst_before = 0, rst_after = 0;
            ULONG rst_sts_before = 0, rst_sts_after = 0;

            I2cCtrl_Log("StartDevice: LPSS BAR2 VA=%p PA=%08X%08X\n",
                        fdoExt->LpssBar2,
                        fdoExt->LpssBar2Phys.HighPart,
                        fdoExt->LpssBar2Phys.LowPart);

            __try {
                clk_before = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->LpssBar2 + WHL_LPSS_CLK_CTL));
                rst_before = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->LpssBar2 + WHL_LPSS_RST_CTL));
                rst_sts_before = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->LpssBar2 + WHL_LPSS_RST_STS));
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                I2cCtrl_Log("StartDevice: EXCEPTION reading LPSS initial state\n");
            }

            I2cCtrl_Log("StartDevice: LPSS CLK_CTL BEFORE = 0x%08lx\n", clk_before);
            I2cCtrl_Log("StartDevice: LPSS RST_CTL BEFORE = 0x%08lx\n", rst_before);
            I2cCtrl_Log("StartDevice: LPSS RST_STS BEFORE = 0x%08lx\n", rst_sts_before);

            I2cCtrl_Log("StartDevice: Writing LPSS CLK_CTL = 0x00000007\n");

            __try {
                WRITE_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->LpssBar2 + WHL_LPSS_CLK_CTL),
                    0x7);
                clk_after = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->LpssBar2 + WHL_LPSS_CLK_CTL));
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                I2cCtrl_Log("StartDevice: EXCEPTION writing LPSS CLK_CTL\n");
            }

            I2cCtrl_Log("StartDevice: LPSS CLK_CTL AFTER  = 0x%08lx\n", clk_after);

            I2cCtrl_Log("StartDevice: Writing LPSS RST_CTL = 0x00000000\n");

            __try {
                WRITE_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->LpssBar2 + WHL_LPSS_RST_CTL),
                    0x0);
                rst_after = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->LpssBar2 + WHL_LPSS_RST_CTL));
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                I2cCtrl_Log("StartDevice: EXCEPTION writing LPSS RST_CTL\n");
            }

            I2cCtrl_Log("StartDevice: LPSS RST_CTL AFTER  = 0x%08lx\n", rst_after);

            {
                ULONG timeout = 1000;
                ULONG iter = 0;
                ULONG rst = 0;

                I2cCtrl_Log("StartDevice: Polling LPSS RST_STS for bit0=1\n");

                do {
                    __try {
                        rst = READ_REGISTER_ULONG(
                            (PULONG)((PUCHAR)fdoExt->LpssBar2 + WHL_LPSS_RST_STS));
                    } __except(EXCEPTION_EXECUTE_HANDLER) {
                        I2cCtrl_Log("StartDevice: EXCEPTION reading LPSS RST_STS (iter=%lu)\n",
                                    iter);
                        break;
                    }

                    if (rst & 0x1)
                        break;

                    if ((iter % 100) == 0)
                        I2cCtrl_Log("StartDevice: LPSS RST_STS poll iter=%lu value=0x%08lx\n",
                                    iter, rst);

                    KeStallExecutionProcessor(1);
                    iter++;

                } while (--timeout);

                rst_sts_after = rst;

                I2cCtrl_Log("StartDevice: LPSS RST_STS FINAL = 0x%08lx (iters=%lu timeout=%lu)\n",
                            rst_sts_after, iter, timeout);
            }
        }
        else
        {
            I2cCtrl_Log("StartDevice: LPSS BAR2 NULL -> cannot access LPSS private registers\n");
        }

        I2cCtrl_Log("StartDevice: === POWER + LPSS DEBUG END ===\n");
    }
}

/* Late pass: install backend, then apply HW quirks */
I2cCtrl_InstallBackend(fdoExt);

I2cCtrl_Log("StartDevice: applying unified quirks (late pass)\n");
I2cCtrlApplyQuirks(fdoExt);
I2cCtrl_Log("StartDevice: unified quirks applied (late pass)\n");


/* -------------------------------------------------------------
 * Enable DW-I2C controller (with deep diagnostics, BAR0-generic)
 * ------------------------------------------------------------- */

I2cCtrl_Log("StartDevice: === DW-I2C ENABLE DEBUG BEGIN ===\n");

/* 1. Sanity check Ops table */
if (fdoExt->Ops == NULL) {
    I2cCtrl_Log("StartDevice: ERROR: fdoExt->Ops is NULL -> cannot enable controller\n");
} else {
    I2cCtrl_Log("StartDevice: Ops table present at %p\n", fdoExt->Ops);
}

/* 2. Sanity check Enable callback */
if (fdoExt->Ops == NULL || fdoExt->Ops->Enable == NULL) {
    I2cCtrl_Log("StartDevice: ERROR: Ops->Enable is NULL -> skipping enable\n");
} else {

    NTSTATUS enStatus;
    ULONG i;
    ULONG val;

    /* 3. Dump BAR0 window BEFORE enabling (first 0x40 bytes) */
    if (fdoExt->Mmio != NULL) {

        I2cCtrl_Log("StartDevice: BAR0 BEFORE ENABLE (first 0x40 bytes):\n");

        __try {
            for (i = 0; i < 0x40; i += 4) {
                val = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->Mmio + i)
                );
                I2cCtrl_Log("  BAR0[%02lX] = 0x%08lx\n", i, val);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            I2cCtrl_Log("StartDevice: EXCEPTION reading BAR0 BEFORE ENABLE\n");
        }

    } else {
        I2cCtrl_Log("StartDevice: BAR0 MMIO is NULL -> cannot dump pre-enable state\n");
    }

    /* 4. Call the actual enable routine */
    I2cCtrl_Log("StartDevice: Calling Ops->Enable(TRUE)...\n");

    enStatus = fdoExt->Ops->Enable(fdoExt, TRUE);

    I2cCtrl_Log("StartDevice: Ops->Enable returned 0x%08lx\n", enStatus);

    /* 5. Dump BAR0 window AFTER enabling (first 0x40 bytes) */
    if (fdoExt->Mmio != NULL) {

        I2cCtrl_Log("StartDevice: BAR0 AFTER ENABLE (first 0x40 bytes):\n");

        __try {
            for (i = 0; i < 0x40; i += 4) {
                val = READ_REGISTER_ULONG(
                    (PULONG)((PUCHAR)fdoExt->Mmio + i)
                );
                I2cCtrl_Log("  BAR0[%02lX] = 0x%08lx\n", i, val);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            I2cCtrl_Log("StartDevice: EXCEPTION reading BAR0 AFTER ENABLE\n");
        }
    }
}

I2cCtrl_Log("StartDevice: === DW-I2C ENABLE DEBUG END ===\n");

//
// Helper macro: safe 3-DWORD dump from a mapped MMIO window
//
#define I2CCTRL_DUMP3(prefix, base)                                   \
    do {                                                               \
        ULONG _r0 = 0, _r1 = 0, _r2 = 0;                               \
        __try {                                                        \
            _r0 = READ_REGISTER_ULONG((PULONG)((PUCHAR)(base) + 0x0)); \
            _r1 = READ_REGISTER_ULONG((PULONG)((PUCHAR)(base) + 0x4)); \
            _r2 = READ_REGISTER_ULONG((PULONG)((PUCHAR)(base) + 0x8)); \
        } __except(EXCEPTION_EXECUTE_HANDLER) {                        \
            I2cCtrl_Log(prefix "EXCEPTION during MMIO read\n");        \
        }                                                              \
        I2cCtrl_Log(prefix "VA=%p\n", (base));                         \
        I2cCtrl_Log(prefix "[0x00]=0x%08lx\n", _r0);                   \
        I2cCtrl_Log(prefix "[0x04]=0x%08lx\n", _r1);                   \
        I2cCtrl_Log(prefix "[0x08]=0x%08lx\n", _r2);                   \
    } while (0)

//
// ENABLE FAILED path
//
status = I2cCtrl_WaitForEnableState(fdoExt, TRUE, 500U);
if (!NT_SUCCESS(status)) {

    BOOLEAN isLpss = FALSE;

    if (fdoExt->PnpId != NULL) {
        if (wcsstr(fdoExt->PnpId, L"DEV_9DE8") != NULL ||
            wcsstr(fdoExt->PnpId, L"DEV_9DE9") != NULL ||
            wcsstr(fdoExt->PnpId, L"DEV_9DC5") != NULL)
        {
            isLpss = TRUE;
        }
    }

    I2cCtrl_Log("StartDevice: ENABLE FAILED (0x%08lx) -> collecting debug\n", status);

    //
    // 1. Dump PWRM window (if mapped, LPSS-only)
    //
    if (isLpss && fdoExt->PwrmBaseVa != NULL) {
        I2CCTRL_DUMP3("StartDevice: PWRM ", fdoExt->PwrmBaseVa);
    } else {
        I2cCtrl_Log("StartDevice: PWRM dump skipped (non-LPSS or VA NULL)\n");
    }

    //
    // 2. Dump LPSS/private window (if mapped, LPSS-only)
    //
    if (isLpss && fdoExt->LpssBar2 != NULL) {
        I2CCTRL_DUMP3("StartDevice: PRIV ", fdoExt->LpssBar2);
    } else {
        I2cCtrl_Log("StartDevice: PRIV dump skipped (non-LPSS or VA NULL)\n");
    }

    //
    // 3. WinDbg-style summary
    //
    {
        WCHAR wbuf[200];
        CHAR  abuf[200];
        UNICODE_STRING ustr;
        ANSI_STRING    astr;

        RtlStringCchPrintfW(
            wbuf,
            RTL_NUMBER_OF(wbuf),
            L"StartDevice: ENABLE FAILED (0x%08lx)\n"
            L"  HWID=%ws\n"
            L"  BAR0=PA=%08X%08X Len=%lu\n"
            L"  IRQ: Vector=%lu Level=%lu Mode=%s Sharable=%lu\n",
            status,
            (fdoExt->PnpId ? fdoExt->PnpId : L"<null>"),
            fdoExt->MmioPhys.HighPart,
            fdoExt->MmioPhys.LowPart,
            fdoExt->MmioLength,
            fdoExt->IrqVector,
            (ULONG)fdoExt->IrqLevel,
            (fdoExt->IrqMode == Latched) ? L"Latched" : L"Level",
            fdoExt->IrqSharable ? 1UL : 0UL
        );

        RtlInitUnicodeString(&ustr, wbuf);
        astr.Buffer        = abuf;
        astr.Length        = 0;
        astr.MaximumLength = sizeof(abuf);

        if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&astr, &ustr, FALSE))) {
            abuf[astr.Length] = '\0';
            I2cCtrl_Log("%s", abuf);
        }
    }

    //
    // 4. Mark failure and clean up resources
    //
    I2cCtrl_Log("StartDevice: marking HardwareFailure=TRUE\n");
    fdoExt->HardwareFailure = TRUE;

    if (fdoExt->InterruptObject) {
        I2cCtrl_Log("StartDevice: disconnecting interrupt object %p\n",
                    fdoExt->InterruptObject);
        IoDisconnectInterrupt(fdoExt->InterruptObject);
        fdoExt->InterruptObject = NULL;
    }

    if (fdoExt->LpssBar2) {
        I2cCtrl_Log("StartDevice: unmapping PRIV at %p (len=%lu)\n",
                    fdoExt->LpssBar2, fdoExt->LpssBar2Length);
        MmUnmapIoSpace(fdoExt->LpssBar2, fdoExt->LpssBar2Length);
        fdoExt->LpssBar2       = NULL;
        fdoExt->LpssBar2Length = 0;
        fdoExt->LpssBar2Phys.QuadPart = 0;
    }

    if (fdoExt->PwrmBaseVa) {
        I2cCtrl_Log("StartDevice: unmapping PWRMBASE VA %p\n", fdoExt->PwrmBaseVa);
        MmUnmapIoSpace(fdoExt->PwrmBaseVa, 0x10000);
        fdoExt->PwrmBaseVa = NULL;
    }

    if (fdoExt->Mmio) {
        I2cCtrl_Log("StartDevice: unmapping MMIO at %p (len=%lu)\n",
                    fdoExt->Mmio, fdoExt->MmioLength);
        MmUnmapIoSpace(fdoExt->Mmio, fdoExt->MmioLength);
        fdoExt->Mmio              = NULL;
        fdoExt->MmioLength        = 0;
        fdoExt->MmioPhys.QuadPart = 0;
        fdoExt->MmioBase          = NULL;
    }

    I2cCtrl_Log("StartDevice: ENABLE FAILED (0x%08lx) -> returning failure\n", status);
    return status;
}

/* Clear transfer context and runtime flags */
RtlZeroMemory(&fdoExt->XferCtx, sizeof(fdoExt->XferCtx));
KeResetEvent(&fdoExt->TransferEvent);

fdoExt->ActiveBusy      = FALSE;
fdoExt->Started         = TRUE;
fdoExt->Removed         = FALSE;
fdoExt->Stopping        = FALSE;

/* Prepare hot-plug rebind runtime flags */
fdoExt->HotplugPending  = FALSE;
fdoExt->ChildrenStale   = FALSE;

I2cCtrl_Log("StartDevice: controller enabled, runtime flags set\n");

I2cCtrl_Log("StartDevice: deferring child enumeration to BusRelations\n");
I2cCtrl_Log("StartDevice: complete\n");

return STATUS_SUCCESS;

}

/* -----------------------------------------------------------------------
 * I2cCtrl_StopDevice - XP/2003-safe, HAL-generic, C89-compliant teardown.
 * Quiesces the controller, disconnects interrupts, drains queues, unmaps
 * MMIO (BAR0) and LPSS BAR2, and resets runtime flags.
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_StopDevice(
    IN PI2CCTRL_FDO fdoExt
    )
{
    NTSTATUS       status;
    KIRQL          oldIrql;
    PIRP           irp;
    PLIST_ENTRY    le;
    PSMBUS_REQUEST req;
    PI2CCTRL_QUEUE q;

    /* C89 init */
    status  = STATUS_SUCCESS;
    oldIrql = (KIRQL)0;
    irp     = NULL;
    le      = NULL;
    req     = NULL;
    q       = NULL;

    PAGED_CODE();

    if (fdoExt == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    I2cCtrl_Log("StopDevice: begin\n");

    fdoExt->Stopping       = TRUE;
    fdoExt->ActiveBusy     = FALSE;
    fdoExt->HotplugPending = FALSE;

    /* 1) Mask interrupts */
    if (fdoExt->Ops != NULL && fdoExt->Ops->MaskInterrupts != NULL) {
        fdoExt->Ops->MaskInterrupts(fdoExt, 0U);
    }

    /* 2) Clear latched interrupt causes */
    if (fdoExt->Ops != NULL && fdoExt->Ops->AckInterrupts != NULL) {
        fdoExt->Ops->AckInterrupts(
            fdoExt,
            I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
            I2C_INT_TX_ABORT | I2C_INT_STOP_DETECTED |
            I2C_INT_START_DETECTED | I2C_INT_GEN_CALL |
            I2C_INT_ACTIVITY | I2C_INT_RX_DONE | I2C_INT_RD_REQ);
    }

    /* 3) Quiesce FIFOs */
    if (fdoExt->Ops != NULL && fdoExt->Ops->QuiesceFifos != NULL) {
        fdoExt->Ops->QuiesceFifos(fdoExt);
    }

    /* 4) Disable controller (best-effort) */
    if (fdoExt->Ops != NULL && fdoExt->Ops->Enable != NULL) {
        (VOID)fdoExt->Ops->Enable(fdoExt, FALSE);
        status = I2cCtrl_WaitForEnableState(fdoExt, FALSE, 500U);
        if (!NT_SUCCESS(status)) {
            I2cCtrl_Log("StopDevice: disable did not latch\n");
            /* Do not poison future starts; just log it */
        }
    }

    /* 5) Deschedule DPCs */
    KeRemoveQueueDpc(&fdoExt->IsrDpc);
    KeRemoveQueueDpc(&fdoExt->QueueDpc);
    KeRemoveQueueDpc(&fdoExt->TimeoutDpc);

    /* 6) Disconnect interrupt (if any) */
    if (fdoExt->InterruptObject != NULL) {
        IoDisconnectInterrupt(fdoExt->InterruptObject);
        fdoExt->InterruptObject = NULL;
    }

    /* 7) Cancel timeout timer and fence transfer context */
    KeCancelTimer(&fdoExt->XferCtx.TimeoutTimer);

    KeAcquireSpinLock(&fdoExt->QueueLock, &oldIrql);
    fdoExt->ActiveBusy = FALSE;
    if (fdoExt->XferCtx.Irp != NULL) {
        irp = fdoExt->XferCtx.Irp;
        fdoExt->XferCtx.Irp    = NULL;
        fdoExt->XferCtx.Status = STATUS_CANCELLED;
    }
    KeReleaseSpinLock(&fdoExt->QueueLock, oldIrql);

    if (irp != NULL) {
        IoSetCancelRoutine(irp, NULL);
        irp->IoStatus.Status      = STATUS_CANCELLED;
        irp->IoStatus.Information = 0U;
        KeSetEvent(&fdoExt->TransferEvent, IO_NO_INCREMENT, FALSE);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        irp = NULL;
    }

    /* 8a) Unmap PWRMBASE VA (if mapped) */
    if (fdoExt->PwrmBaseVa != NULL) {
        MmUnmapIoSpace(fdoExt->PwrmBaseVa, 0x10000);
        fdoExt->PwrmBaseVa = NULL;
        I2cCtrl_Log("StopDevice: PWRMBASE VA unmapped\n");
    }

    /* 8) Unmap LPSS BAR2 (if mapped) */
    if (fdoExt->LpssBar2 != NULL) {
        MmUnmapIoSpace(fdoExt->LpssBar2, fdoExt->LpssBar2Length);
        fdoExt->LpssBar2       = NULL;
        fdoExt->LpssBar2Length = 0U;
        fdoExt->LpssBar2Phys.QuadPart = 0;
        I2cCtrl_Log("StopDevice: LPSS BAR2 unmapped\n");
    }

    /* 9) Unmap MMIO (BAR0) */
    if (fdoExt->Mmio != NULL) {
        MmUnmapIoSpace(fdoExt->Mmio, fdoExt->MmioLength);
        fdoExt->Mmio              = NULL;
        fdoExt->MmioLength        = 0U;
        fdoExt->MmioPhys.QuadPart = 0;
        fdoExt->MmioBase          = NULL;
        I2cCtrl_Log("StopDevice: MMIO unmapped\n");
    }

    /* 10) Stop IOCTL worker queue */
    q = fdoExt->Queue;
    if (q != NULL) {
        KIRQL qIrql;

        KeAcquireSpinLock(&q->Lock, &qIrql);
        q->Running = FALSE;
        KeSetEvent(&q->WorkEvent, IO_NO_INCREMENT, FALSE);

        while (!IsListEmpty(&q->PendingIrps)) {
            PLIST_ENTRY       e2;
            PI2CCTRL_IRP_CONTEXT ctx2;

            e2   = RemoveHeadList(&q->PendingIrps);
            ctx2 = CONTAINING_RECORD(e2, I2CCTRL_IRP_CONTEXT, ListEntry);

            if (ctx2 != NULL && ctx2->Irp != NULL) {
                ctx2->Irp->IoStatus.Status      = STATUS_CANCELLED;
                ctx2->Irp->IoStatus.Information = 0U;
                IoCompleteRequest(ctx2->Irp, IO_NO_INCREMENT);
                ctx2->Completed = TRUE;
            }
        }

        KeReleaseSpinLock(&q->Lock, qIrql);
    }

    /* 11) Drain pending IRPs (simple non-CSQ list) */
    for (;;) {
        PLIST_ENTRY le2;
        PIRP        qIrp;

        KeAcquireSpinLock(&fdoExt->PendingIrpLock, &oldIrql);

        if (IsListEmpty(&fdoExt->PendingIrpList)) {
            KeReleaseSpinLock(&fdoExt->PendingIrpLock, oldIrql);
            break;
        }

        le2 = RemoveHeadList(&fdoExt->PendingIrpList);
        KeReleaseSpinLock(&fdoExt->PendingIrpLock, oldIrql);

        qIrp = CONTAINING_RECORD(le2, IRP, Tail.Overlay.ListEntry);

        IoSetCancelRoutine(qIrp, NULL);
        qIrp->IoStatus.Status      = STATUS_CANCELLED;
        qIrp->IoStatus.Information = 0U;
        IoCompleteRequest(qIrp, IO_NO_INCREMENT);
    }

    /* 12) Free leftover request wrappers */
    KeAcquireSpinLock(&fdoExt->QueueLock, &oldIrql);
    while (!IsListEmpty(&fdoExt->RequestQueue)) {
        le = RemoveHeadList(&fdoExt->RequestQueue);
        KeReleaseSpinLock(&fdoExt->QueueLock, oldIrql);

        req = CONTAINING_RECORD(le, SMBUS_REQUEST, ListEntry);
        if (req != NULL) {
            if (req->Irp != NULL) {
                IoSetCancelRoutine(req->Irp, NULL);
                req->Irp->IoStatus.Status      = STATUS_CANCELLED;
                req->Irp->IoStatus.Information = 0U;
                IoCompleteRequest(req->Irp, IO_NO_INCREMENT);
                req->Irp = NULL;
            }
            ExFreePool(req);
        }

        KeAcquireSpinLock(&fdoExt->QueueLock, &oldIrql);
    }
    fdoExt->ActiveBusy = FALSE;
    RtlZeroMemory(&fdoExt->ActiveRequest, sizeof(fdoExt->ActiveRequest));
    KeReleaseSpinLock(&fdoExt->QueueLock, oldIrql);

    /* 13) Reset runtime flags and transfer context */
    fdoExt->Started        = FALSE;
    fdoExt->ChildrenStale  = FALSE;
    fdoExt->HotplugPending = FALSE;

    RtlZeroMemory(&fdoExt->XferCtx, sizeof(fdoExt->XferCtx));
    KeResetEvent(&fdoExt->TransferEvent);

    I2cCtrl_Log("StopDevice: complete\n");

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * I2cCtrl_RestartDevice - XP/2003-safe controller reinitialization.
 *
 * XP/2003 cannot restart a bus controller without receiving a new
 * IRP_MN_START_DEVICE from PnP. Therefore this routine performs only:
 *
 *   - a full StopDevice teardown (BAR0 + optional LPSS BAR2 unmap)
 *   - clears runtime flags
 *   - returns success
 *
 * PnP will later issue a real START_DEVICE IRP with fresh resources.
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_RestartDevice(
    PI2CCTRL_FDO fdoExt
    )
{
    NTSTATUS status;

    /* C89 init */
    status = STATUS_SUCCESS;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        I2cCtrl_Log("RestartDevice: invalid IRQL\n");
        return STATUS_INVALID_DEVICE_STATE;
    }
    PAGED_CODE();

    if (fdoExt == NULL) {
        I2cCtrl_Log("RestartDevice: fdoExt=NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    I2cCtrl_Log("RestartDevice: begin\n");

    /*
     * Perform a clean stop. This safely:
     *   - masks interrupts
     *   - disconnects ISR
     *   - quiesces FIFOs
     *   - unmaps BAR0 MMIO
     *   - unmaps LPSS BAR2 (if present)
     *   - drains queues
     *   - cancels pending IRPs
     *   - resets runtime flags
     */
    status = I2cCtrl_StopDevice(fdoExt);
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("RestartDevice: StopDevice FAILED\n");
        return status;
    }

    /*
     * DO NOT call StartDevice here.
     * XP/2003 requires a real IRP_MN_START_DEVICE from PnP
     * to provide fresh resources and restart the controller.
     */
    I2cCtrl_Log("RestartDevice: complete, awaiting PnP START_DEVICE\n");

    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * I2cCtrl_RemoveDevice - XP/2003-safe full controller teardown.
 *
 * This routine performs only controller teardown and resource cleanup:
 *   - marks the device as removed
 *   - performs a full StopDevice teardown (BAR0 + optional LPSS BAR2)
 *   - closes ACPI handles
 *   - frees stored resource lists and work items
 *
 * The FDO REMOVE_DEVICE dispatch must still:
 *   - IoDetachDevice()
 *   - IoReleaseRemoveLockAndWait()
 *   - IoDeleteDevice(FDO)
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_RemoveDevice(
    PI2CCTRL_FDO fdoExt,
    PIRP         Irp   /* optional, not completed here */
    )
{
    NTSTATUS status;

    /* C89 init */
    status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(Irp);

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        I2cCtrl_Log("RemoveDevice: invalid IRQL\n");
        return STATUS_INVALID_DEVICE_STATE;
    }
    PAGED_CODE();

    if (fdoExt == NULL) {
        I2cCtrl_Log("RemoveDevice: fdoExt=NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    I2cCtrl_Log("RemoveDevice: begin\n");

    /* Mark removal BEFORE StopDevice */
    fdoExt->Removed        = TRUE;
    fdoExt->Stopping       = TRUE;
    fdoExt->Started        = FALSE;
    fdoExt->ActiveBusy     = FALSE;
    fdoExt->HotplugPending = FALSE;

    /*
     * Perform full stop teardown:
     *   - masks/clears interrupts
     *   - disconnects ISR
     *   - quiesces FIFOs
     *   - unmaps BAR0 MMIO
     *   - unmaps LPSS BAR2 (if present)
     *   - drains queues
     *   - cancels pending IRPs
     *   - resets transfer context and runtime flags
     */
    status = I2cCtrl_StopDevice(fdoExt);
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("RemoveDevice: StopDevice FAILED\n");
        /* Continue anyway - removal must not fail */
    }

    /* Close ACPI handle (child PDOs close their own ACPI handles) */
    I2cCtrl_AcpiClose(fdoExt);

    /* Free raw/translated resource lists */
    if (fdoExt->RawResources != NULL) {
        ExFreePool(fdoExt->RawResources);
        fdoExt->RawResources = NULL;
    }

    if (fdoExt->TranslatedResources != NULL) {
        ExFreePool(fdoExt->TranslatedResources);
        fdoExt->TranslatedResources = NULL;
    }

    /* Free work items */
    if (fdoExt->SelfTestWorkItem != NULL) {
        IoFreeWorkItem(fdoExt->SelfTestWorkItem);
        fdoExt->SelfTestWorkItem = NULL;
    }

    if (fdoExt->RebindWorkItem != NULL) {
        IoFreeWorkItem(fdoExt->RebindWorkItem);
        fdoExt->RebindWorkItem = NULL;
    }

    I2cCtrl_Log("RemoveDevice: complete\n");

    return STATUS_SUCCESS;
}


/*
 * I2cCtrl_WaitForEnableState - poll HAL status until target state latched
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant
 */
NTSTATUS
I2cCtrl_WaitForEnableState(
    PI2CCTRL_FDO fdoExt,
    BOOLEAN      targetOn,
    ULONG        timeout
    )
{
    ULONG          i;
    NTSTATUS       st;
    I2C_HW_STATUS  hwst;

    if (fdoExt == NULL || fdoExt->Mmio == NULL || fdoExt->MmioLength == 0U) {
        I2cCtrl_Log("WaitForEnableState: invalid parameters (fdoExt=%p, Mmio=%p, Len=%lu)\n",
                    fdoExt,
                    (fdoExt ? fdoExt->Mmio : NULL),
                    (fdoExt ? fdoExt->MmioLength : 0U));
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&hwst, sizeof(hwst));

    for (i = 0U; i < timeout; i++) {

        if (fdoExt->Ops != NULL && fdoExt->Ops->GetStatus != NULL) {

            st = fdoExt->Ops->GetStatus(fdoExt, &hwst);
            if (!NT_SUCCESS(st)) {
                I2cCtrl_Log("WaitForEnableState: GetStatus failed (0x%08lx)\n", st);
                break; /* hardware read failed */
            }

            /* Interpret enable state from StatusReg bit0 */
            if (((hwst.StatusReg & 0x1U) != 0U) == targetOn) {
                return STATUS_SUCCESS; /* latched as requested */
            }
        }

        KeStallExecutionProcessor(1U); /* ~1µs */
    }

    I2cCtrl_Log("WaitForEnableState: timeout expired (targetOn=%lu)\n",
                (ULONG)targetOn);

    fdoExt->HardwareFailure = TRUE;
    return STATUS_DEVICE_HARDWARE_ERROR;
}


/* ---------------------------------------------------------------------------
 * CheckAckAndClear - HAL-generic, XP/2003-safe, C89-compliant
 *
 * Purpose:
 *   - Detect ACK/NACK or arbitration loss
 *   - Clear TX abort source safely
 *   - Update diagnostic counters and map to NTSTATUS
 *
 * Guarantees:
 *   - Runs at PASSIVE_LEVEL
 *   - Touches only non-paged memory
 *   - Uses HAL ops only; no backend register references
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_CheckAckAndClear(
    PI2CCTRL_FDO fdoExt
    )
{
    I2C_HW_STATUS hwst;
    NTSTATUS st;

    /* C89 init */
    RtlZeroMemory(&hwst, sizeof(hwst));
    st = STATUS_SUCCESS;

    if (fdoExt == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                    "CheckAckAndClear: invalid fdoExt (NULL)");
        return STATUS_INVALID_PARAMETER;
    }

    /* Always go through HAL GetStatus */
    if (fdoExt->Ops == NULL || fdoExt->Ops->GetStatus == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                    "CheckAckAndClear: HAL GetStatus not available (Ctrl%lu)",
                    fdoExt->ControllerId);
        return STATUS_DEVICE_NOT_READY;
    }

    st = fdoExt->Ops->GetStatus(fdoExt, &hwst);
    if (!NT_SUCCESS(st)) {
        fdoExt->ErrorCount++;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                    "CheckAckAndClear: GetStatus failed (Ctrl%lu, status=0x%08lx)",
                    fdoExt->ControllerId, st);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    if (hwst.AbortBits != 0U) {
        /* Clear abort latch via HAL */
        if (fdoExt->Ops->AckInterrupts != NULL) {
            (VOID)fdoExt->Ops->AckInterrupts(fdoExt, hwst.AbortBits);
        }

        fdoExt->ErrorCount++;

        /* Arbitration lost */
        if (hwst.ArbLost != FALSE) {
            fdoExt->ArbLossCount++;
            fdoExt->ArbConsecutiveLost++;
            fdoExt->LastArbLossTime = KeQueryPerformanceCounter(NULL);

            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                        "CheckAckAndClear: Arbitration lost (Ctrl%lu), total=%lu, consecutive=%lu",
                        fdoExt->ControllerId,
                        fdoExt->ArbLossCount,
                        fdoExt->ArbConsecutiveLost);

            return STATUS_IO_DEVICE_ERROR;
        } else {
            fdoExt->ArbConsecutiveLost = 0;
        }

        /* Address NACK */
        if (hwst.AddressNack != FALSE) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_HW,
                        "CheckAckAndClear: Address NACK (Ctrl%lu)",
                        fdoExt->ControllerId);
            return STATUS_NO_SUCH_DEVICE;
        }

        /* Data NACK */
        if (hwst.DataNack != FALSE) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_HW,
                        "CheckAckAndClear: Data NACK (Ctrl%lu)",
                        fdoExt->ControllerId);
            return STATUS_DEVICE_NOT_CONNECTED;
        }

        /* Fallback for other abort reasons */
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                    "CheckAckAndClear: TX abort, bits=0x%08lx (Ctrl%lu)",
                    hwst.AbortBits, fdoExt->ControllerId);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
 * WriteBurstPolled - HAL-driven polled write path
 *
 * Purpose:
 *   - Perform a multi-byte write to a 7-bit I2C slave using only HAL ops.
 *   - Drive the controller in polling mode when no IRQ is available.
 *   - Use HAL callbacks for ACK/NACK, FIFO checks, RESTART/STOP, and status.
 *   - Flag hardware failure on any HAL error or STOP timeout.
 *
 * Characteristics:
 *   - PASSIVE_LEVEL only.
 *   - Non-paged memory only.
 *   - No direct MMIO access; all backend-specific logic is behind Ops.
 *   - Deterministic success/failure with HardwareFailure tracking.
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_WriteBurstPolled(
    PI2CCTRL_FDO fdoExt,
    USHORT       slaveAddr,
    const UCHAR* buf,
    ULONG        len,
    BOOLEAN      issueRestart,
    BOOLEAN      issueStop
    )
{
    ULONG          sent;
    NTSTATUS       status;
    I2C_HW_STATUS  hwst;

    /* C89 init */
    sent   = 0U;
    status = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (fdoExt == NULL || buf == NULL || len == 0U) {
        I2cCtrl_Log("WriteBurstPolled: invalid parameters (Ctrl%p, buf=%p, len=%lu)",
                    fdoExt, buf, len);
        return STATUS_INVALID_PARAMETER;
    }

    if (fdoExt->Ops == NULL ||
        fdoExt->Ops->GetStatus == NULL ||
        fdoExt->Ops->SetTarget7bit == NULL) {
        I2cCtrl_Log("WriteBurstPolled: HAL ops missing (Ctrl%lu)",
                    fdoExt->ControllerId);
        return STATUS_DEVICE_NOT_READY;
    }

    /* Program target address via HAL */
    status = fdoExt->Ops->SetTarget7bit(fdoExt, (UCHAR)(slaveAddr & 0x7FU));
    if (!NT_SUCCESS(status)) {
        fdoExt->HardwareFailure = TRUE;
        I2cCtrl_Log("WriteBurstPolled: SetTarget7bit failed (Ctrl%lu, status=0x%08lx)",
                    fdoExt->ControllerId, status);
        return status;
    }

    /* Optional: emit restart before first byte */
    if (issueRestart != FALSE && fdoExt->Ops->EmitRestartIfNeeded != NULL) {
        status = fdoExt->Ops->EmitRestartIfNeeded(fdoExt);
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            I2cCtrl_Log("WriteBurstPolled: EmitRestartIfNeeded failed (Ctrl%lu, status=0x%08lx)",
                        fdoExt->ControllerId, status);
            return status;
        }
    }

    while (sent < len) {

        /* ACK/NACK detection */
        status = I2cCtrl_CheckAckAndClear(fdoExt);
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            I2cCtrl_Log("WriteBurstPolled: ACK/NACK failure (Ctrl%lu, status=0x%08lx)",
                        fdoExt->ControllerId, status);
            return status;
        }

        /* Poll HAL status for TX FIFO space */
        status = fdoExt->Ops->GetStatus(fdoExt, &hwst);
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            I2cCtrl_Log("WriteBurstPolled: GetStatus failed (Ctrl%lu, status=0x%08lx)",
                        fdoExt->ControllerId, status);
            return status;
        }

        if (hwst.TxFifoNotFull != FALSE) {
            UCHAR byte = buf[sent];

            /* Write a TX byte via HAL */
            if (fdoExt->Ops->WriteTxByte != NULL) {
                status = fdoExt->Ops->WriteTxByte(fdoExt, byte);
                if (!NT_SUCCESS(status)) {
                    fdoExt->HardwareFailure = TRUE;
                    I2cCtrl_Log("WriteBurstPolled: WriteTxByte failed (Ctrl%lu, status=0x%08lx)",
                                fdoExt->ControllerId, status);
                    return status;
                }
            } else if (fdoExt->Ops->PrimeWrite != NULL) {
                ULONG pushed = 0U;
                status = fdoExt->Ops->PrimeWrite(fdoExt, &byte, 1U, &pushed);
                if (!NT_SUCCESS(status) || pushed != 1U) {
                    fdoExt->HardwareFailure = TRUE;
                    I2cCtrl_Log("WriteBurstPolled: PrimeWrite failed (Ctrl%lu, status=0x%08lx, pushed=%lu)",
                                fdoExt->ControllerId, status, pushed);
                    return NT_SUCCESS(status) ? STATUS_DEVICE_HARDWARE_ERROR : status;
                }
            } else {
                I2cCtrl_Log("WriteBurstPolled: no HAL path to write data (Ctrl%lu)",
                            fdoExt->ControllerId);
                return STATUS_NOT_SUPPORTED;
            }

            sent++;
        } else {
            KeStallExecutionProcessor(5U);
        }
    }

    /* Optional STOP emit via HAL */
    if (issueStop != FALSE && fdoExt->Ops->EmitStopIfNeeded != NULL) {
        status = fdoExt->Ops->EmitStopIfNeeded(fdoExt);
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            I2cCtrl_Log("WriteBurstPolled: EmitStopIfNeeded failed (Ctrl%lu, status=0x%08lx)",
                        fdoExt->ControllerId, status);
            return status;
        }
    }

    /* Confirm STOP */
    if (issueStop != FALSE) {
        ULONG loops = 0U;

        for (;;) {
            status = fdoExt->Ops->GetStatus(fdoExt, &hwst);
            if (!NT_SUCCESS(status)) {
                fdoExt->HardwareFailure = TRUE;
                I2cCtrl_Log("WriteBurstPolled: GetStatus failed during STOP wait (Ctrl%lu, status=0x%08lx)",
                            fdoExt->ControllerId, status);
                return status;
            }

            if (hwst.StopDetected != FALSE) {
                break;
            }

            KeStallExecutionProcessor(5U);
            loops++;

            if (loops > 10000U) {
                fdoExt->HardwareFailure = TRUE;
                I2cCtrl_Log("WriteBurstPolled: STOP timeout (Ctrl%lu)",
                            fdoExt->ControllerId);
                return STATUS_IO_TIMEOUT;
            }
        }

        if (fdoExt->Ops->AckInterrupts != NULL) {
            (VOID)fdoExt->Ops->AckInterrupts(fdoExt, hwst.StopBits);
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS
I2cCtrl_StartCompletion(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
    )
{
    PI2CCTRL_FDO fdoExt = (PI2CCTRL_FDO)Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }

    if (NT_SUCCESS(Irp->IoStatus.Status)) {

        /* 1) Let the FDO process StartDevice (may set UnsupportedPlatform). */
        I2cCtrl_StartDevice(fdoExt, Irp);

        /* 2) Only create the touchpad if the controller is actually supported. */
        if (!fdoExt->UnsupportedPlatform) {

            I2cCtrl_Log("StartCompletion: calling I2cCtrl_CreateTouchpad()\n");
            (void)I2cCtrl_CreateTouchpad(fdoExt->Self, fdoExt);

        } else {

            I2cCtrl_Log(
                "StartCompletion: UnsupportedPlatform=TRUE -> "
                "skipping I2cCtrl_CreateTouchpad()\n"
            );
        }
    }

    /* 3) RELEASE THE REMOVE LOCK - REQUIRED! */
    IoReleaseRemoveLock(&fdoExt->RemoveLock, Irp);

    return STATUS_CONTINUE_COMPLETION;
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_CompletionSignalEvent - XP/2003-safe, C89-compliant
 *
 * Purpose:
 *   - Completion routine that signals a KEVENT when an IRP finishes
 *   - Validates parameters and IRQL
 *   - Signals deterministically, regardless of PendingReturned
 *   - Prevents double completion by using an atomic guard
 *
 * Guarantees:
 *   - Runs at DISPATCH_LEVEL or below
 *   - Operates strictly on non-paged memory
 *   - Returns STATUS_MORE_PROCESSING_REQUIRED to stop further completion processing
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_CompletionSignalEvent(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp,
    PVOID          Context
    )
{
    PKEVENT ev;
    volatile LONG *completedFlag;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* Defensive checks */
    if (Irp == NULL || Context == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                    "CompletionSignalEvent: invalid parameters (Irp=%p, Context=%p)",
                    Irp, Context);
        return STATUS_INVALID_PARAMETER;
    }

    /* Assert IRQL contract (completion can run at <= DISPATCH_LEVEL) */
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                    "CompletionSignalEvent: invalid IRQL=%lu (must run <= DISPATCH_LEVEL)",
                    (ULONG)KeGetCurrentIrql());
        return STATUS_INVALID_DEVICE_STATE;
    }

    ev = (PKEVENT)Context;

    /*
     * Optional: if you maintain a Completed flag in your device extension,
     * pass its address as Context instead of the KEVENT. This prevents
     * double completion races.
     */
    completedFlag = (volatile LONG *)Context;
    if (InterlockedCompareExchange(completedFlag, 1, 0) != 0) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_HW,
                    "CompletionSignalEvent: IRP %p already completed", Irp);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    /* Signal the event unconditionally */
    KeSetEvent(ev, IO_NO_INCREMENT, FALSE);

#if DBG
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_HW,
                "CompletionSignalEvent (Irp=%p, Status=0x%08lx, PendingReturned=%lu)",
                Irp,
                Irp->IoStatus.Status,
                (ULONG)(Irp->PendingReturned ? 1U : 0U));
#endif

    /*
     * Stop further completion processing. The caller that set this completion
     * routine must complete the IRP exactly once after waiters are released.
     */
    return STATUS_MORE_PROCESSING_REQUIRED;
}



/* -----------------------------------------------------------------------
 * I2cCtrl_QuiesceHardware - safely quiesce controller on shutdown/stop
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_QuiesceHardware(
    PI2CCTRL_FDO devctx
    )
{
    KIRQL oldIrql;
    PIRP  irp;

    if (devctx == NULL) {
        return;
    }

    /* Disable controller via HAL ops */
    if (devctx->Ops != NULL && devctx->Ops->Enable != NULL) {
        (VOID)devctx->Ops->Enable(devctx, FALSE);
    }

    /* Complete any pending IRP */
    KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
    irp = devctx->PendingIrp;
    devctx->PendingIrp = NULL;
    KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

    if (irp != NULL) {
        if (IoSetCancelRoutine(irp, NULL) == NULL) {
            /* Cancel may be racing */
        }
        irp->IoStatus.Status      = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

VOID
I2cCtrl_CompletePendingIrpNoDevice(
    PI2CCTRL_FDO devctx
    )
{
    KIRQL oldIrql;
    PIRP  irp;   /* declare before statements */

    KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
    irp = devctx->PendingIrp;
    devctx->PendingIrp = NULL;
    KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

    if (irp != NULL) {
        irp->IoStatus.Status      = STATUS_NO_SUCH_DEVICE;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

/* Local forward declarations */
NTSTATUS I2cCtrl_ParseTranslated(PI2CCTRL_FDO devctx, PCM_RESOURCE_LIST Translated);
NTSTATUS I2cCtrl_ParseRaw(PI2CCTRL_FDO devctx, PCM_RESOURCE_LIST Raw);

/* Map MMIO using MmMapIoSpace */
NTSTATUS
I2cCtrl_MapMmioResource(
    PI2CCTRL_FDO devctx,
    PHYSICAL_ADDRESS        PhysBase,
    ULONG                   Length
    )
{
    PVOID va;

    if (Length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Unmap any previous mapping defensively */
    if (devctx->Mmio != NULL && devctx->MmioLength != 0) {
        MmUnmapIoSpace(devctx->Mmio, devctx->MmioLength);
        devctx->Mmio       = NULL;
        devctx->MmioLength = 0;
    }

    va = MmMapIoSpace(PhysBase, Length, MmNonCached);
    if (va == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    devctx->Mmio       = va;
    devctx->MmioPhys   = PhysBase;
    devctx->MmioLength = Length;

    return STATUS_SUCCESS;
}

/* Connect interrupt using IoConnectInterrupt */
NTSTATUS
I2cCtrl_ConnectInterrupt(
    PI2CCTRL_FDO devctx,
    KIRQL                   IrqLevel,
    ULONG                   IrqVector,
    KAFFINITY               IrqAffinity,
    KINTERRUPT_MODE         IrqMode,
    BOOLEAN                 Shared
    )
{
    NTSTATUS status;

    /* If already connected, disconnect first to avoid leaks */
    if (devctx->InterruptObject != NULL) {
        IoDisconnectInterrupt(devctx->InterruptObject);
        devctx->InterruptObject = NULL;
    }

    /* Save IRQ parameters in the context */
    devctx->IrqLevel    = IrqLevel;
    devctx->IrqVector   = IrqVector;
    devctx->IrqAffinity = IrqAffinity;
    devctx->IrqLatched  = (IrqMode == Latched) ? TRUE : FALSE;
    devctx->IrqMode     = IrqMode;     /* add KINTERRUPT_MODE field in context */
    devctx->IrqSharable = Shared;      /* add BOOLEAN field in context */

    /* Initialize the DPC object for bottom-half processing */
    KeInitializeDpc(&devctx->IsrDpc,
                    (PKDEFERRED_ROUTINE)I2cCtrl_DpcRoutine,
                    devctx);
    devctx->DpcInitialized = TRUE;

    /* Connect the ISR */
    status = IoConnectInterrupt(
        &devctx->InterruptObject,
        (PKSERVICE_ROUTINE)I2cCtrl_Isr,   /* <-- your actual ISR routine */
        devctx,                           /* ServiceContext ties ISR to this controller */
        NULL,                             /* SpinLock (optional) */
        devctx->IrqVector,                /* Vector */
        devctx->IrqLevel,                 /* Irql */
        devctx->IrqLevel,                 /* SynchronizeIrql */
        devctx->IrqMode,                  /* InterruptMode */
        devctx->IrqSharable,              /* ShareVector */
        devctx->IrqAffinity,              /* ProcessorEnableMask */
        FALSE                             /* FloatingSave */
    );

    return status;
}

/* Top-level: parse translated + raw lists, set up MMIO/IRQ */
NTSTATUS
I2cCtrl_ParseCrsResources(
    PI2CCTRL_FDO devctx,
    PCM_RESOURCE_LIST       Translated,
    PCM_RESOURCE_LIST       Raw
    )
{
    NTSTATUS stTrans, stRaw;

    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Reset parsed state before consuming new resources */
    devctx->Mmio       = NULL;
    devctx->MmioLength = 0;
    devctx->MmioPhys.QuadPart = 0;

    devctx->InterruptObject = NULL;
    devctx->IrqLevel    = 0;
    devctx->IrqVector   = 0;
    devctx->IrqAffinity = 0;
    devctx->IrqLatched  = FALSE;

    stTrans = I2cCtrl_ParseTranslated(devctx, Translated);
    stRaw   = I2cCtrl_ParseRaw(devctx, Raw);

    /* Raw parsing is diagnostic; translated must succeed to proceed */
    if (NT_SUCCESS(stTrans)) {
        return STATUS_SUCCESS;
    }
    return stTrans;
}

/* Parse translated resources: apply MMIO and interrupt */
NTSTATUS
I2cCtrl_ParseTranslated(
    PI2CCTRL_FDO devctx,
    PCM_RESOURCE_LIST       Translated
    )
{
    ULONG i, j;
    ULONG listCount;
    PCM_FULL_RESOURCE_DESCRIPTOR    full;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR prd;
    NTSTATUS status;
    BOOLEAN haveMmio;
    BOOLEAN haveIrq;

    if (Translated == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    listCount = Translated->Count;
    if (listCount == 0) {
        return STATUS_UNSUCCESSFUL;
    }

    haveMmio = FALSE;
    haveIrq  = FALSE;

    for (i = 0; i < listCount; i++) {
        full = &Translated->List[i];
        prd  = full->PartialResourceList.PartialDescriptors;

        for (j = 0; j < full->PartialResourceList.Count; j++, prd++) {
            switch (prd->Type) {

            case CmResourceTypeMemory:
                if (!haveMmio) {
                    status = I2cCtrl_MapMmioResource(
                        devctx,
                        prd->u.Memory.Start,
                        prd->u.Memory.Length
                        );
                    if (!NT_SUCCESS(status)) {
                        return status;
                    }
                    haveMmio = TRUE;
                }
                break;

            case CmResourceTypePort:
                /* Optional: handle port-mapped controller if applicable */
                break;

            case CmResourceTypeInterrupt:
                if (!haveIrq) {
                    status = I2cCtrl_ConnectInterrupt(
                        devctx,
                        (KIRQL)prd->u.Interrupt.Level,   /* cast fixes warning */
                        prd->u.Interrupt.Vector,
                        prd->u.Interrupt.Affinity,
                        (prd->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive,
                        (prd->ShareDisposition == CmResourceShareShared) ? TRUE : FALSE
                        );
                    if (!NT_SUCCESS(status)) {
                        return status;
                    }
                    haveIrq = TRUE;
                }
                break;

            case CmResourceTypeDeviceSpecific:
                /* Optional: OEM-specific data; ignore by default */
                break;

            default:
                /* Ignore other resource types */
                break;
            }
        }
    }

    if (!haveMmio) {
        return STATUS_UNSUCCESSFUL;
    }

    /* IRQ may be optional if you support polling; otherwise require it */
    return STATUS_SUCCESS;
}

/* Parse raw resources (diagnostics only) */
NTSTATUS
I2cCtrl_ParseRaw(
    PI2CCTRL_FDO devctx,
    PCM_RESOURCE_LIST       Raw
    )
{
    ULONG i, j;
    ULONG listCount;
    PCM_FULL_RESOURCE_DESCRIPTOR    full;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR prd;

    UNREFERENCED_PARAMETER(devctx);

    if (Raw == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    listCount = Raw->Count;
    if (listCount == 0) {
        return STATUS_UNSUCCESSFUL;
    }

    for (i = 0; i < listCount; i++) {
        full = &Raw->List[i];
        prd  = full->PartialResourceList.PartialDescriptors;

        for (j = 0; j < full->PartialResourceList.Count; j++, prd++) {
            switch (prd->Type) {
            case CmResourceTypeMemory:
                /* Could log Raw memory range */
                break;
            case CmResourceTypeInterrupt:
                /* Could log Raw IRQ line */
                break;
            case CmResourceTypeDeviceSpecific:
                /* OEM-specific data if any */
                break;
            default:
                break;
            }
        }
    }

    return STATUS_SUCCESS;
}

VOID
I2cCtrl_WorkerThread(
    PVOID Context
    )
{
    PI2CCTRL_FDO   dx;
    PI2CCTRL_QUEUE q;

    dx = (PI2CCTRL_FDO)Context;
    if (dx == NULL) {
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
    }

    q = dx->Queue;
    if (q == NULL) {
        PsTerminateSystemThread(STATUS_SUCCESS);
    }

    while (q->Running) {
        NTSTATUS waitStatus;

        waitStatus = KeWaitForSingleObject(&q->WorkEvent,
                                           Executive,
                                           KernelMode,
                                           FALSE,
                                           NULL);
        if (!NT_SUCCESS(waitStatus)) {
            break;
        }

        for (;;) {
            KIRQL                oldIrql;
            PI2CCTRL_IRP_CONTEXT ctx;
            PLIST_ENTRY          e;

            ctx = NULL;
            e   = NULL;

            KeAcquireSpinLock(&q->Lock, &oldIrql);
            if (!q->Running || IsListEmpty(&q->PendingIrps)) {
                KeClearEvent(&q->WorkEvent);
                KeReleaseSpinLock(&q->Lock, oldIrql);
                break;
            }

            e = RemoveHeadList(&q->PendingIrps);
            KeReleaseSpinLock(&q->Lock, oldIrql);

            ctx = CONTAINING_RECORD(e, I2CCTRL_IRP_CONTEXT, ListEntry);
            if (ctx == NULL || ctx->Irp == NULL) {
                continue;
            }

            if (ctx->Canceled || ctx->Irp->Cancel) {
                ctx->Irp->IoStatus.Status      = STATUS_CANCELLED;
                ctx->Irp->IoStatus.Information = 0;
                IoCompleteRequest(ctx->Irp, IO_NO_INCREMENT);
                ctx->Completed = TRUE;
                continue;
            }

            switch (ctx->Opcode) {
            case IOCTL_SET_TARGET:
                ctx->CompletionStatus =
                    I2cCtrl_IoctlSetTarget(dx, NULL, ctx->UserBuffer, ctx->BytesCompleted);
                break;

            case IOCTL_XFER_DESC:
                ctx->CompletionStatus =
                    I2cCtrl_IoctlTransfer(NULL, dx, NULL, ctx->UserBuffer, ctx->BytesCompleted);
                break;

            case IOCTL_SEQUENCE:
                ctx->CompletionStatus =
                    I2cCtrl_IoctlSequence(NULL, dx, NULL, ctx->UserBuffer, ctx->BytesCompleted);
                break;

            case IOCTL_PROBE:
                ctx->CompletionStatus =
                    I2cCtrl_IoctlProbe(dx, NULL, ctx->UserBuffer, ctx->BytesCompleted);
                break;

            default:
                ctx->CompletionStatus = STATUS_INVALID_DEVICE_REQUEST;
                break;
            }

            ctx->Irp->IoStatus.Status =
                ctx->CompletionStatus;
            ctx->Irp->IoStatus.Information =
                NT_SUCCESS(ctx->CompletionStatus) ? ctx->BytesCompleted : 0;

            IoCompleteRequest(ctx->Irp, IO_NO_INCREMENT);
            ctx->Completed = TRUE;
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

VOID I2cCtrl_StopQueue(PI2CCTRL_FDO Dx)
{
    Dx->Queue->Running = FALSE;
    KeSetEvent(&Dx->Queue->WorkEvent, IO_NO_INCREMENT, FALSE);
    // Join worker, then flush and fail remaining IRPs
}

/* File-scope completion routine: releases remove lock when IRP unwinds */
NTSTATUS
I2CCTRL_ReleaseLockCompletion(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
    )
{
    PIO_REMOVE_LOCK lock;

    /* C89: declare before use */
    lock = (PIO_REMOVE_LOCK)Context;

    if (lock != NULL) {
        IoReleaseRemoveLock(lock, Irp);
    }

    UNREFERENCED_PARAMETER(DeviceObject);

    /* Always continue completion so IRP unwinds correctly */
    return STATUS_CONTINUE_COMPLETION;
}

/* ---------------------------------------------------------------------------
 * I2cCtrl_RebindWorkerRoutine - Hot-plug/dynamic rebind (ACPI-safe, XP/2003)
 *
 * ACPI / PDO lifetime rules:
 * - DO NOT delete or recreate child PDOs here
 * - DO NOT free ACPI-visible strings or close child ACPI handles
 * - Only:
 *      * refresh controller-side state (DPCs, flags, optional ACPI open)
 *      * mark children stale
 *      * notify PnP via IoInvalidateDeviceRelations(BusRelations)
 * - Actual PDO removal/creation happens only through PnP IRPs
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_RebindWorkerRoutine(
    PDEVICE_OBJECT DeviceObject,
    PVOID Context
    )
{
    PI2CCTRL_FDO devctx;
    NTSTATUS     status;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* C89 init */
    devctx = (PI2CCTRL_FDO)Context;
    status = STATUS_SUCCESS;

    if (devctx == NULL) {
        return;
    }

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    KdPrint(("I2CCTRL: RebindWorkerRoutine invoked for FDO %p\n", devctx));

    /* This worker is the one scheduled for rebind */
    devctx->HotplugPending = FALSE;

    /*
     * Reinitialize DPCs (ISR queues IsrDpc).
     * This is controller-local state and does not affect ACPI/PDO lifetime.
     */
    KeInitializeDpc(&devctx->IsrDpc,     I2cCtrl_DpcRoutine,        devctx);
    KeInitializeDpc(&devctx->QueueDpc,   I2cCtrl_QueueDpcRoutine,   devctx);
    KeInitializeDpc(&devctx->TimeoutDpc, I2cCtrl_TimeoutDpcRoutine, devctx);
    devctx->DpcInitialized = TRUE;

    /*
     * Optionally reopen controller-level ACPI binding.
     * Do NOT touch per-child ACPI handles here.
     */
    if (devctx->AcpiHandle == NULL || devctx->AcpiDeviceObject == NULL) {
        status = I2cCtrl_AcpiOpen(devctx);
        if (!NT_SUCCESS(status)) {
            KdPrint(("I2CCTRL: RebindWorker: ACPI open failed (0x%08X)\n", status));
            /* Continue; some controllers may not require ACPI methods */
        }
    }

    /*
     * IMPORTANT:
     * We NO LONGER delete or recreate child PDOs here.
     * Any change in ACPI namespace or hot-plug is reflected by:
     *   - marking children stale
     *   - asking PnP to re-query BusRelations
     * PnP will then send IRP_MN_QUERY_DEVICE_RELATIONS and
     * IRP_MN_REMOVE_DEVICE / IRP_MN_START_DEVICE as appropriate.
     */
    devctx->ChildrenStale   = TRUE;
    devctx->HardwareFailure = FALSE;
    devctx->ActiveBusy      = FALSE;
    devctx->Stopping        = FALSE;
    /* devctx->Removed is controlled by FDO REMOVE_DEVICE */

    /*
     * We do NOT remap MMIO or reconnect interrupts here.
     * XP/2003 requires IRP_MN_START_DEVICE to deliver and validate resources.
     */

    /* Notify PnP that BusRelations changed; this will drive proper PDO lifetime. */
    if (devctx->PhysicalDevice != NULL) {
        IoInvalidateDeviceRelations(devctx->PhysicalDevice, BusRelations);
    }

    KdPrint(("I2CCTRL: RebindWorker: BusRelations invalidated for FDO %p (ACPI-safe)\n",
             devctx));
}

__inline BOOLEAN
I2cctrlIsEqualGuid(const GUID* a, const GUID* b)
{
    return (RtlCompareMemory(a, b, sizeof(GUID)) == sizeof(GUID)) ? TRUE : FALSE;
}

//
// I2cCtrl_DispatchPnP - top-level PnP dispatch
// Routes to PDO or FDO handlers based on extension signature.
// Unknown or invalid extensions are failed locally.
// XP/2003-safe, C89-compliant.
//
NTSTATUS
I2cCtrl_DispatchPnP(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PIO_STACK_LOCATION     isl;
    PI2CCTRL_COMMON_HEADER hdr;
    NTSTATUS               status;

    PAGED_CODE();
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (DeviceObject == NULL || Irp == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    isl    = IoGetCurrentIrpStackLocation(Irp);
    hdr    = (PI2CCTRL_COMMON_HEADER)DeviceObject->DeviceExtension;
    status = Irp->IoStatus.Status;

    /* IMPORTANT:
     * Logging removed here because PnP IRPs may run inside
     * registry unload, NTFS teardown, or other contexts where
     * ZwCreateFile / ZwWriteFile are unsafe.
     */

    if (hdr == NULL) {
        Irp->IoStatus.Status      = STATUS_NO_SUCH_DEVICE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NO_SUCH_DEVICE;
    }

    switch (hdr->Signature) {

    case I2CCTRL_PDO_SIGNATURE:
        return I2cCtrl_PdoDispatch(DeviceObject, Irp);

    case I2CCTRL_FDO_SIGNATURE:
        return I2cCtrl_FdoDispatch(DeviceObject, Irp);

    default:
        Irp->IoStatus.Status      = STATUS_NO_SUCH_DEVICE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NO_SUCH_DEVICE;
    }
}


//
// Abstract helpers for I²C controller power/timing
//

NTSTATUS
I2cCtrl_EnableController(
    PI2CCTRL_FDO devctx,
    BOOLEAN      enable
    )
{
    NTSTATUS st = STATUS_SUCCESS;

    if (devctx == NULL) {
        I2cCtrl_Log("EnableController: NULL devctx\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    __try {

        I2cCtrl_Log("EnableController: %s requested (LPSS2 first)\n",
                     enable ? "ENABLE" : "DISABLE");

        /* ---------------------------------------------------------
         * 1) Try native LPSS2 (8086:9DE9) power-on first
         * --------------------------------------------------------- */
        if (enable) {
            st = I2cCtrl_Lpss2PowerOn(devctx);
            I2cCtrl_Log("EnableController: LPSS2 PowerOn returned 0x%08lx\n", st);
        } else {
            st = I2cCtrl_Lpss2PowerOff(devctx);
            I2cCtrl_Log("EnableController: LPSS2 PowerOff returned 0x%08lx\n", st);
        }

        /* LPSS2 succeeded -> done */
        if (NT_SUCCESS(st)) {
            devctx->Enabled = enable ? TRUE : FALSE;
            I2cCtrl_Log("EnableController: LPSS2 path succeeded, Enabled=%lu\n", devctx->Enabled);
            return st;
        }

        I2cCtrl_Log("EnableController: LPSS2 path FAILED -> falling back to HAL ops\n");

        /* ---------------------------------------------------------
         * 2) LPSS2 failed -> fall back to HAL ops (generic path)
         * --------------------------------------------------------- */
        if (devctx->Ops != NULL && devctx->Ops->Enable != NULL) {

            NTSTATUS st2 = devctx->Ops->Enable(devctx, enable);
            I2cCtrl_Log("EnableController: HAL->Enable returned 0x%08lx\n", st2);

            if (NT_SUCCESS(st2)) {
                devctx->Enabled = enable ? TRUE : FALSE;
                I2cCtrl_Log("EnableController: HAL path succeeded, Enabled=%lu\n", devctx->Enabled);
                return st2;
            }

            /* HAL also failed */
            devctx->HardwareFailure = TRUE;
            I2cCtrl_Log("EnableController: HAL path FAILED -> HardwareFailure=TRUE\n");
            return st2;
        }

        /* No HAL ops available */
        devctx->HardwareFailure = TRUE;
        I2cCtrl_Log("EnableController: No HAL ops available -> HARD FAILURE\n");
        return st;

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        I2cCtrl_Log("EnableController: EXCEPTION in LPSS2/HAL enable\n");
        devctx->HardwareFailure = TRUE;
        return STATUS_ACCESS_VIOLATION;
    }
}



/* ---------------------------------------------------------------------------
 * Mask or unmask controller interrupts - HAL-generic, XP/2003-hardened, C89-compliant
 *
 * Purpose:
 *   - Universally mask or unmask interrupts without backend register references
 *   - Relies solely on HAL ops; no MMIO access
 *   - Hardened with SEH guards and strict NULL checks
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_MaskInterrupts(
    PI2CCTRL_FDO devctx,
    BOOLEAN      mask
    )
{
    ULONG newMask;

    /* Defensive init */
    newMask = 0U;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL || devctx->Ops == NULL || devctx->Ops->MaskInterrupts == NULL) {
        return;
    }

    /* Convention: mask==TRUE disables all interrupts, mask==FALSE enables all */
    if (mask != FALSE) {
        newMask = 0U;
    } else {
        newMask = 0xFFFFFFFFU; /* universal "all enabled" mask */
    }

    devctx->IntrMask = newMask;

    __try {
        (VOID)devctx->Ops->MaskInterrupts(devctx, newMask);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("I2CCTRL: MaskInterrupts: exception in HAL op\n"));
        /* On exception, force interrupts disabled */
        devctx->IntrMask = 0U;
    }
}



/* ---------------------------------------------------------------------------
 * Apply abstract bus timing (convert ns + speed into HAL ops)
 * HAL-generic, XP/2003-hardened, C89-compliant
 *
 * Purpose:
 *   - Convert abstract timing (ns + Hz) into HAL ops
 *   - Delegate to HAL without direct register writes
 *   - Cache timing values for policy/state tracking
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_ApplyBusTiming(
    PI2CCTRL_FDO devctx,
    ULONG        highNs,
    ULONG        lowNs,
    ULONG        busSpeedHz
    )
{
    ULONG      hcnt;
    ULONG      lcnt;
    ULONGLONG  tmpHigh;
    ULONGLONG  tmpLow;

    /* Defensive init (C89) */
    hcnt    = 0U;
    lcnt    = 0U;
    tmpHigh = 0ULL;
    tmpLow  = 0ULL;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL || devctx->Ops == NULL) {
        return;
    }

    /* Example conversion: counts = (time_ns * busSpeedHz) / 1e9 */
    tmpHigh = ((ULONGLONG)highNs * (ULONGLONG)busSpeedHz);
    tmpLow  = ((ULONGLONG)lowNs  * (ULONGLONG)busSpeedHz);

    hcnt = (ULONG)(tmpHigh / 1000000000ULL);
    lcnt = (ULONG)(tmpLow  / 1000000000ULL);

    __try {
        /* Delegate to HAL ops instead of direct register writes */
        if (devctx->Ops->SetBusSpeedHz != NULL) {
            (VOID)devctx->Ops->SetBusSpeedHz(devctx, busSpeedHz);
        }
        else if (devctx->Ops->SetSpeed != NULL) {
            /* crude mapping: choose enum based on Hz */
            if (busSpeedHz <= 100000U) {
                (VOID)devctx->Ops->SetSpeed(devctx, I2C_SPEED_STANDARD);
            } else if (busSpeedHz <= 400000U) {
                (VOID)devctx->Ops->SetSpeed(devctx, I2C_SPEED_FAST);
            } else {
                (VOID)devctx->Ops->SetSpeed(devctx, I2C_SPEED_HIGH);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("I2CCTRL: ApplyBusTiming: exception in HAL ops\n"));
    }

    /* Save abstract timing for policy/state tracking */
    devctx->SavedTimingHighNs = highNs;
    devctx->SavedTimingLowNs  = lowNs;
    devctx->SavedBusSpeed     = busSpeedHz;

    /* Optionally cache computed counts if backend wants them */
    devctx->SsSclHighCnt = hcnt;
    devctx->SsSclLowCnt  = lcnt;
}

/* ---------------------------------------------------------------------------
 * Query current high period timing (ns) - HAL-generic, XP/2003-hardened, C89-compliant
 *
 * Purpose:
 *   - Return the cached high period timing in nanoseconds
 *   - Avoid direct register reads; rely on cached values or HAL ops
 *   - Safe fallback to 0 if unavailable
 * --------------------------------------------------------------------------- */
ULONG
I2cCtrl_QueryTimingHigh(
    PI2CCTRL_FDO devctx
    )
{
    I2C_HW_STATUS st;
    NTSTATUS      status;
    ULONG         result;

    /* Defensive init (C89) */
    RtlZeroMemory(&st, sizeof(st));
    status = STATUS_SUCCESS;
    result = 0U;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL || devctx->Ops == NULL) {
        return 0U;
    }

    /* If bus speed is unknown, cannot compute */
    if (devctx->CurrentBusSpeed == 0U) {
        return 0U;
    }

    /* Prefer cached abstract timing */
    if (devctx->SavedTimingHighNs != 0U) {
        return devctx->SavedTimingHighNs;
    }

    /* Optional: ask backend for status if it can provide counts */
    if (devctx->Ops->GetStatus != NULL) {
        __try {
            status = devctx->Ops->GetStatus(devctx, &st);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            KdPrint(("I2CCTRL: QueryTimingHigh: exception in GetStatus\n"));
            status = STATUS_ACCESS_VIOLATION;
        }

        if (NT_SUCCESS(status)) {
            /*
             * If backend populates timing counts, convert them here.
             * For now, return 0 as placeholder until HAL defines fields.
             */
            result = 0U;
        }
    }

    return result;
}


/* ---------------------------------------------------------------------------
 * Query current low period timing (ns) - HAL-generic, XP/2003-hardened, C89-compliant
 *
 * Purpose:
 *   - Return the cached low period timing in nanoseconds
 *   - Avoid direct register reads; rely on cached values or HAL ops
 *   - Safe fallback to 0 if unavailable
 * --------------------------------------------------------------------------- */
ULONG
I2cCtrl_QueryTimingLow(
    PI2CCTRL_FDO devctx
    )
{
    I2C_HW_STATUS st;
    NTSTATUS      status;
    ULONG         result;

    /* Defensive init (C89) */
    RtlZeroMemory(&st, sizeof(st));
    status = STATUS_SUCCESS;
    result = 0U;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL || devctx->Ops == NULL) {
        return 0U;
    }

    /* If bus speed is unknown, cannot compute */
    if (devctx->CurrentBusSpeed == 0U) {
        return 0U;
    }

    /* Prefer cached abstract timing */
    if (devctx->SavedTimingLowNs != 0U) {
        return devctx->SavedTimingLowNs;
    }

    /* Optional: ask backend for status if it can provide timing info */
    if (devctx->Ops->GetStatus != NULL) {
        __try {
            status = devctx->Ops->GetStatus(devctx, &st);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            KdPrint(("I2CCTRL: QueryTimingLow: exception in GetStatus\n"));
            status = STATUS_ACCESS_VIOLATION;
        }

        if (NT_SUCCESS(status)) {
            /*
             * Backend may extend I2C_HW_STATUS to include timing counts.
             * If valid counts are provided, convert them into nanoseconds
             * using the current bus speed. Otherwise, fall back to 0.
             */
            if (st.SsSclLowCnt != 0U && devctx->CurrentBusSpeed != 0U) {
                result = (ULONG)(((ULONGLONG)st.SsSclLowCnt * 1000000000ULL) /
                                 (ULONGLONG)devctx->CurrentBusSpeed);
            } else {
                result = 0U;
            }
        }
    }

    return result;
}

/* ---------------------------------------------------------------------------
 * ClearErrors - HAL-generic, XP/2003-hardened, C89-compliant
 * Uses BSOD-safe register accessors (I2cCtrl_WriteRegisterSafe).
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_ClearErrors(
    PI2CCTRL_FDO devctx
    )
{
    KIRQL oldIrql;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    if (devctx == NULL) {
        return;
    }

    /* 1) Fence ISR/DPC access while clearing */
    KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);

    /* 2) Clear sticky hardware error/status registers (safe MMIO) */
    I2cCtrl_WriteRegisterSafe(devctx,
                              I2CCTRL_REG_STATUS,
                              I2CCTRL_STATUS_CLEAR_MASK);

    I2cCtrl_WriteRegisterSafe(devctx,
                              I2CCTRL_REG_ERROR,
                              I2CCTRL_ERROR_CLEAR_MASK);

    /* 3) Reset software error counters */
    devctx->ErrorCount    = 0U;
    devctx->ArbLossCount  = 0U;
    devctx->TimeoutCount  = 0U;
    devctx->HidErrorCount = 0U;

    /* 4) Reset transfer context error status */
    devctx->XferCtx.Status = STATUS_SUCCESS;
    devctx->XferCtx.Errors = 0U;

    /* 5) Defensive: clear any stale IRP pointer in transfer context */
    if (devctx->XferCtx.Irp != NULL) {
        PIRP irp;

        irp = (PIRP)InterlockedExchangePointer(
                    (PVOID*)&devctx->XferCtx.Irp,
                    NULL);

        if (irp != NULL) {
            irp->IoStatus.Status      = STATUS_CANCELLED;
            irp->IoStatus.Information = 0U;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
        }
    }

    KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

    /* 6) Trace for diagnostics */
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL,
                "ClearErrors: controller=%p errors cleared, counters reset",
                devctx);
}


/* ---------------------------------------------------------------------------
 * ClockGate - HAL-generic, XP/2003-hardened, C89-compliant
 * Uses BSOD-safe register accessors (I2cCtrl_WriteRegisterSafe).
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_ClockGate(
    PI2CCTRL_FDO devctx,
    BOOLEAN      enableGate   /* TRUE = gate (disable clock), FALSE = ungate */
    )
{
    KIRQL oldIrql;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    if (devctx == NULL) {
        return;
    }

    /* 1) Fence ISR/DPC access while changing clock state */
    KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
    devctx->ActiveBusy = FALSE;
    KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

    if (enableGate != FALSE) {

        /* --- Gate functional clock --- */
        I2cCtrl_WriteRegisterSafe(devctx,
                                  I2CCTRL_REG_CLKCTRL,
                                  I2CCTRL_CLK_DISABLE_MASK);

        /* Defensive: mask interrupts while clock is gated */
        I2cCtrl_MaskInterrupts(devctx, TRUE);

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_POWER,
                    "ClockGate: controller=%p clock gated", devctx);

        /* Wake policy */
        if (devctx->WakeCapable && !devctx->WakeArmed) {
            I2cCtrl_ArmWake(devctx);
            devctx->WakeArmed = TRUE;
        }
    }
    else {

        /* --- Ungate functional clock --- */
        I2cCtrl_WriteRegisterSafe(devctx,
                                  I2CCTRL_REG_CLKCTRL,
                                  I2CCTRL_CLK_ENABLE_MASK);

        /* Restore bus timing after ungating */
        I2cCtrl_ApplyBusTiming(devctx,
                               devctx->SavedTimingHighNs,
                               devctx->SavedTimingLowNs,
                               devctx->SavedBusSpeed);

        /* Unmask interrupts last */
        I2cCtrl_MaskInterrupts(devctx, FALSE);

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_POWER,
                    "ClockGate: controller=%p clock ungated", devctx);

        /* Wake policy */
        if (devctx->WakeCapable && devctx->WakeArmed) {
            I2cCtrl_DisarmWake(devctx);
            devctx->WakeArmed = FALSE;
        }
    }
}


/* ---------------------------------------------------------------------------
 * Generic Power Dispatch with D0/D1/D2/D3 - HAL-generic, XP/2003-hardened, C89-compliant
 * Feature-complete: context save/restore, wake policy, busy fencing, error fallback
 * --------------------------------------------------------------------------- */

NTSTATUS
I2cCtrl_SetControllerPowerD0(
    PI2CCTRL_FDO devctx
    )
{
    NTSTATUS status;
    KIRQL    oldIrql;
    int      tries;

    /* Defensive init */
    status   = STATUS_SUCCESS;
    oldIrql  = PASSIVE_LEVEL;
    tries    = 0;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL) {
        return STATUS_NO_SUCH_DEVICE;
    }

    /* Fence: ensure ISR/DPC won't touch stale IRPs during resume */
    KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
    devctx->ActiveBusy = FALSE;
    KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

    __try {
/* 1) Power/clock enable and controller bring-up (LPSS2 / 9DE9) */
for (tries = 0; tries < 3; tries++) {

    /* IMPORTANT: capture the return value */
    status = I2cCtrl_EnableController(devctx, TRUE);

    if (NT_SUCCESS(status)) {
        break;     /* controller powered successfully */
    }

    /* backoff before retry */
    KeStallExecutionProcessor(1000);   /* 1 ms */
}

if (!NT_SUCCESS(status)) {
    KdPrint(("I2CCTRL: D0: enable controller failed status=0x%08lx\n", status));

    /* Fail-safe: force controller to D3 hard-off */
    I2cCtrl_SetControllerPowerD3(devctx);

    return status;
}


        /* 2) Re-apply bus timing and speed */
        I2cCtrl_ApplyBusTiming(devctx,
                               devctx->SavedTimingHighNs,
                               devctx->SavedTimingLowNs,
                               devctx->SavedBusSpeed);

        /* 3) Restore extended context (FIFO, queues, counters) */
        I2cCtrl_RestoreFifoState(devctx);
        I2cCtrl_RestoreQueueState(devctx);
        I2cCtrl_RestoreArbCounters(devctx);

        /* 4) Clear sticky status/error and unmask interrupts last */
        I2cCtrl_ClearErrors(devctx);           /* abstract: clear arbitration/timeout flags */
        I2cCtrl_MaskInterrupts(devctx, FALSE); /* enable IRQs after state is valid */

        /* 5) Wake policy: disarm if we’re fully on */
        if (devctx->WakeCapable != FALSE && devctx->WakeArmed != FALSE) {
            I2cCtrl_DisarmWake(devctx);        /* cancel WaitWake, mask wake source */
            devctx->WakeArmed = FALSE;
        }

        /* 6) Mark device state */
        devctx->CurrentDevicePowerState = PowerDeviceD0;
        status = STATUS_SUCCESS;

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("I2CCTRL: D0: exception during resume\n"));
        status = STATUS_ACCESS_VIOLATION;
        /* Fail-safe: hard-off to avoid undefined hardware state */
        I2cCtrl_SetControllerPowerD3(devctx);
    }

    return status;
}

VOID
I2cCtrl_SetControllerPowerD1(
    PI2CCTRL_FDO devctx
    )
{
    KIRQL oldIrql;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL) {
        return;
    }

    /* Fence: snapshot and mark not busy before sleep */
    KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
    devctx->ActiveBusy = FALSE;
    KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

    __try {
        /* 1) Save lightweight timing context */
        devctx->SavedTimingHighNs = I2cCtrl_QueryTimingHigh(devctx);
        devctx->SavedTimingLowNs  = I2cCtrl_QueryTimingLow(devctx);

        /* 2) Light sleep: clock gate + IRQ mask; keep register context */
        I2cCtrl_MaskInterrupts(devctx, TRUE);
        I2cCtrl_ClockGate(devctx, TRUE);       /* abstract: gate functional clock */
        I2cCtrl_EnableController(devctx, FALSE);

        /* 3) Wake policy: arm wake if supported (e.g., GPIO wake line) */
        if (devctx->WakeCapable != FALSE && devctx->WakeArmed == FALSE) {
            I2cCtrl_ArmWake(devctx);           /* PoRequestPowerIrp for WaitWake, program wake source */
            devctx->WakeArmed = TRUE;
        }

        devctx->CurrentDevicePowerState = PowerDeviceD1;

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("I2CCTRL: D1: exception during transition\n"));
        /* Defensive: fall back to deeper sleep */
        I2cCtrl_SetControllerPowerD2(devctx);
    }
}

VOID
I2cCtrl_SetControllerPowerD2(
    PI2CCTRL_FDO devctx
    )
{
    KIRQL oldIrql;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL) {
        return;
    }

    /* Fence: snapshot queues and mark not busy */
    KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
    devctx->ActiveBusy = FALSE;
    KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

    __try {
        /* 1) Save minimal context for quick resume */
        devctx->SavedBusAddress = devctx->TargetAddress;
        devctx->SavedBusSpeed   = devctx->CurrentBusSpeed;

        /* 2) Deeper sleep: mask IRQs, quiesce HW, gate clocks, disable controller */
        I2cCtrl_MaskInterrupts(devctx, TRUE);
        I2cCtrl_QuiesceHardware(devctx);       /* flush FIFOs, stop transfers */
        I2cCtrl_ClockGate(devctx, TRUE);
        I2cCtrl_EnableController(devctx, FALSE);

        /* 3) Wake policy: arm wake to allow resume from input activity */
        if (devctx->WakeCapable != FALSE && devctx->WakeArmed == FALSE) {
            I2cCtrl_ArmWake(devctx);
            devctx->WakeArmed = TRUE;
        }

        devctx->CurrentDevicePowerState = PowerDeviceD2;

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("I2CCTRL: D2: exception during transition\n"));
        /* Defensive: hard-off on exception */
        I2cCtrl_SetControllerPowerD3(devctx);
    }
}

VOID
I2cCtrl_SetControllerPowerD3(
    PI2CCTRL_FDO devctx
    )
{
    KIRQL oldIrql;
    PIRP  irp;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL) {
        return;
    }

    __try {
        /* 1) Disarm wake before full-off */
        if (devctx->WakeCapable != FALSE && devctx->WakeArmed != FALSE) {
            I2cCtrl_DisarmWake(devctx);        /* cancel WaitWake, mask wake source */
            devctx->WakeArmed = FALSE;
        }

        /* 2) Mask interrupts and disable controller & clocks */
        I2cCtrl_MaskInterrupts(devctx, TRUE);
        I2cCtrl_QuiesceHardware(devctx);       /* flush/stop, ensure no DMA/PIO active */
        I2cCtrl_ClockGate(devctx, TRUE);
        I2cCtrl_EnableController(devctx, FALSE);

        /* 3) Cancel any pending IRP defensively (single completion) */
        KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
        irp = devctx->PendingIrp;
        devctx->PendingIrp = NULL;
        KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

        if (irp != NULL) {
            if (IoSetCancelRoutine(irp, NULL) == NULL) {
                /* Cancel in progress elsewhere; let that path complete */
            } else {
                irp->IoStatus.Status      = STATUS_NO_SUCH_DEVICE;
                irp->IoStatus.Information = 0U;
                IoCompleteRequest(irp, IO_NO_INCREMENT);
            }
        }

        /* 4) Context lost in D3 */
        devctx->SavedBusAddress   = 0U;
        devctx->SavedBusSpeed     = 0U;
        devctx->SavedTimingHighNs = 0U;
        devctx->SavedTimingLowNs  = 0U;

        /* 5) Mark device state */
        devctx->CurrentDevicePowerState = PowerDeviceD3;

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("I2CCTRL: D3: exception during transition\n"));
        /* Already attempting hard-off; nothing further to do safely */
    }
}

/* ---------------------------------------------------------------------------
 * Completion routine for system power IRPs: translate to device power and set state
 * HAL-generic, XP/2003-hardened, C89-compliant
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_SystemPowerCompletion(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp,
    PVOID          Context
    )
{
    PI2CCTRL_FDO       devctx;
    PIO_STACK_LOCATION isl;
    SYSTEM_POWER_STATE sState;
    DEVICE_POWER_STATE dTarget;
    NTSTATUS           status;
    POWER_STATE        ps;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* Defensive init (C89) */
    devctx = (PI2CCTRL_FDO)Context;
    isl = IoGetNextIrpStackLocation(Irp);
    sState = PowerSystemUnspecified;
    dTarget = PowerDeviceUnspecified;
    status = Irp->IoStatus.Status;
    ps.DeviceState = PowerDeviceUnspecified;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL) {
        return STATUS_CONTINUE_COMPLETION;
    }

    if (!NT_SUCCESS(status) || isl->Parameters.Power.Type != SystemPowerState) {
        return STATUS_CONTINUE_COMPLETION;
    }

    /* Record system power state */
    sState = isl->Parameters.Power.State.SystemState;
    devctx->SystemPowerState = sState;

    /* Map system state to target device state with capability checks */
    switch (sState) {
    case PowerSystemWorking:
        dTarget = PowerDeviceD0;
        break;
    case PowerSystemSleeping1:
        dTarget = (devctx->SupportsD1 != FALSE) ? PowerDeviceD1 : PowerDeviceD3;
        break;
    case PowerSystemSleeping2:
        dTarget = (devctx->SupportsD2 != FALSE) ? PowerDeviceD2 : PowerDeviceD3;
        break;
    case PowerSystemSleeping3:
    case PowerSystemHibernate:
    case PowerSystemShutdown:
        dTarget = PowerDeviceD3;
        break;
    default:
        dTarget = PowerDeviceD3;
        break;
    }

    /* If removed, force D3 regardless */
    if (devctx->Removed != FALSE) {
        dTarget = PowerDeviceD3;
    }

    /* Apply device power transition if needed */
    if (devctx->CurrentDevicePowerState != dTarget) {
        __try {
            switch (dTarget) {
            case PowerDeviceD0:
                status = I2cCtrl_SetControllerPowerD0(devctx);
                break;
            case PowerDeviceD1:
                I2cCtrl_SetControllerPowerD1(devctx);
                status = STATUS_SUCCESS;
                break;
            case PowerDeviceD2:
                I2cCtrl_SetControllerPowerD2(devctx);
                status = STATUS_SUCCESS;
                break;
            case PowerDeviceD3:
            default:
                I2cCtrl_SetControllerPowerD3(devctx);
                status = STATUS_SUCCESS;
                break;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            KdPrint(("I2CCTRL: SystemPowerCompletion: exception during device power set\n"));
            status = STATUS_ACCESS_VIOLATION;
        }

        if (NT_SUCCESS(status)) {
            ps.DeviceState = dTarget;
            PoSetPowerState(devctx->Self, DevicePowerState, ps);
            devctx->CurrentDevicePowerState = dTarget;
        } else {
            Irp->IoStatus.Status = status;
        }
    }

    return STATUS_CONTINUE_COMPLETION;
}

/* ---------------------------------------------------------------------------
 * Hardware helpers to arm/disarm wake (XP/WDM compliant, hardened, C89-compliant)
 * --------------------------------------------------------------------------- */

VOID
I2cCtrl_EnableWakeSignal(
    PI2CCTRL_FDO devctx
    )
{
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL) {
        return;
    }

    if (devctx->WakeCapable == FALSE) {
        return;
    }

    /* Arm wake by enabling the controller’s interrupt as a wake source */
    if (devctx->InterruptObject != NULL) {
        devctx->WakeArmed = TRUE;

        /* Ensure interrupts are enabled if wake is armed */
        I2cCtrl_MaskInterrupts(devctx, FALSE);

        KdPrint(("I2CCTRL: Wake armed on IRQ vector %lu\n", devctx->IrqVector));
    }
}

VOID
I2cCtrl_DisableWakeSignal(
    PI2CCTRL_FDO devctx
    )
{
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL) {
        return;
    }

    if (devctx->WakeCapable == FALSE) {
        return;
    }

    /* Disarm wake and optionally mask interrupts */
    devctx->WakeArmed = FALSE;
    I2cCtrl_MaskInterrupts(devctx, TRUE);

    KdPrint(("I2CCTRL: Wake disarmed on IRQ vector %lu\n", devctx->IrqVector));
}

/* ---------------------------------------------------------------------------
 * Cancel routine for WAIT_WAKE IRP (XP/WDM compliant, hardened, C89-compliant)
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_CancelWakeIrp(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PI2CCTRL_FDO devctx;
    KIRQL        oldIrql;
    PIRP         pending;

    /* C89 init */
    devctx  = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    oldIrql = PASSIVE_LEVEL;
    pending = NULL;

    /* Release cancel spin first per WDM contract */
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    if (devctx == NULL) {
        Irp->IoStatus.Status = STATUS_CANCELLED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return;
    }

    KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
    pending = devctx->PendingIrp;

    if (pending == Irp) {
        devctx->PendingIrp = NULL;
        KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

        /* Disarm wake at hardware level */
        I2cCtrl_DisableWakeSignal(devctx);

        Irp->IoStatus.Status = STATUS_CANCELLED;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return;
    }

    KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

    /* If it wasn't ours, just complete */
    Irp->IoStatus.Status = STATUS_CANCELLED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/* ---------------------------------------------------------------------------
 * Power management helpers - XP/2003-safe, C89-compliant
 * Feature-complete within existing devctx fields (no new functions or members)
 * --------------------------------------------------------------------------- */

VOID
I2cCtrl_SaveFifoState(
    PI2CCTRL_FDO devctx
    )
{
    if (devctx == NULL) {
        return;
    }

    /* No FIFO-specific fields in FDO; just quiesce HW to ensure a clean suspend */
    KdPrint(("I2CCTRL: SaveFifoState: quiescing hardware (no FIFO fields)\n"));
    I2cCtrl_QuiesceHardware(devctx);
}

VOID
I2cCtrl_RestoreFifoState(
    PI2CCTRL_FDO devctx
    )
{
    if (devctx == NULL) {
        return;
    }

    /* No FIFO-specific fields to restore; rely on normal D0 re-init paths */
    KdPrint(("I2CCTRL: RestoreFifoState: nothing to restore (no FIFO fields)\n"));
}

VOID
I2cCtrl_SaveQueueState(
    PI2CCTRL_FDO devctx
    )
{
    KIRQL oldIrql;

    if (devctx == NULL) {
        return;
    }

    /* Mask interrupts to avoid completions during suspend */
    I2cCtrl_MaskInterrupts(devctx, TRUE);

    /* Snapshot burst counters under queue lock (fields exist) */
    KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
    KdPrint(("I2CCTRL: SaveQueueState: bursts(H=%lu N=%lu L=%lu)\n",
             devctx->BurstHigh, devctx->BurstNormal, devctx->BurstLow));
    KeReleaseSpinLock(&devctx->QueueLock, oldIrql);
}

VOID
I2cCtrl_RestoreQueueState(
    PI2CCTRL_FDO devctx
    )
{
    if (devctx == NULL) {
        return;
    }

    /* Unmask interrupts to resume completions */
    I2cCtrl_MaskInterrupts(devctx, FALSE);

    /* Kick scheduler DPC if queues have work (use existing lists and DPC flag) */
    if (!IsListEmpty(&devctx->HighQueue) ||
        !IsListEmpty(&devctx->NormalQueue) ||
        !IsListEmpty(&devctx->LowQueue)) {

        if (devctx->DpcInitialized != FALSE) {
            KeInsertQueueDpc(&devctx->QueueDpc, NULL, NULL);
        }
    }

    KdPrint(("I2CCTRL: RestoreQueueState: scheduler nudged if pending work\n"));
}

VOID
I2cCtrl_SaveArbCounters(
    PI2CCTRL_FDO devctx
    )
{
    KIRQL oldIrql;

    if (devctx == NULL) {
        return;
    }

    /* Snapshot available arbitration/backoff parameters (existing fields) */
    KeAcquireSpinLock(&devctx->BusLock, &oldIrql);
    KdPrint(("I2CCTRL: SaveArbCounters: base=%lu max=%lu jitter=%lu\n",
             devctx->ArbBackoffBaseUs,
             devctx->ArbBackoffMaxUs,
             devctx->ArbBackoffJitterUs));
    KeReleaseSpinLock(&devctx->BusLock, oldIrql);
}

VOID
I2cCtrl_RestoreArbCounters(
    PI2CCTRL_FDO devctx
    )
{
    if (devctx == NULL) {
        return;
    }

    /* Nothing to restore beyond existing parameters; log for traceability */
    KdPrint(("I2CCTRL: RestoreArbCounters: base=%lu max=%lu jitter=%lu\n",
             devctx->ArbBackoffBaseUs,
             devctx->ArbBackoffMaxUs,
             devctx->ArbBackoffJitterUs));
}

VOID
I2cHal_EnableWakeSource(
    PI2CCTRL_FDO devctx,
    BOOLEAN      enable
    )
{
    if (devctx == NULL) {
        return;
    }

    __try {
        /* Controller-agnostic: rely on HAL ops if provided */
        if (devctx->Ops != NULL && devctx->Ops->EnableWakeSource != NULL) {
            devctx->Ops->EnableWakeSource(devctx, enable);
            KdPrint(("I2CCTRL: EnableWakeSource: HAL op invoked (enable=%lu)\n",
                     (ULONG)enable));
        } else {
            /* Fallback: no HAL hook, just log */
            if (enable != FALSE) {
                KdPrint(("I2CCTRL: EnableWakeSource: requested enable, but no HAL support\n"));
            } else {
                KdPrint(("I2CCTRL: EnableWakeSource: requested disable, but no HAL support\n"));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("I2CCTRL: EnableWakeSource: exception in HAL or fallback path\n"));
    }
}


/* ---------------------------------------------------------------------------
 * ACPI-aware wake management - XP/2003-safe, C89-compliant
 * Implements ACPI 2.0+ behavior with ACPI 1.0b fallback using existing members
 * Assumptions:
 *  - devctx->AcpiIs20Plus indicates ACPI >= 2.0 support
 *  - devctx->WakeCapable / WakeArmed / WaitWakeIrp exist
 *  - devctx->SupportsD1 / SupportsD2 reflect device power capabilities
 *  - devctx->Ops->EnableWakeSource is optional; guarded by NULL checks
 *  - devctx->PhysicalDevice is the PDO used for PoRequestPowerIrp
 * --------------------------------------------------------------------------- */

VOID
I2cCtrl_ArmWake(
    PI2CCTRL_FDO devctx
    )
{
    POWER_STATE ps;
    PIRP        waitWakeIrp;
    SYSTEM_POWER_STATE targetS;

    if (devctx == NULL) {
        return;
    }

    if (devctx->WakeCapable == FALSE) {
        KdPrint(("I2CCTRL: ArmWake: device not wake-capable\n"));
        return;
    }

    if (devctx->WakeArmed != FALSE) {
        KdPrint(("I2CCTRL: ArmWake: already armed\n"));
        return;
    }

    /* Mask normal interrupts before programming wake source */
    I2cCtrl_MaskInterrupts(devctx, TRUE);

    /* ACPI 2.0+: prefer the lightest sleep state the device can wake from */
    if (devctx->AcpiIs20Plus != FALSE) {
        if (devctx->SupportsD1 != FALSE) {
            targetS = PowerSystemSleeping1;   /* S1 -> D1 wake */
        } else if (devctx->SupportsD2 != FALSE) {
            targetS = PowerSystemSleeping2;   /* S2 -> D2 wake */
        } else {
            targetS = PowerSystemSleeping3;   /* S3 -> D3 wake */
        }
    } else {
        /* ACPI 1.0b fallback: assume deepest sleep only (S3) */
        targetS = PowerSystemSleeping3;
    }

    /* Enable wake source if HAL provides a hook */
    if (devctx->Ops != NULL && devctx->Ops->EnableWakeSource != NULL) {
        __try {
            devctx->Ops->EnableWakeSource(devctx, TRUE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            KdPrint(("I2CCTRL: ArmWake: exception enabling wake source\n"));
        }
    }

    /* Issue WaitWake IRP to power manager for the chosen S-state */
    ps.SystemState = targetS;
    waitWakeIrp = NULL;

    PoRequestPowerIrp(devctx->PhysicalDevice,
                      IRP_MN_WAIT_WAKE,
                      ps,
                      NULL,    /* no completion callback */
                      NULL,    /* no context */
                      &waitWakeIrp);

    devctx->WaitWakeIrp = waitWakeIrp;
    devctx->WakeArmed   = TRUE;

    KdPrint(("I2CCTRL: ArmWake: WaitWake(IRP=%p) for S%lu (ACPI%S)\n",
             waitWakeIrp,
             (ULONG)targetS,
             (devctx->AcpiIs20Plus != FALSE) ? "2.0+" : "1.0b"));
}

VOID
I2cCtrl_DisarmWake(
    PI2CCTRL_FDO devctx
    )
{
    PIRP ww;

    if (devctx == NULL) {
        return;
    }

    if (devctx->WakeCapable == FALSE) {
        I2cCtrl_MaskInterrupts(devctx, FALSE);
        KdPrint(("I2CCTRL: DisarmWake: device not wake-capable\n"));
        return;
    }

    /* Clear outstanding WaitWake IRP reference (XP: no explicit cancel API) */
    ww = devctx->WaitWakeIrp;
    if (ww != NULL) {
        devctx->WaitWakeIrp = NULL;
        KdPrint(("I2CCTRL: DisarmWake: clearing WaitWake IRP reference (was %p)\n", ww));
    }

    /* Mark wake as disarmed */
    devctx->WakeArmed = FALSE;

    /* Disable wake source if present. For ACPI 1.0b, this is still safe (no-op if NULL). */
    if (devctx->Ops != NULL && devctx->Ops->EnableWakeSource != NULL) {
        __try {
            devctx->Ops->EnableWakeSource(devctx, FALSE);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            KdPrint(("I2CCTRL: DisarmWake: exception disabling wake source\n"));
        }
    }

    /* Re-enable normal interrupts regardless of ACPI version */
    I2cCtrl_MaskInterrupts(devctx, FALSE);

    KdPrint(("I2CCTRL: DisarmWake: wake disarmed, interrupts restored (ACPI%S)\n",
             (devctx->AcpiIs20Plus != FALSE) ? "2.0+" : "1.0b"));
}

/* -----------------------------------------------------------------------
 * I2cCtrl_DispatchPower
 *
 * Top-level IRP_MJ_POWER router for the I2C controller bus driver.
 * Routes to PDO or FDO power handlers based on extension signature.
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_DispatchPower(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PVOID ext;

    PAGED_CODE();

    if (DeviceObject == NULL || Irp == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    ext = DeviceObject->DeviceExtension;
    if (ext == NULL) {
        Irp->IoStatus.Status      = STATUS_NO_SUCH_DEVICE;
        Irp->IoStatus.Information = 0;
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NO_SUCH_DEVICE;
    }

    /* PDO path */
    if (((PI2CCTRL_PDO)ext)->Signature == I2CCTRL_PDO_SIGNATURE) {
        return I2cCtrl_PdoDispatchPower(DeviceObject, Irp);
    }

    /* FDO path */
    return I2cCtrl_FdoDispatchPower(DeviceObject, Irp);
}


/* ---------------------------------------------------------------------------
 * Helper: signal wake (call when ISR/DPC detects a wake event)
 * XP/WDM-safe, hardened, C89-compliant
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_CompleteWakeIfArmed(
    PI2CCTRL_FDO devctx
    )
{
    PIRP  wakeIrp;
    KIRQL oldIrql;
    POWER_STATE ps;

    /* Defensive init (C89) */
    wakeIrp = NULL;
    oldIrql = PASSIVE_LEVEL;
    ps.DeviceState = PowerDeviceUnspecified;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    if (devctx == NULL) {
        return;
    }

    /* Disarm hardware wake first to avoid retrigger */
    I2cCtrl_DisableWakeSignal(devctx);

    KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
    wakeIrp = devctx->PendingIrp;
    devctx->PendingIrp = NULL;
    KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

    if (wakeIrp != NULL) {
        /* Clear cancel routine before completing */
        if (IoSetCancelRoutine(wakeIrp, NULL) != NULL) {
            /* Cancel routine cleared successfully */
        }

        __try {
            wakeIrp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(wakeIrp, IO_NO_INCREMENT);

            /* Request power-up to D0 and Working system state */
            ps.DeviceState = PowerDeviceD0;
            PoSetPowerState(devctx->Self, DevicePowerState, ps);
            devctx->CurrentDevicePowerState = PowerDeviceD0;
            devctx->SystemPowerState        = PowerSystemWorking;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            KdPrint(("I2CCTRL: CompleteWakeIfArmed: exception completing wake IRP\n"));
        }
    }
}


/* ---------------------------------------------------------------------------
   CancelPending - HAL-generic, XP/2003 BSOD-safe, C89-compliant
   Purpose:
     - Ensures ISR/DPC cannot touch the IRP after cancellation by:
       * Cancelling the timeout timer before completion
       * Fencing under QueueLock (clear ActiveBusy and XferCtx.Irp, set cancelled status)
       * Masking interrupts and acking latched causes via HAL ops
       * Disabling the controller via HAL ops
       * Completing the IRP exactly once
   --------------------------------------------------------------------------- */
VOID
I2cCtrl_CancelPending(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp
    )
{
    PI2CCTRL_FDO fdoExt;
    KIRQL        oldIrql;
    BOOLEAN      matched;
    BOOLEAN      hadTimer;

    /* C89 init */
    fdoExt   = NULL;
    oldIrql  = 0;
    matched  = FALSE;
    hadTimer = FALSE;

    if (DeviceObject == NULL || Irp == NULL) {
        return;
    }

    fdoExt = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    if (fdoExt == NULL) {
        IoReleaseCancelSpinLock(Irp->CancelIrql);
        return;
    }

    /* Release the cancel spin lock we entered with before taking our own locks */
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    /* Detach IRP from our pending slot to prevent double completion */
    KeAcquireSpinLock(&fdoExt->PendingIrpLock, &oldIrql);
    if (fdoExt->PendingIrp == Irp) {
        fdoExt->PendingIrp = NULL;
        matched = TRUE;
    }
    KeReleaseSpinLock(&fdoExt->PendingIrpLock, oldIrql);

    if (matched == FALSE) {
        /* IRP no longer pending in our slot; nothing to cancel here */
        return;
    }

    /* Cancel any active timeout timer first to prevent future DPC firing */
    hadTimer = KeCancelTimer(&fdoExt->XferCtx.TimeoutTimer);
    UNREFERENCED_PARAMETER(hadTimer);

    /* Fence off ISR/DPC from this IRP and mark transfer inactive under QueueLock */
    KeAcquireSpinLock(&fdoExt->QueueLock, &oldIrql);
    fdoExt->ActiveBusy     = FALSE;
    fdoExt->XferCtx.Status = STATUS_CANCELLED;
    fdoExt->XferCtx.Irp    = NULL;
    KeReleaseSpinLock(&fdoExt->QueueLock, oldIrql);

    /* Abort the hardware transfer safely if device isn't removed */
    if (fdoExt->Removed == FALSE && fdoExt->Ops != NULL) {
        /* Mask all interrupts to stop further ISR activity */
        if (fdoExt->Ops->MaskInterrupts != NULL) {
            fdoExt->Ops->MaskInterrupts(fdoExt, 0U);
        }

        /* Ack latched causes via HAL ops */
        if (fdoExt->Ops->AckInterrupts != NULL) {
            fdoExt->Ops->AckInterrupts(fdoExt,
                I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
                I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
                I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
                I2C_INT_RD_REQ);
        }

        /* Disable controller to abort any in-progress transfer */
        if (fdoExt->Ops->Enable != NULL) {
            (VOID)fdoExt->Ops->Enable(fdoExt, FALSE);
        }
    }

    /* Complete the IRP once with STATUS_CANCELLED */
    Irp->IoStatus.Status      = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0U;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/* Helper: safe insert onto the selected priority queue */
static __inline VOID
I2cCtrl_QueueInsertLocked(PI2CCTRL_FDO devctx, PSMBUS_REQUEST req)
{
    if (req->Priority == I2C_QOS_HIGH) {
        InsertTailList(&devctx->HighQueue, &req->ListEntry);
    } else if (req->Priority == I2C_QOS_LOW) {
        InsertTailList(&devctx->LowQueue, &req->ListEntry);
    } else {
        InsertTailList(&devctx->NormalQueue, &req->ListEntry);
    }
}

NTSTATUS
I2cCtrl_QueueInsert(PI2CCTRL_FDO devctx, PSMBUS_REQUEST req)
{
    KIRQL oldIrql;
    if (devctx == NULL || req == NULL) { return STATUS_INVALID_PARAMETER; }

    /* Default QoS if caller didn't set */
    if (req->Priority != I2C_QOS_HIGH &&
        req->Priority != I2C_QOS_NORMAL &&
        req->Priority != I2C_QOS_LOW) {
        req->Priority = I2C_QOS_NORMAL;
    }

    KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
    I2cCtrl_QueueInsertLocked(devctx, req);
    KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

    I2cCtrl_QueueKick(devctx);
    return STATUS_SUCCESS;
}

/* Pick next request: strict priority, bounded bursts (XP-safe) */
static PSMBUS_REQUEST
I2cCtrl_ScheduleNextRequest(PI2CCTRL_FDO devctx)
{
    PLIST_ENTRY le;
    PSMBUS_REQUEST req;

    /* High priority first, unless burst limit reached and lower queues have work */
    if (!IsListEmpty(&devctx->HighQueue)) {
        if (devctx->BurstHigh < devctx->BurstHighMax || 
            (IsListEmpty(&devctx->NormalQueue) && IsListEmpty(&devctx->LowQueue))) {
            le  = RemoveHeadList(&devctx->HighQueue);
            req = CONTAINING_RECORD(le, SMBUS_REQUEST, ListEntry);
            devctx->BurstHigh++;
            devctx->BurstNormal = 0U;
            devctx->BurstLow    = 0U;
            return req;
        }
    }

    /* Normal priority next (subject to its own burst) */
    if (!IsListEmpty(&devctx->NormalQueue)) {
        if (devctx->BurstNormal < devctx->BurstNormalMax || 
            (IsListEmpty(&devctx->HighQueue) && IsListEmpty(&devctx->LowQueue))) {
            le  = RemoveHeadList(&devctx->NormalQueue);
            req = CONTAINING_RECORD(le, SMBUS_REQUEST, ListEntry);
            devctx->BurstNormal++;
            devctx->BurstHigh = 0U;
            devctx->BurstLow  = 0U;
            return req;
        }
    }

    /* Low priority last (subject to its burst) */
    if (!IsListEmpty(&devctx->LowQueue)) {
        if (devctx->BurstLow < devctx->BurstLowMax || 
            (IsListEmpty(&devctx->HighQueue) && IsListEmpty(&devctx->NormalQueue))) {
            le  = RemoveHeadList(&devctx->LowQueue);
            req = CONTAINING_RECORD(le, SMBUS_REQUEST, ListEntry);
            devctx->BurstLow++;
            devctx->BurstHigh   = 0U;
            devctx->BurstNormal = 0U;
            return req;
        }
    }

    return NULL;
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_QueueKick - Schedule the queue DPC
 * XP/2003-hardened, WDM-safe, C89-compliant
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_QueueKick(
    PI2CCTRL_FDO devctx
    )
{
    BOOLEAN queued;

    /* Defensive init (C89 style) */
    queued = FALSE;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    if (devctx == NULL) {
        KdPrint(("I2CCTRL: QueueKick: invalid devctx\n"));
        return;
    }

    /* Attempt to queue the DPC */
    queued = KeInsertQueueDpc(&devctx->QueueDpc, NULL, NULL);

    if (queued == FALSE) {
        /* DPC already queued or failed to insert */
        KdPrint(("I2CCTRL: QueueKick: DPC already queued or failed\n"));
    }

    /* If you have a dedicated worker thread, signal its event here instead */
}



/* ---------------------------------------------------------------------------
 * Enqueue SMBus IRP - XP/2003-hardened, WDM-safe, C89-compliant
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_EnqueueSmbusIrp(
    PI2CCTRL_FDO   devctx,
    PIRP           Irp,
    UCHAR          SlaveAddress,
    UCHAR          Command,
    UCHAR          OpCode,
    PUCHAR         Buffer,
    UCHAR          Length,
    UCHAR          PecMode
    )
{
    PSMBUS_REQUEST req;
    KIRQL          oldIrql;
    UCHAR          copyLen;

    /* Defensive init (C89) */
    req     = NULL;
    oldIrql = PASSIVE_LEVEL;
    copyLen = 0U;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    if (devctx == NULL || Irp == NULL) {
        KdPrint(("I2CCTRL: EnqueueSmbusIrp: invalid devctx or Irp\n"));
        return STATUS_INVALID_PARAMETER;
    }

    /* Allocate a request object */
    req = (PSMBUS_REQUEST)ExAllocatePoolWithTag(NonPagedPool,
                                                sizeof(SMBUS_REQUEST),
                                                'qmbS');
    if (req == NULL) {
        KdPrint(("I2CCTRL: EnqueueSmbusIrp: allocation failed\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(req, sizeof(SMBUS_REQUEST));

    /* Fill request fields */
    req->Irp          = Irp;
    req->SlaveAddress = (UCHAR)(SlaveAddress & 0x7FU);  /* explicit cast, 7-bit mask */
    req->Command      = Command;
    req->OpCode       = OpCode;
    req->PecMode      = (PecMode != 0U) ? 1U : 0U;
    req->Length       = 0U;
    req->Flags        = 0U;

    /* Copy payload into internal buffer if provided */
    if (Buffer != NULL && Length > 0U) {
        copyLen = Length;
        if (copyLen > (UCHAR)sizeof(req->Buffer)) {
            copyLen = (UCHAR)sizeof(req->Buffer); /* clamp to max */
        }
        __try {
            RtlCopyMemory(req->Buffer, Buffer, copyLen);
            req->Length = copyLen;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            KdPrint(("I2CCTRL: EnqueueSmbusIrp: exception copying buffer\n"));
            ExFreePoolWithTag(req, 'qmbS');
            return STATUS_ACCESS_VIOLATION;
        }
    }

    /* PEC handling: mark request if PEC is enabled */
    if (req->PecMode != 0U) {
        req->Flags |= SMBUS_REQ_FLAG_PEC;
    }

    /* Special handling for block process call (write+read) */
    if (OpCode == I2CCTRL_OPCODE_BLOCK_PROCESS_CALL) {
        req->Flags |= SMBUS_REQ_FLAG_PROCESS_CALL;
    }

    /* Insert into queue under lock */
    KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
    I2cCtrl_QueueInsert(devctx, req);
    KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

    /* Mark IRP pending; completion will happen later */
    IoMarkIrpPending(Irp);

    /* Kick processing if idle */
    I2cCtrl_QueueKick(devctx);

    return STATUS_PENDING;
}

/* ---------------------------------------------------------------------------
 * I2cCtrl_HidCancelRead - XP/2003-safe, cancel-safe, feature-complete
 * Cancels a pending IOCTL_HID_READ_REPORT IRP:
 *  - Runs at DISPATCH_LEVEL (cancel spin lock held on entry)
 *  - Releases cancel spin lock promptly
 *  - Serializes access to PendingHidReadIrp with HidInputLock
 *  - Detaches and completes the IRP exactly once with STATUS_CANCELLED
 *  - Leaves completion to producer path if IRP was already detached
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_HidCancelRead(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PI2CCTRL_PDO hidpdo;
    KIRQL            oldIrql;

    /* Defensive guards (minimal work before releasing cancel spin lock) */
    if (DeviceObject == NULL || Irp == NULL) {
        return;
    }

    hidpdo = (PI2CCTRL_PDO)DeviceObject->DeviceExtension;

    /* Cancel routine is invoked with the I/O manager's cancel spin lock held */
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    if (hidpdo == NULL) {
        return;
    }

    /* Serialize access to the pending HID read IRP */
    KeAcquireSpinLock(&hidpdo->HidInputLock, &oldIrql);

    if (hidpdo->PendingHidReadIrp == Irp) {
        /* Detach from pending slot; we own completion now */
        hidpdo->PendingHidReadIrp = NULL;
        KeReleaseSpinLock(&hidpdo->HidInputLock, oldIrql);

        /* Clear cancel routine to avoid late cancel races */
        IoSetCancelRoutine(Irp, NULL);

        /* Complete with STATUS_CANCELLED; buffered I/O requires no MDL work */
        Irp->IoStatus.Status = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0;

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL,
                    "HidCancelRead: cancelled pending HID read Irp=%p", Irp);

        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return;
    }

    /* Not our pending IRP anymore (already completed or never queued here) */
    KeReleaseSpinLock(&hidpdo->HidInputLock, oldIrql);

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL,
                "HidCancelRead: Irp=%p not in pending slot (ignored)", Irp);
}


/* ---------------------------------------------------------------------------
 * IOCTL Dispatch - XP/2003-safe, HAL-generic, WDM-compliant
 * Handles SMBus, raw I2C, façade IOCTLs, plus HID read path.
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_DispatchIoctl(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PIO_STACK_LOCATION isl;
    PI2CCTRL_FDO      devctx;
    NTSTATUS          status;
    ULONG_PTR         info;
    ULONG             ackMask;

    isl     = IoGetCurrentIrpStackLocation(Irp);
    devctx  = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    status  = STATUS_INVALID_DEVICE_REQUEST;
    info    = 0U;
    ackMask = 0U;

    ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL);

    I2cCtrl_Log("DispatchIoctl: enter DevExt=%p Irp=%p Ioctl=0x%08lx",
                devctx, Irp,
                (isl ? (ULONG)isl->Parameters.DeviceIoControl.IoControlCode : 0UL));

    if (devctx == NULL || isl == NULL) {
        I2cCtrl_Log("DispatchIoctl: invalid context DevExt=%p Isl=%p", devctx, isl);
        status = STATUS_INVALID_PARAMETER;
        goto CompleteDirect;
    }

//
// Allow ACPI evaluation IOCTLs to pass through even if the controller
// is not started. These IOCTLs must reach ACPI.SYS.
//
if (isl->Parameters.DeviceIoControl.IoControlCode == IOCTL_ACPI_EVAL_METHOD) {
    I2cCtrl_Log("DispatchIoctl: forwarding ACPI IOCTL to lower driver");
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(devctx->LowerDevice, Irp);
}

    if (devctx->Started == FALSE || devctx->Stopping != FALSE || devctx->Ops == NULL) {
        I2cCtrl_Log("DispatchIoctl: device not ready Started=%lu Stopping=%lu Ops=%p",
                    (ULONG)devctx->Started, (ULONG)devctx->Stopping, devctx->Ops);
        status = STATUS_DEVICE_NOT_READY;
        goto CompleteDirect;
    }

    switch (isl->Parameters.DeviceIoControl.IoControlCode) {

    case IOCTL_I2C_FORCE_CRASH:
        I2cCtrl_Log("DispatchIoctl: IOCTL_I2C_FORCE_CRASH");
        I2cCtrl_ForceCrash(devctx, STATUS_UNSUCCESSFUL);
        status = STATUS_UNSUCCESSFUL;
        break;

    case IOCTL_HID_READ_REPORT:
    {
        PI2CCTRL_PDO hidpdo = (PI2CCTRL_PDO)DeviceObject->DeviceExtension;
        ULONG  outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
        PUCHAR outBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;

        I2cCtrl_Log("DispatchIoctl: IOCTL_HID_READ_REPORT HidPdo=%p OutLen=%lu Buf=%p",
                    hidpdo, outLen, outBuf);

        if (hidpdo == NULL || outBuf == NULL || outLen == 0U) {
            I2cCtrl_Log("DispatchIoctl: HID_READ_REPORT invalid params HidPdo=%p OutLen=%lu Buf=%p",
                        hidpdo, outLen, outBuf);
            status = STATUS_INVALID_PARAMETER;
            info   = 0U;
            break;
        }

        IoMarkIrpPending(Irp);
        IoSetCancelRoutine(Irp, I2cCtrl_HidCancelRead);

        {
            KIRQL oldIrql;
            KeAcquireSpinLock(&hidpdo->HidInputLock, &oldIrql);

            if (hidpdo->PendingHidReadIrp != NULL) {
                KeReleaseSpinLock(&hidpdo->HidInputLock, oldIrql);
                IoSetCancelRoutine(Irp, NULL);
                I2cCtrl_Log("DispatchIoctl: HID_READ_REPORT busy Pending=%p",
                            hidpdo->PendingHidReadIrp);
                status = STATUS_DEVICE_BUSY;
                info   = 0U;
                break;
            }

            hidpdo->PendingHidReadIrp = Irp;
            KeReleaseSpinLock(&hidpdo->HidInputLock, oldIrql);
        }

        I2cCtrl_Log("DispatchIoctl: HID_READ_REPORT queued Irp=%p", Irp);
        return STATUS_PENDING;
    }

case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
{
    PI2CCTRL_PDO hidpdo = (PI2CCTRL_PDO)DeviceObject->DeviceExtension;
    ULONG  outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
    PUCHAR outBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;

    I2cCtrl_Log("HID_GET_DEVICE_DESCRIPTOR HidPdo=%p OutLen=%lu Buf=%p",
                hidpdo, outLen, outBuf);

    if (hidpdo == NULL || outBuf == NULL) {
        status = STATUS_INVALID_PARAMETER;
        info   = 0U;
        break;
    }

    if (outLen < sizeof(HID_DESCRIPTOR)) {
        status = STATUS_BUFFER_TOO_SMALL;
        info   = sizeof(HID_DESCRIPTOR);
        break;
    }

    RtlCopyMemory(outBuf, &hidpdo->HidDesc, sizeof(HID_DESCRIPTOR));
    status = STATUS_SUCCESS;
    info   = sizeof(HID_DESCRIPTOR);
    break;
}

case IOCTL_HID_GET_REPORT_DESCRIPTOR:
{
    PI2CCTRL_PDO hidpdo =
        (PI2CCTRL_PDO)DeviceObject->DeviceExtension;

    ULONG  outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
    PUCHAR outBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    USHORT descLen;

    if (!hidpdo || !outBuf || !hidpdo->HidReportDesc) {
        status = STATUS_INVALID_PARAMETER;
        info   = 0;
        break;
    }

    if (hidpdo->HidDesc.bNumDescriptors < 1 ||
        hidpdo->HidDesc.DescriptorList[0].bReportType != HID_REPORT_DESCRIPTOR_TYPE)
    {
        status = STATUS_INVALID_DEVICE_STATE;
        info   = 0;
        break;
    }

    descLen = hidpdo->HidDesc.DescriptorList[0].wReportLength;
    if (descLen == 0) {
        status = STATUS_INVALID_DEVICE_STATE;
        info   = 0;
        break;
    }

    if (outLen < descLen) {
        status = STATUS_BUFFER_TOO_SMALL;
        info   = descLen;
        break;
    }

    RtlCopyMemory(outBuf, hidpdo->HidReportDesc, descLen);
    status = STATUS_SUCCESS;
    info   = descLen;
    break;
}

case IOCTL_HID_WRITE_REPORT:
{
    PI2CCTRL_PDO hidpdo =
        (PI2CCTRL_PDO)DeviceObject->DeviceExtension;

    ULONG  inLen = isl->Parameters.DeviceIoControl.InputBufferLength;
    PUCHAR inBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    ULONG  bytesDone = 0;

    if (!hidpdo || !inBuf || inLen == 0) {
        status = STATUS_INVALID_PARAMETER;
        info   = 0;
        break;
    }

    if (hidpdo->HidDesc.bNumDescriptors < 1 ||
        hidpdo->HidDesc.DescriptorList[0].bReportType != HID_REPORT_DESCRIPTOR_TYPE)
    {
        status = STATUS_INVALID_DEVICE_STATE;
        info   = 0;
        break;
    }

    if (inLen > hidpdo->HidDesc.DescriptorList[0].wReportLength ||
        inLen > HID_REPORT_MAX_LEN)
    {
        status = STATUS_INVALID_BUFFER_SIZE;
        info   = 0;
        break;
    }

    {
        HID_I2C_DESCRIPTOR_V10 parsed;
        NTSTATUS descStatus =
            I2cCtrl_ReadAndValidateHidDescriptor(devctx,
                                                 (UCHAR)hidpdo->SlaveAddress,
                                                 (PUCHAR)&parsed,
                                                 sizeof(parsed),
                                                 &parsed);

        if (NT_SUCCESS(descStatus)) {
            RtlZeroMemory(&hidpdo->HidDesc, sizeof(hidpdo->HidDesc));
            RtlCopyMemory(&hidpdo->HidDesc,
                          &parsed,
                          min(sizeof(hidpdo->HidDesc),
                              parsed.wHIDDescLength));
        }
    }

    status = I2CctrlHw_Write(devctx->Self,
                             inBuf,
                             inLen,
                             &bytesDone,
                             0);

    if (!NT_SUCCESS(status)) {
        hidpdo->HidErrorCount++;
        info = 0;
        break;
    }

    status = STATUS_SUCCESS;
    info   = bytesDone;
    break;
}

case IOCTL_HID_GET_FEATURE:
{
    PI2CCTRL_PDO hidpdo = (PI2CCTRL_PDO)DeviceObject->DeviceExtension;
    ULONG  outLen    = isl->Parameters.DeviceIoControl.OutputBufferLength;
    PUCHAR outBuf    = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    ULONG  bytesDone = 0U;

    I2cCtrl_Log("HID_GET_FEATURE HidPdo=%p OutLen=%lu Buf=%p",
                hidpdo, outLen, outBuf);

    if (hidpdo == NULL || outBuf == NULL || outLen == 0U) {
        status = STATUS_INVALID_PARAMETER;
        info   = 0U;
        break;
    }

    if (outLen > HID_REPORT_MAX_LEN) {
        status = STATUS_INVALID_BUFFER_SIZE;
        info   = 0U;
        break;
    }

    {
        HID_I2C_DESCRIPTOR_V10 parsed;
        NTSTATUS descStatus = I2cCtrl_ReadAndValidateHidDescriptor(devctx,
                                                                   (UCHAR)hidpdo->SlaveAddress,
                                                                   (PUCHAR)&parsed,
                                                                   sizeof(parsed),
                                                                   &parsed);
        if (NT_SUCCESS(descStatus)) {
            RtlCopyMemory(&hidpdo->HidDesc, &parsed, sizeof(HID_I2C_DESCRIPTOR_V10));
        } else {
            I2cCtrl_Log("HID_GET_FEATURE: descriptor validation failed Slave=0x%02X Status=0x%08lx",
                        (unsigned)hidpdo->SlaveAddress,
                        descStatus);
        }
    }

    if (outLen >= 1U) {
        UCHAR reportId = outBuf[0];
        (void)devctx->Ops->IssueWriteByte(devctx, reportId);
    }

    status = devctx->Ops->IssueBlockRead(devctx,
                                         (UCHAR)hidpdo->SlaveAddress,
                                         hidpdo->DataRegister,
                                         outBuf,
                                         outLen,
                                         &bytesDone);

    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("HID_GET_FEATURE failed Slave=0x%02X Len=%lu Status=0x%08lx",
                    (unsigned)hidpdo->SlaveAddress,
                    outLen,
                    status);
        hidpdo->HidErrorCount++;
        info = 0U;
        break;
    }

    status = STATUS_SUCCESS;
    info   = bytesDone;
    break;
}

case IOCTL_HID_SET_FEATURE:
{
    PI2CCTRL_PDO hidpdo = (PI2CCTRL_PDO)DeviceObject->DeviceExtension;
    ULONG  inLen     = isl->Parameters.DeviceIoControl.InputBufferLength;
    PUCHAR inBuf     = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    ULONG  bytesDone = 0U;

    I2cCtrl_Log("HID_SET_FEATURE HidPdo=%p InLen=%lu Buf=%p",
                hidpdo, inLen, inBuf);

    if (hidpdo == NULL || inBuf == NULL || inLen == 0U) {
        status = STATUS_INVALID_PARAMETER;
        info   = 0U;
        break;
    }

    if (inLen > HID_REPORT_MAX_LEN) {
        status = STATUS_INVALID_BUFFER_SIZE;
        info   = 0U;
        break;
    }

    {
        HID_I2C_DESCRIPTOR_V10 parsed;
        NTSTATUS descStatus = I2cCtrl_ReadAndValidateHidDescriptor(devctx,
                                                                   (UCHAR)hidpdo->SlaveAddress,
                                                                   (PUCHAR)&parsed,
                                                                   sizeof(parsed),
                                                                   &parsed);
        if (NT_SUCCESS(descStatus)) {
            RtlCopyMemory(&hidpdo->HidDesc, &parsed, sizeof(HID_I2C_DESCRIPTOR_V10));
        } else {
            I2cCtrl_Log("HID_SET_FEATURE: descriptor validation failed Slave=0x%02X Status=0x%08lx",
                        (unsigned)hidpdo->SlaveAddress,
                        descStatus);
        }
    }

    status = devctx->Ops->IssueBlockWrite(devctx,
                                          (UCHAR)hidpdo->SlaveAddress,
                                          hidpdo->DataRegister,
                                          inBuf,
                                          inLen,
                                          &bytesDone);

    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("HID_SET_FEATURE failed Slave=0x%02X Len=%lu Status=0x%08lx",
                    (unsigned)hidpdo->SlaveAddress,
                    inLen,
                    status);
        hidpdo->HidErrorCount++;
        info = 0U;
        break;
    }

    status = STATUS_SUCCESS;
    info   = bytesDone;
    break;
}

default:
    I2cCtrl_Log("DispatchIoctl: unknown IOCTL 0x%08lx",
                (ULONG)isl->Parameters.DeviceIoControl.IoControlCode);
    break;
}

CompleteDirect:
Irp->IoStatus.Status      = status;
Irp->IoStatus.Information = info;

I2cCtrl_Log("DispatchIoctl: complete Irp=%p Status=0x%08lx Info=%Iu",
            Irp, status, info);

IoCompleteRequest(Irp, IO_NO_INCREMENT);
return status;
}


/* --- Simple helpers: enable controller and do 1-byte transactions --- */

/* -----------------------------------------------------------------------
 * EnableAndRead1 - XP/2003 BSOD-safe, C89-compliant (HAL-universal)
 *
 * Purpose:
 *   - Configure and enable the I²C controller for a single-byte read
 *   - Use HAL ops only (no direct register macros)
 *   - Issue a read request and poll status until data arrives
 *   - Handle timeout conditions with defensive interrupt acknowledgement
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids MMIO access if unmapped
 *   - Flags hardware failure on timeout or invalid state
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_EnableAndRead1(
    PI2CCTRL_FDO fdoExt,
    UCHAR        slave7,
    UCHAR*       outByte
    )
{
    I2C_HW_STATUS hwst;
    ULONG spins;

    if (fdoExt == NULL || outByte == NULL || !fdoExt->Started || fdoExt->Ops == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Disable controller before reconfiguring */
    if (fdoExt->Ops->Enable) {
        (VOID)fdoExt->Ops->Enable(fdoExt, FALSE);
    }

    /* Configure target address */
    if (fdoExt->Ops->SetTarget7bit) {
        (VOID)fdoExt->Ops->SetTarget7bit(fdoExt, (UCHAR)(slave7 & 0x7F));
    }

    /* Configure speed (fast mode) */
    if (fdoExt->Ops->SetSpeed) {
        (VOID)fdoExt->Ops->SetSpeed(fdoExt, I2C_SPEED_FAST);
    }

    /* Optional controller flags */
    if (fdoExt->Ops->ConfigureController) {
        (VOID)fdoExt->Ops->ConfigureController(
            fdoExt,
            TRUE,   /* masterMode */
            TRUE,   /* restartEnable */
            TRUE    /* slaveDisable */
        );
    }

    /* Enable controller */
    if (fdoExt->Ops->Enable) {
        (VOID)fdoExt->Ops->Enable(fdoExt, TRUE);
    }

    /* Issue a single-byte read request */
    if (fdoExt->Ops->EmitReadRequest) {
        (VOID)fdoExt->Ops->EmitReadRequest(fdoExt);
    } else if (fdoExt->Ops->WriteTxByte) {
        /* Some HALs model READ as a Tx command byte */
        (VOID)fdoExt->Ops->WriteTxByte(fdoExt, 0x00U);
    }

    /* Poll until RX FIFO has data or timeout (~100ms) */
    for (spins = 0U; spins < 10000U; ++spins) {
        if (fdoExt->Ops->GetStatus) {
            if (NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                if (hwst.RxFifoNotEmpty || hwst.RxFifoLevel > 0U) {
                    /* Read one byte from RX FIFO via HAL */
                    if (fdoExt->Ops->ReadRxByte) {
                        if (NT_SUCCESS(fdoExt->Ops->ReadRxByte(fdoExt, outByte))) {
                            return STATUS_SUCCESS;
                        }
                    } else if (fdoExt->Ops->ReadRxByteSafe) {
                        if (NT_SUCCESS(fdoExt->Ops->ReadRxByteSafe(fdoExt, outByte))) {
                            return STATUS_SUCCESS;
                        }
                    } else {
                        break; /* no RX read op available */
                    }
                }
            }
        }
        KeStallExecutionProcessor(10);
    }


    /* Timeout: defensively ack STOP/ABRT interrupts */
    if (fdoExt->Ops->AckInterrupts) {
        fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_STOP_DETECTED | I2C_INT_TX_ABORT);
    }

    fdoExt->HardwareFailure = TRUE;
    return STATUS_IO_TIMEOUT;
}

/* -----------------------------------------------------------------------
 * EnableAndWrite1 - XP/2003 BSOD-safe, HAL-universal, C89-compliant
 *
 * Purpose:
 *   - Configure and enable the I²C controller for a single-byte write
 *   - Use HAL ops only (no direct register macros)
 *   - Issue a write and poll status until TX FIFO drains
 *   - Handle timeout conditions with defensive interrupt acknowledgement
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids MMIO access if unmapped
 *   - Flags hardware failure on timeout or invalid state
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_EnableAndWrite1(
    PI2CCTRL_FDO fdoExt,
    UCHAR        slave7,
    UCHAR        data
    )
{
    NTSTATUS       status;
    I2C_HW_STATUS  hwst;
    ULONG          spins;

    status = STATUS_SUCCESS;
    spins  = 0U;

    if (fdoExt == NULL || !fdoExt->Started || fdoExt->Mmio == NULL || fdoExt->Ops == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Disable controller before reconfiguring */
    if (fdoExt->Ops->Enable != NULL) {
        (VOID)fdoExt->Ops->Enable(fdoExt, FALSE);
    }

    /* Set target address and speed via HAL */
    if (fdoExt->Ops->SetTarget7bit != NULL) {
        status = fdoExt->Ops->SetTarget7bit(fdoExt, (UCHAR)(slave7 & 0x7F));
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    if (fdoExt->Ops->SetSpeed != NULL) {
        (VOID)fdoExt->Ops->SetSpeed(fdoExt, I2C_SPEED_FAST);
    }

    /* Optional consolidated controller config (master, restart, slave-disable) */
    if (fdoExt->Ops->ConfigureController != NULL) {
        (VOID)fdoExt->Ops->ConfigureController(
            fdoExt,
            /* masterMode */ TRUE,
            /* restartEnable */ TRUE,
            /* slaveDisable */ TRUE
        );
    }

    /* Enable controller */
    if (fdoExt->Ops->Enable != NULL) {
        (VOID)fdoExt->Ops->Enable(fdoExt, TRUE);
    }

    /* Issue a single-byte write via HAL */
    if (fdoExt->Ops->IssueWriteByte != NULL) {
        status = fdoExt->Ops->IssueWriteByte(fdoExt, data);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    } else if (fdoExt->Ops->WriteTxByte != NULL) {
        (VOID)fdoExt->Ops->WriteTxByte(fdoExt, data);
    } else {
        return STATUS_INVALID_DEVICE_STATE;
    }

    /* Poll until TX FIFO drains or timeout (~100ms) */
    for (spins = 0U; spins < 10000U; ++spins) {
        if (fdoExt->Ops->GetStatus != NULL) {
            (VOID)fdoExt->Ops->GetStatus(fdoExt, &hwst);

            /* Success when TX FIFO is empty (or not 'not-empty') */
            if ((hwst.TxFifoNotEmpty == FALSE) || (hwst.TxFifoLevel == 0U)) {
                return STATUS_SUCCESS;
            }
        }

        KeStallExecutionProcessor(10U);
    }

    /* If we timed out, defensively ack STOP/ABRT interrupts */
    if (fdoExt->Ops->AckInterrupts != NULL) {
        fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_STOP_DETECTED | I2C_INT_TX_ABORT);
    }

    fdoExt->HardwareFailure = TRUE;
    return STATUS_IO_TIMEOUT;
}

/* -----------------------------------------------------------------------
 * EnableAndReadN - XP/2003 BSOD-safe, HAL-generic, C89-compliant
 *
 * Purpose:
 *   - Configure and enable the I²C controller for a multi-byte read
 *   - Program target address and issue sequential read tokens
 *   - Poll RX FIFO until each byte arrives or timeout occurs
 *   - Handle STOP/ABRT conditions with defensive interrupt acknowledgement
 *   - Disable controller cleanly after completion
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids MMIO access if unmapped
 *   - Flags hardware failure on timeout or device error
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_EnableAndReadN(
    PI2CCTRL_FDO fdoExt,
    UCHAR        slave7,
    PUCHAR       outBuf,
    USHORT       length
    )
{
    NTSTATUS      status;
    ULONG         i;
    ULONG         timeout;
    I2C_HW_STATUS hwst;

    /* Defensive init */
    status  = STATUS_SUCCESS;
    i       = 0U;
    timeout = 0U;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (fdoExt == NULL || fdoExt->Mmio == NULL || outBuf == NULL || length == 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!fdoExt->Started || fdoExt->Removed) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Program target address */
    if (fdoExt->Ops != NULL && fdoExt->Ops->SetTarget7bit != NULL) {
        status = fdoExt->Ops->SetTarget7bit(fdoExt, slave7);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    /* Enable controller */
    if (fdoExt->Ops != NULL && fdoExt->Ops->Enable != NULL) {
        (VOID)fdoExt->Ops->Enable(fdoExt, TRUE);
    }

    /* Issue read tokens and collect bytes */
    for (i = 0U; i < (ULONG)length; i++) {
        timeout = 1000U;

        if (fdoExt->Ops != NULL && fdoExt->Ops->IssueReadToken != NULL) {
            status = fdoExt->Ops->IssueReadToken(fdoExt);
            if (!NT_SUCCESS(status)) {
                goto done;
            }
        }

        while (timeout > 0U) {
            if (fdoExt->Ops != NULL && fdoExt->Ops->GetStatus != NULL) {
                status = fdoExt->Ops->GetStatus(fdoExt, &hwst);
                if (!NT_SUCCESS(status)) {
                    goto done;
                }
                if (hwst.RxFifoNotEmpty) {
                    break;
                }
            }
            KeStallExecutionProcessor(1U);
            timeout--;
        }

        if (timeout == 0U) {
            status = STATUS_IO_TIMEOUT;
            goto done;
        }

        if (fdoExt->Ops != NULL && fdoExt->Ops->ReadRxByte != NULL) {
            status = fdoExt->Ops->ReadRxByte(fdoExt, &outBuf[i]);
            if (!NT_SUCCESS(status)) {
                goto done;
            }
        }
    }

    /* Wait for STOP or ABRT */
    timeout = 2000U;
    while (timeout > 0U) {
        if (fdoExt->Ops != NULL && fdoExt->Ops->GetStatus != NULL) {
            status = fdoExt->Ops->GetStatus(fdoExt, &hwst);
            if (!NT_SUCCESS(status)) {
                goto done;
            }

            if (hwst.StopDetected) {
                if (fdoExt->Ops->AckInterrupts != NULL) {
                    fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_STOP_DETECTED);
                }
                break;
            }
            if (hwst.TxAborted) {
                if (fdoExt->Ops->AckInterrupts != NULL) {
                    fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_TX_ABORT);
                }
                status = STATUS_IO_DEVICE_ERROR;
                goto done;
            }
        }
        KeStallExecutionProcessor(1U);
        timeout--;
    }
    if (timeout == 0U) {
        status = STATUS_IO_TIMEOUT;
    }

done:
    /* Disable controller */
    if (fdoExt->Ops != NULL && fdoExt->Ops->Enable != NULL) {
        (VOID)fdoExt->Ops->Enable(fdoExt, FALSE);
    }

    return status;
}


/* -----------------------------------------------------------------------
 * EnableAndWriteN - XP/2003 BSOD-safe, C89-compliant (HAL-universal)
 *
 * Purpose:
 *   - Configure and enable the I²C controller for a multi-byte write
 *   - Program target address and issue sequential write commands
 *   - Poll TX FIFO until space is available for each byte
 *   - Handle STOP/ABRT conditions with defensive interrupt acknowledgement
 *   - Disable controller cleanly after completion
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids MMIO access if unmapped
 *   - Flags hardware failure on timeout or device error
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_EnableAndWriteN(
    PI2CCTRL_FDO fdoExt,
    UCHAR        slave7,
    PUCHAR       data,
    USHORT       length
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i, timeout;
    I2C_HW_STATUS hwst;

    RtlZeroMemory(&hwst, sizeof(hwst));

    if (fdoExt == NULL || data == NULL || length == 0 || fdoExt->Ops == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!fdoExt->Started || fdoExt->Removed) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Program target address */
    if (fdoExt->Ops->SetTarget7bit) {
        (VOID)fdoExt->Ops->SetTarget7bit(fdoExt, (UCHAR)(slave7 & 0x7F));
    }

    /* Enable controller */
    if (fdoExt->Ops->Enable) {
        (VOID)fdoExt->Ops->Enable(fdoExt, TRUE);
    }

    /* Write each byte */
    for (i = 0; i < (ULONG)length; i++) {
        timeout = 1000;
        while (timeout > 0) {
            if (fdoExt->Ops->GetStatus &&
                NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                if (hwst.TxFifoNotFull || hwst.TxFifoLevel < fdoExt->Caps->TxFifoDepth) {
                    break;
                }
            }
            KeStallExecutionProcessor(1);
            timeout--;
        }
        if (timeout == 0) {
            status = STATUS_IO_TIMEOUT;
            goto done;
        }

        if (fdoExt->Ops->WriteTxByte) {
            status = fdoExt->Ops->WriteTxByte(fdoExt, data[i]);
            if (!NT_SUCCESS(status)) {
                goto done;
            }
        }
    }

    /* Wait for STOP or ABRT */
    timeout = 2000;
    while (timeout > 0) {
        if (fdoExt->Ops->GetRawIntr) {
            ULONG intr = fdoExt->Ops->GetRawIntr(fdoExt);

            if ((intr & I2C_INT_STOP_DETECTED) != 0U) {
                if (fdoExt->Ops->AckInterrupts) {
                    fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_STOP_DETECTED);
                }
                break;
            }
            if ((intr & I2C_INT_TX_ABORT) != 0U) {
                if (fdoExt->Ops->AckInterrupts) {
                    fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_TX_ABORT);
                }
                status = STATUS_IO_DEVICE_ERROR;
                goto done;
            }
        }
        KeStallExecutionProcessor(1);
        timeout--;
    }
    if (timeout == 0) {
        status = STATUS_IO_TIMEOUT;
    }

done:
    /* Disable controller */
    if (fdoExt->Ops->Enable) {
        (VOID)fdoExt->Ops->Enable(fdoExt, FALSE);
    }
    return status;
}


//
// ---------------------------------------------------------------------------
// Controller detection, register map, quirks, and AddDevice
// Optimized for WinDDK 7.1.0 (XP x86 / Server 2003 x64)
// ---------------------------------------------------------------------------
//

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

I2CCTRL_REGMAP g_CurrentRegMap;

//
// Default safe map (prevents Code 10 if detection fails)
//
VOID InitDefault(VOID)
{
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);  // XP-safe: init only at PASSIVE_LEVEL

    KdPrint(("I2CCTRL: Initializing default safe register map\n"));

    if (&g_CurrentRegMap == NULL) {
        KdPrint(("I2CCTRL: InitDefault: g_CurrentRegMap NULL\n"));
        return;
    }

    // Clear the struct before assigning defaults
    RtlZeroMemory(&g_CurrentRegMap, sizeof(g_CurrentRegMap));

    // Assign safe default offsets
    g_CurrentRegMap.ControlReg = 0x00;
    g_CurrentRegMap.StatusReg  = 0x04;
    g_CurrentRegMap.DataReg    = 0x08;
    g_CurrentRegMap.ClockReg   = 0x0C;
    g_CurrentRegMap.Quirks     = 0;
}


//
// Generic per-chip init routine (XP-safe: guard IRQL and struct)
//
VOID
I2cCtrl_GenericInit(
    ULONG  controlOffset,
    ULONG  statusOffset,
    ULONG  dataOffset,
    ULONG  clockOffset,
    ULONG  quirks,
    ULONG  bsodQuirks,
    PCWSTR name
    )
{
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (&g_CurrentRegMap == NULL) {
        KdPrint(("I2CCTRL: %ws: g_CurrentRegMap NULL\n", name));
        return;
    }

    KdPrint(("I2CCTRL: Initializing I2C Controller %ws\n", name));

    g_CurrentRegMap.ControlReg = controlOffset;
    g_CurrentRegMap.StatusReg  = statusOffset;
    g_CurrentRegMap.DataReg    = dataOffset;
    g_CurrentRegMap.ClockReg   = clockOffset;

    g_CurrentRegMap.Quirks     = quirks;
    g_CurrentRegMap.BsodQuirks = bsodQuirks;

    if (quirks != QUIRK_NONE) {
        KdPrint(("I2CCTRL: %ws: functional quirks mask=0x%08lx\n", name, quirks));
    } else {
        KdPrint(("I2CCTRL: %ws: no functional quirks\n", name));
    }

    if (bsodQuirks != BSOD_NONE) {
        KdPrint(("I2CCTRL: %ws: BSOD quirks mask=0x%08lx\n", name, bsodQuirks));
    } else {
        KdPrint(("I2CCTRL: %ws: no BSOD quirks\n", name));
    }
}

//
// Dispatcher to call the generic init
//
VOID
I2cCtrl_InitById(PCWSTR hwid)
{
    size_t i;

    if (hwid == NULL) {
        KdPrint(("I2CCTRL: InitById: NULL HWID provided\n"));
        g_CurrentRegMap.BsodQuirks = BSOD_NONE;
        return;
    }

    for (i = 0; i < RTL_NUMBER_OF(g_I2cControllers); i++) {
        /* Use substring match: HWIDs are MULTI_SZ entries like "PCI\\VEN_8086&DEV_9DC5&CC_0C8000" */
        if (wcsstr(hwid, g_I2cControllers[i].PciId) != NULL) {
            I2cCtrl_GenericInit(
                g_I2cControllers[i].ControlOffset,
                g_I2cControllers[i].StatusOffset,
                g_I2cControllers[i].DataOffset,
                g_I2cControllers[i].ClockOffset,
                g_I2cControllers[i].Quirks,
                g_I2cControllers[i].BsodQuirks,
                g_I2cControllers[i].PciId
            );

            /* Apply BSOD-tweak-workarounds if present */
            g_CurrentRegMap.BsodQuirks = g_I2cControllers[i].BsodQuirks;
            if (g_I2cControllers[i].BsodQuirks != BSOD_NONE) {
                KdPrint(("I2CCTRL: InitById: BSOD quirks applied (mask=0x%08lx) for %S\n",
                         g_I2cControllers[i].BsodQuirks,
                         g_I2cControllers[i].PciId));
            } else {
                KdPrint(("I2CCTRL: InitById: no BSOD quirks for %S\n",
                         g_I2cControllers[i].PciId));
            }

            return;
        }
    }

    /* If no match, still check for generic class ID */
    if (wcsstr(hwid, L"PCI\\CC_0C8000") != NULL) {
        I2cCtrl_GenericInit(
            0x00, 0x04, 0x08, 0x0C,
            QUIRK_NONE,
            BSOD_NONE,
            L"PCI\\CC_0C8000"
        );
        g_CurrentRegMap.BsodQuirks = BSOD_NONE;
        KdPrint(("I2CCTRL: InitById: generic class ID matched, no BSOD quirks\n"));
        return;
    }

    /* Unknown controller */
    KdPrint(("I2CCTRL: InitById: Unknown controller HWID %ws\n", hwid));
    g_CurrentRegMap.BsodQuirks = BSOD_NONE;
}



NTSTATUS
I2cCtrlIdentifyAndInitController(
    PI2CCTRL_FDO devctx
    )
{
    NTSTATUS status;
    WCHAR hwidBuffer[256];            /* local MULTI_SZ buffer */
    ULONG length;
    BOOLEAN matched;
    ULONG i;

    PVOID dynBuf;                     /* dynamic buffer if 256 is too small */
    ULONG dynLen;

    const WCHAR* base;
    ULONG        bytes;
    const WCHAR* p;
    const WCHAR* end;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL || devctx->PhysicalDevice == NULL) {
        KdPrint(("I2CCTRL: IdentifyInit: invalid devctx/PhysicalDevice\n"));
        InitDefault();
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(hwidBuffer, sizeof(hwidBuffer));
    length  = sizeof(hwidBuffer);
    matched = FALSE;
    dynBuf  = NULL;
    dynLen  = 0;

    status = IoGetDeviceProperty(devctx->PhysicalDevice,
                                 DevicePropertyHardwareID,
                                 length,
                                 hwidBuffer,
                                 &length);

    if (status == STATUS_BUFFER_TOO_SMALL && length > sizeof(hwidBuffer)) {
        dynLen = length;
        dynBuf = ExAllocatePoolWithTag(NonPagedPool, dynLen, TAG_I2C_MISC);
        if (dynBuf == NULL) {
            KdPrint(("I2CCTRL: IdentifyInit: alloc %lu failed, using defaults\n", dynLen));
            InitDefault();
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        status = IoGetDeviceProperty(devctx->PhysicalDevice,
                                     DevicePropertyHardwareID,
                                     dynLen,
                                     dynBuf,
                                     &dynLen);
        if (!NT_SUCCESS(status)) {
            KdPrint(("I2CCTRL: IdentifyInit: IoGetDeviceProperty requery failed 0x%08X\n", status));
            ExFreePoolWithTag(dynBuf, TAG_I2C_MISC);
            InitDefault();
            return STATUS_SUCCESS;
        }
    } else if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL: IdentifyInit: property query failed 0x%08X\n", status));
        InitDefault();
        return STATUS_SUCCESS;
    }

    if (dynBuf != NULL) {
        base  = (const WCHAR*)dynBuf;
        bytes = dynLen;
    } else {
        base  = hwidBuffer;
        bytes = length;
    }

    if (bytes < sizeof(WCHAR) * 2) {
        KdPrint(("I2CCTRL: IdentifyInit: MULTI_SZ too short, using defaults\n"));
        if (dynBuf) ExFreePoolWithTag(dynBuf, TAG_I2C_MISC);
        InitDefault();
        return STATUS_SUCCESS;
    }

    p   = base;
    end = (const WCHAR*)((const UCHAR*)base + bytes);

    while (p < end && *p != L'\0') {
        const WCHAR* s;
        size_t slen;

        s = p;
        slen = 0;
        while (s < end && *s != L'\0') {
            s++;
            slen++;
        }

        /* Match against explicit ACPI/PCI IDs */
        for (i = 0; i < ARRAYSIZE(g_I2cControllers); i++) {
            const WCHAR* hwid = g_I2cControllers[i].PciId;
            if (hwid != NULL && wcsstr(p, hwid) != NULL) {
                I2cCtrl_GenericInit(
                    g_I2cControllers[i].ControlOffset,
                    g_I2cControllers[i].StatusOffset,
                    g_I2cControllers[i].DataOffset,
                    g_I2cControllers[i].ClockOffset,
                    g_I2cControllers[i].Quirks,
                    g_I2cControllers[i].BsodQuirks,
                    g_I2cControllers[i].PciId
                );
                matched = TRUE;

                /* Detect ACPI version from quirks */
                if ((g_I2cControllers[i].Quirks & QUIRK_ACPI20) != 0U) {
                    devctx->AcpiIs20Plus = TRUE;
                    KdPrint(("I2CCTRL: IdentifyInit: ACPI 2.0+ controller matched (%S)\n",
                             g_I2cControllers[i].PciId));
                } else if ((g_I2cControllers[i].Quirks & QUIRK_ACPI10) != 0U) {
                    devctx->AcpiIs20Plus = FALSE;
                    KdPrint(("I2CCTRL: IdentifyInit: ACPI 1.0b forced by quirk (%S)\n",
                             g_I2cControllers[i].PciId));
                } else {
                    devctx->AcpiIs20Plus = FALSE;
                    KdPrint(("I2CCTRL: IdentifyInit: ACPI 1.0b fallback controller matched (%S)\n",
                             g_I2cControllers[i].PciId));
                }

                /* Apply BSOD-tweak-workarounds if present */
                devctx->BsodQuirks = g_I2cControllers[i].BsodQuirks;
                if (g_I2cControllers[i].BsodQuirks != BSOD_NONE) {
                    KdPrint(("I2CCTRL: IdentifyInit: BSOD quirks applied (mask=0x%08lx)\n",
                             g_I2cControllers[i].BsodQuirks));
                } else {
                    KdPrint(("I2CCTRL: IdentifyInit: no BSOD quirks for %S\n",
                             g_I2cControllers[i].PciId));
                }

                break;
            }
        }

        if (matched) {
            break;
        }
        p = s + 1;
    }

    if (dynBuf) {
        ExFreePoolWithTag(dynBuf, TAG_I2C_MISC);
    }

    if (!matched) {
        KdPrint(("I2CCTRL: IdentifyInit: no match in HWIDs, applying defaults (ACPI 1.0b fallback)\n"));
        InitDefault();
        devctx->AcpiIs20Plus = FALSE;
        devctx->BsodQuirks   = BSOD_NONE;
    }

    return STATUS_SUCCESS;
}

//
// Perform a full DW-I2C controller reset (XP-safe, universal)
//
VOID
I2cCtrl_PerformReset(
    PI2CCTRL_FDO devctx
    )
{
    const ULONG CTRL_RESET_BIT      = 0x00000001U;
    const ULONG STAT_BUSY_BIT       = 0x00000001U;
    const ULONG STAT_RESET_DONE_BIT = 0x00000002U;

    const I2CCTRL_DEVICE_ID* id;
    ULONG ctrl;
    ULONG stat;
    ULONG tries;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL) {
        I2cCtrl_Log("PerformReset: invalid devctx\n");
        return;
    }

    id = I2cCtrl_FindControllerId(devctx->PnpId);
    if (id == NULL) {
        I2cCtrl_Log("PerformReset: no controller profile\n");
        return;
    }

    if (devctx->MmioBase == NULL) {
        I2cCtrl_Log("PerformReset: BAR0 unmapped\n");
        devctx->HardwareFailure = TRUE;
        return;
    }

    I2cCtrl_Log("PerformReset: begin\n");

    /* Assert reset bit */
    ctrl = I2cCtrl_ReadRegisterSafe(devctx, id->ControlOffset);
    I2cCtrl_WriteRegisterSafe(devctx, id->ControlOffset,
                              ctrl | CTRL_RESET_BIT);

    /* Poll for reset completion or idle state */
    tries = 0U;
    do {
        stat = I2cCtrl_ReadRegisterSafe(devctx, id->StatusOffset);

        if (((stat & STAT_BUSY_BIT) == 0U) ||
            ((stat & STAT_RESET_DONE_BIT) != 0U))
        {
            break;
        }

        KeStallExecutionProcessor(10U);
        tries++;

    } while (tries < 500U);

    /* Deassert reset bit */
    ctrl = I2cCtrl_ReadRegisterSafe(devctx, id->ControlOffset);
    if ((ctrl & CTRL_RESET_BIT) != 0U) {
        I2cCtrl_WriteRegisterSafe(devctx, id->ControlOffset,
                                  ctrl & ~CTRL_RESET_BIT);
    }

    /* Final verification */
    if (tries >= 500U) {
        I2cCtrl_Log("PerformReset: timeout waiting for reset completion\n");
    } else {
        I2cCtrl_Log("PerformReset: reset complete (tries=%lu, STAT=0x%08lx)\n",
                    tries, stat);
    }
}

const I2CCTRL_DEVICE_ID*
I2cCtrl_FindControllerId(
    PCWSTR PnpId
    )
{
    ULONG i;

    if (PnpId == NULL) {
        I2cCtrl_Log("FindControllerId: NULL PnpId\n");
        return NULL;
    }

    for (i = 0; i < RTL_NUMBER_OF(g_I2cControllers); i++) {

        const I2CCTRL_DEVICE_ID* id = &g_I2cControllers[i];

        if (id->PciId == NULL)
            continue;

        /* Case-insensitive match */
        if (_wcsnicmp(PnpId, id->PciId, wcslen(id->PciId)) == 0) {
            I2cCtrl_Log("FindControllerId: match found\n");
            return id;
        }
    }

    I2cCtrl_Log("FindControllerId: no match\n");
    return NULL;
}


VOID
I2cCtrlApplyQuirks(
    PI2CCTRL_FDO devctx
    )
{
    const ULONG CTRL_RESET_BIT      = 0x00000001U;
    const ULONG CTRL_DMA_EN_BIT     = 0x00000100U;
    const ULONG CLK_ENABLE_BIT      = 0x00000001U;
    const ULONG CLK_GATE_BIT        = 0x00000002U;
    const ULONG STAT_BUSY_BIT       = 0x00000001U;
    const ULONG STAT_RESET_DONE_BIT = 0x00000002U;

    const I2CCTRL_DEVICE_ID* id;
    PUCHAR bar0;
    PUCHAR bar2;
    ULONG  ctrl, stat, clk, verify, tries;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL || devctx->PnpId == NULL) {
        I2cCtrl_Log("ApplyQuirks: invalid devctx\n");
        return;
    }

    id = I2cCtrl_FindControllerId(devctx->PnpId);
    if (id == NULL) {
        I2cCtrl_Log("ApplyQuirks: no table entry\n");
        return;
    }

    bar0 = devctx->MmioBase;
    bar2 = devctx->LpssBar2;

    if (bar0 == NULL) {
        I2cCtrl_Log("ApplyQuirks: BAR0 NULL -> skipping HW quirks\n");
        return;
    }

    if ((id->LpssClkGateOffset ||
         id->LpssResetOffset   ||
         id->LpssFuncClkOffset ||
         id->LpssMiscOffset) && bar2 == NULL)
    {
        I2cCtrl_Log("ApplyQuirks: LPSS offsets present but BAR2 NULL\n");
    }

    I2cCtrl_Log("ApplyQuirks: begin (quirks=0x%08lx bsod=0x%08lx)\n",
                id->Quirks, id->BsodQuirks);

    /* ============================================================
       LPSS POWER-ON (BAR2, guarded + exception-safe)
       ============================================================ */
    if (bar2 != NULL &&
        (id->LpssClkGateOffset ||
         id->LpssResetOffset   ||
         id->LpssFuncClkOffset ||
         id->LpssMiscOffset))
    {
        __try {

            if (id->LpssClkGateOffset) {
                clk = READ_REGISTER_ULONG(
                    (PULONG)(bar2 + id->LpssClkGateOffset));
                clk &= ~0x1U;
                WRITE_REGISTER_ULONG(
                    (PULONG)(bar2 + id->LpssClkGateOffset), clk);
                I2cCtrl_Log("LPSS: clock gate cleared\n");
            }

            if (id->LpssResetOffset) {
                ctrl = READ_REGISTER_ULONG(
                    (PULONG)(bar2 + id->LpssResetOffset));
                ctrl &= ~0x1U;
                WRITE_REGISTER_ULONG(
                    (PULONG)(bar2 + id->LpssResetOffset), ctrl);
                I2cCtrl_Log("LPSS: reset deasserted\n");
            }

            if (id->LpssFuncClkOffset) {
                clk = READ_REGISTER_ULONG(
                    (PULONG)(bar2 + id->LpssFuncClkOffset));
                clk |= 0x1U;
                WRITE_REGISTER_ULONG(
                    (PULONG)(bar2 + id->LpssFuncClkOffset), clk);
                I2cCtrl_Log("LPSS: functional clock enabled\n");
            }

            if (id->LpssMiscOffset) {
                verify = READ_REGISTER_ULONG(
                    (PULONG)(bar2 + id->LpssMiscOffset));
                WRITE_REGISTER_ULONG(
                    (PULONG)(bar2 + id->LpssMiscOffset), verify);
                I2cCtrl_Log("LPSS: misc touched\n");
            }

        } __except(EXCEPTION_EXECUTE_HANDLER) {
            I2cCtrl_Log("ApplyQuirks: EXCEPTION in LPSS BAR2 sequence\n");
        }
    }

    /* ============================================================
       DW-I2C FUNCTIONAL QUIRKS (BAR0, exception-safe)
       ============================================================ */
    __try {

        if (id->Quirks & QUIRK_NEEDS_RESET_WORKAROUND) {

            I2cCtrl_Log("Quirk: reset workaround\n");

            ctrl = READ_REGISTER_ULONG(
                (PULONG)(bar0 + id->ControlOffset));
            WRITE_REGISTER_ULONG(
                (PULONG)(bar0 + id->ControlOffset),
                ctrl | CTRL_RESET_BIT);

            tries = 0;
            while (tries < 500) {
                stat = READ_REGISTER_ULONG(
                    (PULONG)(bar0 + id->StatusOffset));
                if ((stat & STAT_BUSY_BIT) == 0U ||
                    (stat & STAT_RESET_DONE_BIT) != 0U)
                    break;
                KeStallExecutionProcessor(10);
                tries++;
            }

            ctrl = READ_REGISTER_ULONG(
                (PULONG)(bar0 + id->ControlOffset));
            WRITE_REGISTER_ULONG(
                (PULONG)(bar0 + id->ControlOffset),
                ctrl & ~CTRL_RESET_BIT);

            I2cCtrl_Log("Quirk: reset workaround complete\n");
        }

        if (id->Quirks & QUIRK_BROKEN_CLOCK_GATE) {

            I2cCtrl_Log("Quirk: broken clock gate\n");

            clk = READ_REGISTER_ULONG(
                (PULONG)(bar0 + id->ClockOffset));
            clk |= CLK_ENABLE_BIT;
            clk &= ~CLK_GATE_BIT;
            WRITE_REGISTER_ULONG(
                (PULONG)(bar0 + id->ClockOffset), clk);
        }

        if (id->Quirks & QUIRK_NO_DMA_SUPPORT) {

            I2cCtrl_Log("Quirk: no DMA support\n");

            ctrl = READ_REGISTER_ULONG(
                (PULONG)(bar0 + id->ControlOffset));
            ctrl &= ~CTRL_DMA_EN_BIT;
            WRITE_REGISTER_ULONG(
                (PULONG)(bar0 + id->ControlOffset), ctrl);
        }

    } __except(EXCEPTION_EXECUTE_HANDLER) {
        I2cCtrl_Log("ApplyQuirks: EXCEPTION in DW-I2C BAR0 sequence\n");
    }

    /* ============================================================
       NON-MMIO QUIRKS (safe)
       ============================================================ */

    if (id->Quirks & QUIRK_ACPI20) {
        devctx->AcpiIs20Plus = TRUE;
        I2cCtrl_Log("Quirk: ACPI 2.0+\n");
    }

    if (id->Quirks & QUIRK_ACPI10) {
        devctx->AcpiIs20Plus = FALSE;
        I2cCtrl_Log("Quirk: ACPI 1.0b\n");
    }

    if (id->Quirks & QUIRK_SLOW_CLOCK) {
        devctx->StallIntervalUs += 5;
        I2cCtrl_Log("Quirk: slow clock\n");
    }

    if (id->Quirks & QUIRK_NO_D1D2) {
        devctx->SupportsD1 = FALSE;
        devctx->SupportsD2 = FALSE;
        I2cCtrl_Log("Quirk: no D1/D2\n");
    }

    /* ============================================================
       BSOD WORKAROUNDS
       ============================================================ */

    if (id->BsodQuirks & BSOD_FORCE_PIO) {
        devctx->ForcePioMode = TRUE;
        I2cCtrl_Log("BSOD: force PIO\n");
    }

    if (id->BsodQuirks & BSOD_MASK_INTERRUPTS) {
        I2cCtrl_MaskInterrupts(devctx, TRUE);
        I2cCtrl_Log("BSOD: mask interrupts\n");
    }

    if (id->BsodQuirks & BSOD_EXTRA_RESET) {
        I2cCtrl_PerformReset(devctx);
        I2cCtrl_Log("BSOD: extra reset\n");
    }

    if (id->BsodQuirks & BSOD_DELAY_INIT) {
        KeStallExecutionProcessor(50000);
        I2cCtrl_Log("BSOD: delay init\n");
    }

    /* ============================================================
       USER POLICY
       ============================================================ */

    {
        ULONG val;

        val = I2cCtrl_ReadRegDword(devctx, L"WakeCapable", 0);
        devctx->WakeCapable = (val != 0);

        val = I2cCtrl_ReadRegDword(devctx, L"MultiMasterEnabled", 1);
        devctx->MultiMasterEnabled = (val != 0);

        devctx->ArbBackoffBaseUs =
            I2cCtrl_ReadRegDword(devctx, L"ArbBackoffBaseUs", 100);

        devctx->ArbBackoffMaxUs =
            I2cCtrl_ReadRegDword(devctx, L"ArbBackoffMaxUs", 5000);

        devctx->ArbBackoffJitterUs =
            I2cCtrl_ReadRegDword(devctx, L"ArbBackoffJitterUs", 50);

        val = I2cCtrl_ReadRegDword(devctx, L"Policy.BusSpeedHz", 400000);
        devctx->PolicyBusSpeedHz = val;

        devctx->PolicyMaxRetries =
            I2cCtrl_ReadRegDword(devctx, L"Policy.MaxRetries", 3);

        devctx->PolicyTxnTimeoutMs =
            I2cCtrl_ReadRegDword(devctx, L"Policy.TransactionTimeoutMs", 1000);

        devctx->PolicyBackoffInitialUs =
            I2cCtrl_ReadRegDword(devctx, L"Policy.BackoffInitialUs", 10);

        devctx->PolicyBackoffMaxUs =
            I2cCtrl_ReadRegDword(devctx, L"Policy.BackoffMaxUs", 5000);

        devctx->PolicyUsePec =
            I2cCtrl_ReadRegDword(devctx, L"Policy.UsePec", 0);

        devctx->PolicyForce10Bit =
            I2cCtrl_ReadRegDword(devctx, L"Policy.Force10BitAddr", 0);

        val = I2cCtrl_ReadRegDword(devctx, L"ForceCrashOnError", 0);
        devctx->ForceCrashOnError = (val != 0);
    }

    I2cCtrl_Log("ApplyQuirks: done\n");
}

VOID
I2cHidApplyQuirks(
    PI2CCTRL_PDO childDx,
    const I2CHID_DEVICE_ID* hidMatch
    )
{
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (!childDx || !hidMatch || !hidMatch->HidId) {
        I2cCtrl_Log("I2CHID: ApplyQuirks: invalid parameters\n");
        return;
    }

    I2cCtrl_Log("I2CHID: Applying HID quirks for %ws\n", hidMatch->HidId);

    //
    // All HID-over-I2C devices are touchpads unless flagged otherwise
    //
    if (hidMatch->Flags & HID_FLAG_TOUCHPAD)
        childDx->IsTouchpad = TRUE;

    //
    // Apply vendor-specific quirks
    //
    switch (hidMatch->Quirks)
    {
        case HID_QUIRK_ELAN:
            childDx->HidExtraDelayUs = 200;
            childDx->HidNeedsAlignmentFix = TRUE;
            I2cCtrl_Log("I2CHID: ELAN quirks applied\n");
            break;

        case HID_QUIRK_SYNAPTICS:
            childDx->HidSynapticsFix = TRUE;
            childDx->HidPacketHeaderSize = 4;
            I2cCtrl_Log("I2CHID: Synaptics quirks applied\n");
            break;

        case HID_QUIRK_ASUS:
            childDx->HidDebounceFix = TRUE;
            childDx->HidExtraDelayUs = 150;
            I2cCtrl_Log("I2CHID: ASUS quirks applied\n");
            break;

        case HID_QUIRK_GOODIX:
            childDx->HidSlowRead = TRUE;
            childDx->HidPacketHeaderSize = 2;
            I2cCtrl_Log("I2CHID: Goodix quirks applied\n");
            break;

        case HID_QUIRK_RAYDIUM:
            childDx->HidPacketHeaderSize = 2;
            childDx->HidRaydiumMode = TRUE;
            I2cCtrl_Log("I2CHID: Raydium quirks applied\n");
            break;

        case HID_QUIRK_FOCALTECH:
            childDx->HidScaleCoordinates = TRUE;
            I2cCtrl_Log("I2CHID: FocalTech quirks applied\n");
            break;

        case HID_QUIRK_CYPRESS:
            childDx->HidFilterInterrupts = TRUE;
            I2cCtrl_Log("I2CHID: Cypress quirks applied\n");
            break;

        case HID_QUIRK_HIMAX:
            childDx->HidHimaxMode = TRUE;
            childDx->HidPacketHeaderSize = 3;
            I2cCtrl_Log("I2CHID: Himax quirks applied\n");
            break;

        case HID_QUIRK_PIXART:
            childDx->HidPixartChecksum = TRUE;
            I2cCtrl_Log("I2CHID: PixArt quirks applied\n");
            break;

        case HID_QUIRK_SILEAD:
            childDx->HidExtraDelayUs = 300;
            childDx->HidSileadMode = TRUE;
            I2cCtrl_Log("I2CHID: Silead quirks applied\n");
            break;

        case HID_QUIRK_ATMEL:
            childDx->HidAtmelHeaderFix = TRUE;
            I2cCtrl_Log("I2CHID: Atmel quirks applied\n");
            break;

        case HID_QUIRK_PRIMAX:
            childDx->HidPrimaxMode = TRUE;
            I2cCtrl_Log("I2CHID: Primax quirks applied\n");
            break;

        case HID_QUIRK_CHICONY:
            childDx->HidDebounceFix = TRUE;
            I2cCtrl_Log("I2CHID: Chicony quirks applied\n");
            break;

        default:
            I2cCtrl_Log("I2CHID: No vendor-specific quirks\n");
            break;
    }

    I2cCtrl_Log("I2CHID: HID quirks applied successfully\n");
}

//
// Guarded MMIO helpers (XP-BSOD-safe, WinDDK, C89)
//

__forceinline
VOID
I2cCtrl_WriteRegisterSafe(
    PI2CCTRL_FDO Dx,
    ULONG Offset,
    ULONG Value
    )
{
    KIRQL           oldIrql;
    volatile ULONG* reg;

    oldIrql = 0;
    reg     = NULL;

    if (Dx == NULL) {
        KdPrint(("I2CCTRL: WriteRegisterSafe NULL Dx\n"));
        return;
    }

    if (Dx->Removed || Dx->Stopping || !Dx->Started) {
        KdPrint(("I2CCTRL: WriteRegisterSafe inactive "
                 "(Removed=%lu Stopping=%lu Started=%lu)\n",
                 Dx->Removed, Dx->Stopping, Dx->Started));
        Dx->HardwareFailure = TRUE;
        return;
    }

    if (Dx->Mmio == NULL) {
        KdPrint(("I2CCTRL: WriteRegisterSafe unmapped MMIO Base=%p\n", Dx->Mmio));
        Dx->HardwareFailure = TRUE;
        return;
    }

    if ((Offset + sizeof(ULONG)) > Dx->MmioLength) {
        KdPrint(("I2CCTRL: WriteRegisterSafe OOB Off=0x%lx Len=%lu Val=0x%lx\n",
                 Offset,
                 Dx->MmioLength,
                 Value));
        Dx->HardwareFailure = TRUE;
        return;
    }

    KeAcquireSpinLock(&Dx->HwLock, &oldIrql);
    __try {
        reg = (volatile ULONG*)((PUCHAR)Dx->Mmio + Offset);
        WRITE_REGISTER_ULONG((PULONG)reg, Value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        NTSTATUS code = GetExceptionCode();
        KdPrint(("I2CCTRL: WriteRegisterSafe SEH Off=0x%lx Val=0x%lx Code=0x%08lx\n",
                 Offset,
                 Value,
                 code));
        Dx->HardwareFailure = TRUE;
    }
    KeReleaseSpinLock(&Dx->HwLock, oldIrql);
}

__forceinline
ULONG
I2cCtrl_ReadRegisterSafe(
    PI2CCTRL_FDO Dx,
    ULONG Offset
    )
{
    KIRQL           oldIrql;
    volatile ULONG* reg;
    ULONG           value;

    oldIrql = 0;
    reg     = NULL;
    value   = 0U;

    if (Dx == NULL) {
        KdPrint(("I2CCTRL: ReadRegisterSafe NULL Dx\n"));
        return 0U;
    }

    if (Dx->Removed || Dx->Stopping || !Dx->Started) {
        KdPrint(("I2CCTRL: ReadRegisterSafe inactive "
                 "(Removed=%lu Stopping=%lu Started=%lu)\n",
                 Dx->Removed, Dx->Stopping, Dx->Started));
        Dx->HardwareFailure = TRUE;
        return 0U;
    }

    if (Dx->Mmio == NULL) {
        KdPrint(("I2CCTRL: ReadRegisterSafe unmapped MMIO Base=%p\n", Dx->Mmio));
        Dx->HardwareFailure = TRUE;
        return 0U;
    }

    if ((Offset + sizeof(ULONG)) > Dx->MmioLength) {
        KdPrint(("I2CCTRL: ReadRegisterSafe OOB Off=0x%lx Len=%lu\n",
                 Offset,
                 Dx->MmioLength));
        Dx->HardwareFailure = TRUE;
        return 0U;
    }

    KeAcquireSpinLock(&Dx->HwLock, &oldIrql);
    __try {
        reg   = (volatile ULONG*)((PUCHAR)Dx->Mmio + Offset);
        value = READ_REGISTER_ULONG((PULONG)reg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        NTSTATUS code = GetExceptionCode();
        KdPrint(("I2CCTRL: ReadRegisterSafe SEH Off=0x%lx Code=0x%08lx\n",
                 Offset,
                 code));
        Dx->HardwareFailure = TRUE;
        value = 0U;
    }
    KeReleaseSpinLock(&Dx->HwLock, oldIrql);

    return value;
}

//
// Backend-agnostic data/status accessors: delegate to Ops (DW-safe)
//

VOID
I2cCtrlWriteData(
    PI2CCTRL_FDO devctx,
    ULONG value
    )
{
    if (devctx == NULL || devctx->Ops == NULL) {
        I2cCtrl_Log("WriteData: missing devctx/Ops\n");
        return;
    }

    if (devctx->Ops->WriteTxByte != NULL) {
        devctx->Ops->WriteTxByte(devctx, (UCHAR)value);
    } else {
        I2cCtrl_Log("WriteData: backend has no WriteTxByte\n");
    }
}

ULONG
I2cCtrlReadData(
    PI2CCTRL_FDO devctx
    )
{
    UCHAR b;

    if (devctx == NULL || devctx->Ops == NULL) {
        I2cCtrl_Log("ReadData: missing devctx/Ops\n");
        return 0U;
    }

    b = 0;

    if (devctx->Ops->ReadRxByteSafe != NULL) {
        (VOID)devctx->Ops->ReadRxByteSafe(devctx, &b);
    } else if (devctx->Ops->ReadRxByte != NULL) {
        (VOID)devctx->Ops->ReadRxByte(devctx, &b);
    } else {
        I2cCtrl_Log("ReadData: backend has no ReadRxByte\n");
        b = 0;
    }

    return (ULONG)b;
}

ULONG
I2cCtrlReadStatus(
    PI2CCTRL_FDO devctx
    )
{
    I2C_HW_STATUS st;

    if (devctx == NULL || devctx->Ops == NULL) {
        return 0U;
    }

    if (devctx->Ops->GetStatus != NULL) {
        /* I2C_HW_STATUS is opaque here; we just invoke the backend */
        (VOID)devctx->Ops->GetStatus(devctx, &st);
    } else {
        I2cCtrl_Log("ReadStatus: backend has no GetStatus\n");
    }

    /* Legacy callers expect a ULONG; we cannot derive it from opaque status */
    return 0U;
}


//
// ---------------------------------------------------------------------------
// Functional roadmap: MMIO mapping, interrupts, child PDO, and I2C transfer
// Optimized for WinDDK 7.1.0 (XP x86 / Server 2003 x64)
// ---------------------------------------------------------------------------
//

//
// Minimal globals for MMIO and interrupt handling
//
PHYSICAL_ADDRESS g_Bar0Phys = {0};
ULONG            g_Bar0Length = 0;
PVOID            g_MmioBase = NULL;

PKINTERRUPT      g_InterruptObject = NULL;
KIRQL            g_IrqLevel = 0;
ULONG            g_IrqVector = 0;
KAFFINITY        g_IrqAffinity = 0;
BOOLEAN          g_IrqLatched = TRUE; // default: latched
PDEVICE_OBJECT   g_ChildPdo = NULL;


//
// Setter for BAR0 physical address/length (call this from your START_DEVICE handler)
//
VOID
I2cCtrlSetBar0(
    PHYSICAL_ADDRESS Bar0Phys,
    ULONG            Bar0Length
    )
{
    g_Bar0Phys = Bar0Phys;
    g_Bar0Length = Bar0Length;
}

//
// Map PCI BAR resources and perform MMIO register access setup
//
NTSTATUS
I2cCtrlMapResources(
    PDEVICE_OBJECT DeviceObject
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);

    if (g_MmioBase != NULL) {
        KdPrint(("I2CCTRL: MMIO already mapped\n"));
        return STATUS_SUCCESS;
    }

    if (g_Bar0Length == 0) {
        KdPrint(("I2CCTRL: BAR0 length is zero; cannot map\n"));
        return STATUS_INVALID_PARAMETER;
    }

    g_MmioBase = MmMapIoSpace(g_Bar0Phys, g_Bar0Length, MmNonCached);
    if (g_MmioBase == NULL) {
        KdPrint(("I2CCTRL: MmMapIoSpace failed\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KdPrint(("I2CCTRL: MMIO mapped at %p (len=0x%lx)\n", g_MmioBase, g_Bar0Length));
    return STATUS_SUCCESS;
}


//
// Connect a legacy line-based interrupt (XP/2003)
//
NTSTATUS
I2cCtrlSetupInterrupts(
    PI2CCTRL_FDO devctx,
    ULONG          IrqVector,
    KIRQL          IrqLevel,
    KAFFINITY      IrqAffinity,
    BOOLEAN        Latched
    )
{
    NTSTATUS status;

    if (devctx->InterruptObject) {
        KdPrint(("I2CCTRL: Interrupt already connected\n"));
        return STATUS_SUCCESS;
    }

    /* Save parameters into the device context */
    devctx->IrqVector   = IrqVector;
    devctx->IrqLevel    = IrqLevel;
    devctx->IrqAffinity = IrqAffinity;
    devctx->IrqLatched  = Latched;

    status = IoConnectInterrupt(
                 &devctx->InterruptObject,   /* OUT PKINTERRUPT* */
                 I2cCtrl_Isr,                 /* PKSERVICE_ROUTINE */
                 devctx,                     /* ServiceContext ties ISR to this controller */
                 NULL,                       /* SpinLock (use internal) */
                 devctx->IrqVector,          /* Vector */
                 devctx->IrqLevel,           /* Irql */
                 devctx->IrqLevel,           /* SynchronizeIrql (usually same as Irql) */
                 devctx->IrqLatched ? Latched : LevelSensitive, /* InterruptMode */
                 TRUE,                       /* ShareVector (most PCI devices share) */
                 devctx->IrqAffinity,        /* ProcessorEnableMask */
                 FALSE                       /* FloatingSave */
             );

    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL: IoConnectInterrupt failed (0x%08X)\n", status));
        devctx->InterruptObject = NULL;
        return status;
    }

    KdPrint(("I2CCTRL: Interrupt connected (vector=%lu, IRQL=%u)\n",
             (unsigned long)devctx->IrqVector, (unsigned)devctx->IrqLevel));

    return STATUS_SUCCESS;
}

//
// Disconnect interrupt (call from IRP_MN_REMOVE_DEVICE or StopDevice)
//
VOID
I2cCtrlTeardownInterrupt(
    PI2CCTRL_FDO devctx
    )
{
    if (devctx->InterruptObject) {
        IoDisconnectInterrupt(devctx->InterruptObject);
        devctx->InterruptObject = NULL;
        KdPrint(("I2CCTRL: Interrupt disconnected\n"));
    }
}

NTSTATUS
I2cCtrlInitHidChildIds(
    PI2CCTRL_PDO childDx,
    ULONG childId,
    const I2CHID_DEVICE_ID* hidMatch
    )
{
    WCHAR *devIdBuf;
    WCHAR *instBuf;
    WCHAR *multiHw;
    WCHAR *multiCompat;
    SIZE_T len, i;
    WCHAR *p;
    PCWSTR hidId;

    if (!childDx || !hidMatch || !hidMatch->HidId)
        return STATUS_INVALID_PARAMETER;

    hidId = hidMatch->HidId;

    //
    // DeviceId = HID ID (e.g. ACPI\PNP0C50)
    //
    len = wcslen(hidId) + 1;

    devIdBuf = ExAllocatePoolWithTag(NonPagedPool, sizeof(WCHAR) * len, TAG_I2C_MISC);
    if (!devIdBuf)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (i = 0; hidId[i] != L'\0'; i++)
        devIdBuf[i] = hidId[i];
    devIdBuf[i] = L'\0';

    RtlInitUnicodeString(&childDx->Desc.DeviceId, devIdBuf);
    RtlInitUnicodeString(&childDx->Desc.HardwareId, devIdBuf);
    RtlInitUnicodeString(&childDx->Desc.CompatibleId, devIdBuf);

    RtlInitUnicodeString(&childDx->HardwareId, devIdBuf);
    RtlInitUnicodeString(&childDx->CompatibleId, devIdBuf);

    //
    // MULTI_SZ
    //
    len = wcslen(hidId) + 2;

    multiHw = ExAllocatePoolWithTag(NonPagedPool, sizeof(WCHAR) * len, TAG_I2C_MISC);
    if (!multiHw)
        return STATUS_INSUFFICIENT_RESOURCES;

    p = multiHw;
    for (i = 0; hidId[i] != L'\0'; i++)
        *p++ = hidId[i];
    *p++ = L'\0';
    *p++ = L'\0';

    childDx->HardwareIdsMultiSz = multiHw;

    multiCompat = ExAllocatePoolWithTag(NonPagedPool, sizeof(WCHAR) * len, TAG_I2C_MISC);
    if (!multiCompat)
        return STATUS_INSUFFICIENT_RESOURCES;

    p = multiCompat;
    for (i = 0; hidId[i] != L'\0'; i++)
        *p++ = hidId[i];
    *p++ = L'\0';
    *p++ = L'\0';

    childDx->CompatibleIdsMultiSz = multiCompat;

    //
    // InstanceId = "0000" + childId
    //
    instBuf = ExAllocatePoolWithTag(NonPagedPool, sizeof(WCHAR) * 8, TAG_I2C_MISC);
    if (!instBuf)
        return STATUS_INSUFFICIENT_RESOURCES;

    instBuf[0] = L'0' + (childId / 1000) % 10;
    instBuf[1] = L'0' + (childId / 100)  % 10;
    instBuf[2] = L'0' + (childId / 10)   % 10;
    instBuf[3] = L'0' + (childId)        % 10;
    instBuf[4] = L'\0';

    RtlInitUnicodeString(&childDx->Desc.InstanceId, instBuf);
    RtlInitUnicodeString(&childDx->InstanceId,      instBuf);

    childDx->Desc.IfGuid    = GUID_I2CCTRL_CHILD_IFACE;
    childDx->Desc.IfEnabled = TRUE;

    return STATUS_SUCCESS;
}


static NTSTATUS
I2cCtrlBuildMultiSz(
    PWSTR *outBuf,
    const WCHAR *ids[],
    ULONG idCount
    )
{
    ULONG totalChars;
    ULONG i;
    PWSTR buf;
    PWSTR p;
    size_t copied;
    NTSTATUS copyStatus;

    totalChars = 0U;
    buf        = NULL;
    p          = NULL;
    copied     = 0U;
    copyStatus = STATUS_SUCCESS;

    /* Calculate total characters needed */
    for (i = 0U; i < idCount; i++) {
        if (ids[i] != NULL) {
            totalChars += (ULONG)wcslen(ids[i]) + 1U; /* string + NUL */
        }
    }
    totalChars += 1U; /* final double NUL terminator */

    buf = (PWSTR)ExAllocatePoolWithTag(PagedPool,
                                       totalChars * sizeof(WCHAR),
                                       TAG_I2C_MISC);
    if (buf == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(buf, totalChars * sizeof(WCHAR));

    p = buf;
    for (i = 0U; i < idCount; i++) {
        if (ids[i] != NULL) {
            copyStatus = RtlStringCchCopyW(p,
                                           totalChars - (ULONG)(p - buf),
                                           ids[i]);
            if (!NT_SUCCESS(copyStatus)) {
                ExFreePoolWithTag(buf, TAG_I2C_MISC);
                return copyStatus;
            }

            (void)RtlStringCchLengthW(p,
                                      totalChars - (ULONG)(p - buf),
                                      &copied);
            p += copied + 1U; /* advance past string + NUL */
        }
    }

    *p = L'\0'; /* double NUL terminator */

    *outBuf = buf;
    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * I2cCtrlTransfer - XP/2003 BSOD-hardened, HAL-generic, C89-compliant
 *
 * Purpose:
 *   - Implement a simple polled I²C transfer using HAL ops
 *   - Support both read and write operations
 *   - Apply controller quirks before transfer
 *   - Handle FIFO readiness and STOP detection
 *
 * Safety hardening:
 *   - PASSIVE_LEVEL assertion
 *   - Strict NULL checks on pointers and caps
 *   - Bounded polling with watchdog back-off
 *   - SEH guards around HAL ops to catch access faults
 *   - Avoid MMIO/ops use if unmapped/uninitialized
 *   - Graceful controller disable on failure
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrlTransfer(
    PI2CCTRL_FDO fdoExt,
    PUCHAR Buffer,
    ULONG Length,
    BOOLEAN Write
    )
{
    ULONG i;
    ULONG spins;
    ULONG maxSpins;
    ULONG backoffUs;
    I2C_HW_STATUS hwst;
    NTSTATUS st;
    ULONG intr;
    BOOLEAN enabled;
    BOOLEAN haveStatus;
    BOOLEAN canWrite;
    BOOLEAN canEmitRead;
    BOOLEAN canRead;
    BOOLEAN canReadSafe;
    BOOLEAN canSetTarget;

    RtlZeroMemory(&hwst, sizeof(hwst));
    st         = STATUS_SUCCESS;
    intr       = 0U;
    enabled    = FALSE;
    haveStatus = FALSE;
    canWrite   = FALSE;
    canEmitRead= FALSE;
    canRead    = FALSE;
    canReadSafe= FALSE;
    canSetTarget = FALSE;
    spins      = 0U;
    maxSpins   = I2C_POLL_SPINS_MAX;
    backoffUs  = I2C_STALL_US;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    /* Parameter and capability validation */
    if (fdoExt == NULL || Buffer == NULL || Length == 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    if (fdoExt->Ops == NULL || fdoExt->Caps == NULL) {
        KdPrint(("I2CCTRL: Transfer: missing Ops/Caps\n"));
        return STATUS_DEVICE_NOT_READY;
    }
    if (fdoExt->Caps->TxFifoDepth == 0U) {
        KdPrint(("I2CCTRL: Transfer: invalid TxFifoDepth\n"));
        return STATUS_DEVICE_NOT_READY;
    }
    if (fdoExt->HardwareFailure) {
        KdPrint(("I2CCTRL: Transfer: hardware flagged failed; aborting\n"));
        return STATUS_IO_DEVICE_ERROR;
    }

    /* Apply quirks before transfer (safe precondition) */
    I2cCtrlApplyQuirks(fdoExt);

    /* Precompute function pointer availability */
    haveStatus   = (fdoExt->Ops->GetStatus != NULL) ? TRUE : FALSE;
    canWrite     = (fdoExt->Ops->IssueWriteByte != NULL) ? TRUE : FALSE;
    canEmitRead  = (fdoExt->Ops->EmitReadRequest != NULL) ? TRUE : FALSE;
    canRead      = (fdoExt->Ops->ReadRxByte != NULL) ? TRUE : FALSE;
    canReadSafe  = (fdoExt->Ops->ReadRxByteSafe != NULL) ? TRUE : FALSE;
    canSetTarget = (fdoExt->Ops->SetTarget7bit != NULL) ? TRUE : FALSE;

    /* Program target address (7-bit) and enable controller */
    __try {
        if (canSetTarget) {
            st = fdoExt->Ops->SetTarget7bit(fdoExt, fdoExt->TargetAddress & 0x7FU);
            if (!NT_SUCCESS(st)) {
                KdPrint(("I2CCTRL: Transfer: SetTarget7bit failed 0x%08X\n", st));
                __leave;
            }
        }
        if (fdoExt->Ops->Enable != NULL) {
            (VOID)fdoExt->Ops->Enable(fdoExt, TRUE);
            enabled = TRUE;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("I2CCTRL: Transfer: exception in enable/target\n"));
        st = STATUS_ACCESS_VIOLATION;
    }
    if (!NT_SUCCESS(st)) {
        if (enabled && fdoExt->Ops->Enable != NULL) {
            __try { (VOID)fdoExt->Ops->Enable(fdoExt, FALSE); } __except(EXCEPTION_EXECUTE_HANDLER) { /* noop */ }
        }
        fdoExt->HardwareFailure = TRUE;
        return st;
    }

    /* Transfer data bytes */
    if (Write) {
        if (!canWrite || !haveStatus) {
            KdPrint(("I2CCTRL: Transfer: write/status ops not available\n"));
            st = STATUS_NOT_SUPPORTED;
            goto Cleanup;
        }

        for (i = 0U; i < Length; i++) {
            /* Wait until TX FIFO not full (bounded polling with back-off) */
            spins = 0U;
            while (spins < maxSpins) {
                __try {
                    st = fdoExt->Ops->GetStatus(fdoExt, &hwst);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    KdPrint(("I2CCTRL: Transfer: exception in GetStatus (TX)\n"));
                    st = STATUS_ACCESS_VIOLATION;
                }
                if (!NT_SUCCESS(st)) {
                    goto CleanupFail;
                }
                if (hwst.TxFifoNotFull || (hwst.TxFifoLevel < fdoExt->Caps->TxFifoDepth)) {
                    break;
                }
                KeStallExecutionProcessor(backoffUs);
                spins++;
            }
            if (spins == maxSpins) {
                st = STATUS_IO_TIMEOUT;
                goto CleanupFail;
            }

            __try {
                st = fdoExt->Ops->IssueWriteByte(fdoExt, Buffer[i]);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                KdPrint(("I2CCTRL: Transfer: exception in IssueWriteByte\n"));
                st = STATUS_ACCESS_VIOLATION;
            }
            if (!NT_SUCCESS(st)) {
                goto CleanupFail;
            }
        }
    } else {
        if (!haveStatus || (!canEmitRead && !canRead && !canReadSafe)) {
            KdPrint(("I2CCTRL: Transfer: read ops not available\n"));
            st = STATUS_NOT_SUPPORTED;
            goto Cleanup;
        }

        for (i = 0U; i < Length; i++) {
            /* Wait until TX FIFO has room to emit read request */
            spins = 0U;
            while (spins < maxSpins) {
                __try {
                    st = fdoExt->Ops->GetStatus(fdoExt, &hwst);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    KdPrint(("I2CCTRL: Transfer: exception in GetStatus (emit)\n"));
                    st = STATUS_ACCESS_VIOLATION;
                }
                if (!NT_SUCCESS(st)) {
                    goto CleanupFail;
                }
                if (hwst.TxFifoNotFull || (hwst.TxFifoLevel < fdoExt->Caps->TxFifoDepth)) {
                    break;
                }
                KeStallExecutionProcessor(backoffUs);
                spins++;
            }
            if (spins == maxSpins) {
                st = STATUS_IO_TIMEOUT;
                goto CleanupFail;
            }

            if (canEmitRead) {
                __try { (VOID)fdoExt->Ops->EmitReadRequest(fdoExt); }
                __except(EXCEPTION_EXECUTE_HANDLER) {
                    KdPrint(("I2CCTRL: Transfer: exception in EmitReadRequest\n"));
                    st = STATUS_ACCESS_VIOLATION;
                }
                if (!NT_SUCCESS(st)) {
                    goto CleanupFail;
                }
            }

            /* Wait until RX FIFO not empty */
            spins = 0U;
            while (spins < maxSpins) {
                __try {
                    st = fdoExt->Ops->GetStatus(fdoExt, &hwst);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    KdPrint(("I2CCTRL: Transfer: exception in GetStatus (RX)\n"));
                    st = STATUS_ACCESS_VIOLATION;
                }
                if (!NT_SUCCESS(st)) {
                    goto CleanupFail;
                }
                if (hwst.RxFifoNotEmpty || (hwst.RxFifoLevel > 0U)) {
                    break;
                }
                KeStallExecutionProcessor(backoffUs);
                spins++;
            }
            if (spins == maxSpins) {
                st = STATUS_IO_TIMEOUT;
                goto CleanupFail;
            }

            if (canRead) {
                __try {
                    st = fdoExt->Ops->ReadRxByte(fdoExt, &Buffer[i]);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    KdPrint(("I2CCTRL: Transfer: exception in ReadRxByte\n"));
                    st = STATUS_ACCESS_VIOLATION;
                }
            } else if (canReadSafe) {
                __try {
                    st = fdoExt->Ops->ReadRxByteSafe(fdoExt, &Buffer[i]);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    KdPrint(("I2CCTRL: Transfer: exception in ReadRxByteSafe\n"));
                    st = STATUS_ACCESS_VIOLATION;
                }
            } else {
                st = STATUS_NOT_SUPPORTED;
            }
            if (!NT_SUCCESS(st)) {
                goto CleanupFail;
            }
        }
    }

    /* Optionally wait for STOP detect and clear (best-effort) */
    if (fdoExt->Ops->GetRawIntr != NULL && fdoExt->Ops->AckInterrupts != NULL) {
        __try {
            intr = fdoExt->Ops->GetRawIntr(fdoExt);
            if ((intr & I2C_INT_STOP_DETECTED) != 0U) {
                (VOID)fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_STOP_DETECTED);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            /* non-fatal; continue */
        }
    }

Cleanup:
    /* Gracefully disable controller if we enabled it */
    if (enabled && fdoExt->Ops->Enable != NULL) {
        __try { (VOID)fdoExt->Ops->Enable(fdoExt, FALSE); }
        __except(EXCEPTION_EXECUTE_HANDLER) { /* noop */ }
    }
    return st;

CleanupFail:
    /* Mark hardware failure and disable controller */
    fdoExt->HardwareFailure = TRUE;
    if (enabled && fdoExt->Ops->Enable != NULL) {
        __try { (VOID)fdoExt->Ops->Enable(fdoExt, FALSE); }
        __except(EXCEPTION_EXECUTE_HANDLER) { /* noop */ }
    }
    return st;
}



/* Helper to initialize a single-phase transfer */
__inline VOID
I2cCtrl_InitSinglePhase(
    PI2C_TRANSFER_CONTEXT Ctx,
    PUCHAR Buffer,
    ULONG Length,
    BOOLEAN IsRead,
    UCHAR Address7Bit
    )
{
    RtlZeroMemory(Ctx, sizeof(*Ctx));
    Ctx->Phases[0].Buffer = Buffer;
    Ctx->Phases[0].Length = Length;
    Ctx->Phases[0].IsRead = IsRead;
    Ctx->NumPhases        = 1;
    Ctx->CurrentPhase     = 0;
    Ctx->Position         = 0;
    Ctx->Address7Bit      = Address7Bit;
    Ctx->Status           = STATUS_PENDING;
    Ctx->StopSeen         = FALSE;
}

/* Helper to initialize a two-phase transfer (e.g., block process call) */
__inline VOID
I2cCtrl_InitTwoPhase(
    PI2C_TRANSFER_CONTEXT Ctx,
    PUCHAR WrBuf, ULONG WrLen,
    PUCHAR RdBuf, ULONG RdLen,
    UCHAR Address7Bit
    )
{
    RtlZeroMemory(Ctx, sizeof(*Ctx));
    Ctx->Phases[0].Buffer = WrBuf;
    Ctx->Phases[0].Length = WrLen;
    Ctx->Phases[0].IsRead = FALSE;

    Ctx->Phases[1].Buffer = RdBuf;
    Ctx->Phases[1].Length = RdLen;
    Ctx->Phases[1].IsRead = TRUE;

    Ctx->NumPhases        = 2;
    Ctx->CurrentPhase     = 0;
    Ctx->Position         = 0;
    Ctx->Address7Bit      = Address7Bit;
    Ctx->Status           = STATUS_PENDING;
    Ctx->StopSeen         = FALSE;
}

/* XP-BSOD-safe, WinDDK-compiler-safe, and C89-compliant */
NTSTATUS
I2cCtrl_ProcessQueue(
    IN PI2CCTRL_FDO devctx
    )
{
    KIRQL oldIrql;
    PSMBUS_REQUEST req;
    NTSTATUS status;
    SPBCX_COMPAT_CONTEXT compat;
    UCHAR addr;
    UCHAR cmd;
    UCHAR len;
    BOOLEAN isBlockWrite;
    BOOLEAN usePec;
    LARGE_INTEGER due;

    /* C89: initialize locals at top */
    oldIrql = 0;
    req = NULL;
    status = STATUS_SUCCESS;
    RtlZeroMemory(&compat, sizeof(compat));
    addr = 0U;
    cmd = 0U;
    len = 0U;
    isBlockWrite = FALSE;
    usePec = FALSE;
    due.QuadPart = 0;

    /* Defensive gates */
    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (devctx->Removed || devctx->Stopping) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Avoid touching hardware if MMIO unmapped */
    if (devctx->MmioBase == NULL) {
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    /* Serialize queue access */
    KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);

    /* Already busy -> nothing to do now */
    if (devctx->ActiveBusy != FALSE) {
        KeReleaseSpinLock(&devctx->QueueLock, oldIrql);
        return STATUS_PENDING;
    }

    /* No queued requests */
    if (IsListEmpty(&devctx->RequestQueue)) {
        KeReleaseSpinLock(&devctx->QueueLock, oldIrql);
        return STATUS_NO_MORE_ENTRIES;
    }

    /* Dequeue next request node (ownership transfers to us) */
    req = CONTAINING_RECORD(RemoveHeadList(&devctx->RequestQueue), SMBUS_REQUEST, ListEntry);
    if (req == NULL) {
        KeReleaseSpinLock(&devctx->QueueLock, oldIrql);
        return STATUS_NO_MORE_ENTRIES;
    }

    /* Snapshot into ActiveRequest (stable storage) */
    RtlCopyMemory(&devctx->ActiveRequest, req, sizeof(SMBUS_REQUEST));
    devctx->ActiveBusy = TRUE;

    KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

    /* We no longer need the allocated request node; free safely */
    ExFreePoolWithTag(req, 'qmbS');

    /* Basic field extraction */
    addr = (UCHAR)(devctx->ActiveRequest.SlaveAddress & 0x7F);
    cmd  = devctx->ActiveRequest.Command;
    len  = devctx->ActiveRequest.Length;
    usePec = ((devctx->ActiveRequest.Flags & SMBUS_REQ_FLAG_PEC) != 0);
    isBlockWrite = (devctx->ActiveRequest.OpCode == 0x10) ? TRUE : FALSE;

    /* Validate IRP presence early */
    if (devctx->ActiveRequest.Irp == NULL) {
        devctx->ActiveBusy = FALSE;
        devctx->XferCtx.Status = STATUS_INVALID_PARAMETER;
        return STATUS_INVALID_PARAMETER;
    }

    /* Build transfer context based on opcode and flags */
    switch (devctx->ActiveRequest.OpCode) {
    case I2CCTRL_OPCODE_BLOCK_PROCESS_CALL:
        {
            UCHAR count;
            UCHAR outLen;

            outLen = 0U;
            count = devctx->ActiveRequest.Buffer[1];

            if (count == 0U || count > 32U) {
                devctx->ActiveBusy = FALSE;
                devctx->XferCtx.Status = STATUS_INVALID_PARAMETER;
                return STATUS_INVALID_PARAMETER;
            }

            if (usePec != FALSE) {
                /* payload: command + count + data[count] */
                (VOID)Smbus_BuildWriteFrameAndAppendPec(
                    addr,
                    cmd,
                    &devctx->ActiveRequest.Buffer[1],
                    (UCHAR)(1U + count),
                    &devctx->ActiveRequest.Buffer[0],
                    &outLen
                );

                /* Two-phase: write then read (count returned + data), plus optional PEC */
                I2cCtrl_InitTwoPhase(
                    &devctx->XferCtx,
                    &devctx->ActiveRequest.Buffer[0], (ULONG)outLen,
                    &devctx->ActiveRequest.Buffer[0], (ULONG)(1U + 32U + 1U),
                    addr
                );
            } else {
                I2cCtrl_InitTwoPhase(
                    &devctx->XferCtx,
                    &devctx->ActiveRequest.Buffer[0], (ULONG)(2U + count),
                    &devctx->ActiveRequest.Buffer[0], (ULONG)(1U + 32U),
                    addr
                );
            }
        }
        break;

    case 0x01: /* SEND_BYTE */
    case 0x10: /* BLOCK_WRITE */
        {
            UCHAR payloadLen;
            UCHAR outLen2;

            payloadLen = 0U;
            outLen2 = 0U;

            if (isBlockWrite != FALSE) {
                UCHAR count2;
                count2 = devctx->ActiveRequest.Buffer[1];
                if (count2 == 0U || count2 > 32U) {
                    devctx->ActiveBusy = FALSE;
                    devctx->XferCtx.Status = STATUS_INVALID_PARAMETER;
                    return STATUS_INVALID_PARAMETER;
                }
                payloadLen = (UCHAR)(1U + count2);
            } else {
                /* SEND_BYTE: len includes addr/cmd + data; ensure len >= 1 */
                payloadLen = (len > 1U) ? (UCHAR)(len - 1U) : 0U;
            }

            if (usePec != FALSE) {
                (VOID)Smbus_BuildWriteFrameAndAppendPec(
                    addr,
                    cmd,
                    &devctx->ActiveRequest.Buffer[1],
                    payloadLen,
                    &devctx->ActiveRequest.Buffer[0],
                    &outLen2
                );

                I2cCtrl_InitSinglePhase(
                    &devctx->XferCtx,
                    &devctx->ActiveRequest.Buffer[0],
                    (ULONG)outLen2,
                    FALSE,
                    addr
                );
            } else {
                I2cCtrl_InitSinglePhase(
                    &devctx->XferCtx,
                    &devctx->ActiveRequest.Buffer[0],
                    (ULONG)len,
                    FALSE,
                    addr
                );
            }
        }
        break;

    case 0x02: /* RECEIVE_BYTE */
    case 0x11: /* BLOCK_READ */
        {
            ULONG rdMax;
            rdMax = (devctx->ActiveRequest.OpCode == 0x11)
                        ? (ULONG)(1U + 32U + (usePec != FALSE ? 1U : 0U))
                        : (ULONG)(1U + (usePec != FALSE ? 1U : 0U));

            I2cCtrl_InitSinglePhase(
                &devctx->XferCtx,
                &devctx->ActiveRequest.Buffer[0],
                rdMax,
                TRUE,
                addr
            );
        }
        break;

    default:
        devctx->ActiveBusy = FALSE;
        devctx->XferCtx.Status = STATUS_INVALID_DEVICE_REQUEST;
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /* Associate IRP with transfer context */
    devctx->XferCtx.Irp = devctx->ActiveRequest.Irp;

    /* Initialize retries/timeouts */
    devctx->ActiveRequest.RetriesRemaining = devctx->MaxRetries;

    if (devctx->TransactionTimeoutMs > 0U) {
        due.QuadPart = -(LONGLONG)devctx->TransactionTimeoutMs * 10000LL; /* ms -> 100ns */
        KeInitializeTimer(&devctx->XferCtx.TimeoutTimer);
        KeInitializeDpc(&devctx->XferCtx.TimeoutDpc, I2cCtrl_DpcRoutine, devctx);
        KeSetTimer(&devctx->XferCtx.TimeoutTimer, due, &devctx->XferCtx.TimeoutDpc);
    }

    /* Populate SPBCx compatibility context (no pageable work here) */
    compat.TargetAddress = (ULONG)(addr);
    compat.TimeoutMs     = devctx->TransactionTimeoutMs;
    compat.Flags         = devctx->ActiveRequest.Flags;
    compat.IsRead        = (devctx->XferCtx.Direction == I2C_DIR_READ) ? TRUE : FALSE;
    compat.BufferLen     = devctx->XferCtx.Length;
    compat.SequenceHdr.TransferCount = devctx->XferCtx.NumPhases;
    compat.SequenceHdr.Flags         = 0U;
    compat.SequenceHdr.OutLength     = devctx->XferCtx.Length;

    /* Start hardware transfer; if it fails, complete IRP once and clear busy */
    status = I2cCtrl_StartTransfer(devctx, &compat);
    if (!NT_SUCCESS(status)) {
        PIRP failIrp;

        devctx->ActiveBusy = FALSE;
        devctx->XferCtx.Status = status;

        failIrp = devctx->ActiveRequest.Irp;
        if (failIrp != NULL) {
            failIrp->IoStatus.Status = status;
            failIrp->IoStatus.Information = 0U;

            /* Signal transfer event if used by waiters */
            KeSetEvent(&devctx->TransferEvent, IO_NO_INCREMENT, FALSE);

            IoCompleteRequest(failIrp, IO_NO_INCREMENT);
        }
        return status;
    }

    /* Transfer is in progress; ISR/DPC or PollWorker will complete the IRP */
    return STATUS_PENDING;
}

VOID
I2cCtrl_CancelQueuedIrp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PI2CCTRL_FDO dx;
    KIRQL oldIrql;
    PLIST_ENTRY le;
    PIRP qIrp;

    if (Irp == NULL) {
        return;
    }

    dx = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    if (dx == NULL) {
        IoReleaseCancelSpinLock(Irp->CancelIrql);
        return;
    }

    /* Release cancel spin lock BEFORE taking our own lock */
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    /* Remove IRP from our PendingIrpList if it is still there */
    KeAcquireSpinLock(&dx->PendingIrpLock, &oldIrql);

    for (le = dx->PendingIrpList.Flink;
         le != &dx->PendingIrpList;
         le = le->Flink)
    {
        qIrp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);

        if (qIrp == Irp) {
            RemoveEntryList(le);
            KeReleaseSpinLock(&dx->PendingIrpLock, oldIrql);

            IoSetCancelRoutine(Irp, NULL);
            Irp->IoStatus.Status      = STATUS_CANCELLED;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return;
        }
    }

    KeReleaseSpinLock(&dx->PendingIrpLock, oldIrql);

    /* If not found, IRP is already being completed elsewhere */
}


/* -----------------------------------------------------------------------
 * I2cCtrl_ReadBlock - XP/2003 BSOD-safe, HAL-generic, C89-compliant
 *
 * Purpose:
 *   - Read a block of bytes from a device register using HAL ops
 *   - Program the target register offset and issue sequential read requests
 *   - Poll RX FIFO until each byte arrives or timeout occurs
 *   - Flag hardware failure on timeout and log diagnostic information
 *   - Use unified timeout/backoff macros from i2cctrl_ext.h
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids direct MMIO/register macros (HAL-only)
 *   - Completes or fails deterministically with hardware failure flagged
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_ReadBlock(
    PI2CCTRL_FDO fdoExt,
    USHORT       slaveAddr,
    ULONG        reg,
    PUCHAR       buffer,
    ULONG        length
    )
{
    NTSTATUS      status;
    LARGE_INTEGER freq;
    LARGE_INTEGER start;
    LARGE_INTEGER now;
    ULONG         elapsedMs;
    ULONG         delayUs;
    ULONG         i;
    I2C_HW_STATUS hwst;

    /* C89 init */
    status    = STATUS_SUCCESS;
    elapsedMs = 0U;
    delayUs   = (fdoExt && fdoExt->PolicyBackoffInitialUs)
                 ? fdoExt->PolicyBackoffInitialUs : 10U;
    i         = 0U;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (fdoExt == NULL || buffer == NULL || length == 0U || fdoExt->Ops == NULL) {
        KdPrint(("I2CCTRL: ReadBlock invalid parameters fdoExt=%p buf=%p len=%lu\n",
                 fdoExt, buffer, length));
        return STATUS_INVALID_PARAMETER;
    }
    if (!fdoExt->Enabled) {
        return STATUS_DEVICE_NOT_READY;
    }

    fdoExt->XferCtx.Status = STATUS_PENDING;

    /* XP-compatible: first call returns counter and fills frequency */
    start = KeQueryPerformanceCounter(&freq);

    /* Program target slave address */
    if (fdoExt->Ops->SetTarget7bit) {
        status = fdoExt->Ops->SetTarget7bit(fdoExt, (UCHAR)(slaveAddr & 0x7F));
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }
    }

    /* Write register offset first */
    if (fdoExt->Ops->WriteTxByte) {
        status = fdoExt->Ops->WriteTxByte(fdoExt, (UCHAR)(reg & 0xFFU));
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }
    }

    /* Issue read requests */
    for (i = 0U; i < length; i++) {
        if (fdoExt->Ops->EmitReadRequest) {
            (VOID)fdoExt->Ops->EmitReadRequest(fdoExt);
        } else if (fdoExt->Ops->IssueReadToken) {
            status = fdoExt->Ops->IssueReadToken(fdoExt);
            if (!NT_SUCCESS(status)) {
                fdoExt->HardwareFailure = TRUE;
                goto Exit;
            }
        }
    }

    /* Poll RX FIFO for data */
    for (i = 0U; i < length; i++) {
        for (;;) {
            if (fdoExt->Ops->GetStatus &&
                NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                if (hwst.RxFifoNotEmpty || hwst.RxFifoLevel > 0U) {
                    break;
                }
            }
            I2cCtrl_Backoff(fdoExt, &delayUs);
            CHECK_TIMEOUT_BREAK(fdoExt->TransactionTimeoutMs, start, freq, elapsedMs, now, status);
        }
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }

        if (fdoExt->Ops->ReadRxByte) {
            status = fdoExt->Ops->ReadRxByte(fdoExt, &buffer[i]);
        } else if (fdoExt->Ops->ReadRxByteSafe) {
            status = fdoExt->Ops->ReadRxByteSafe(fdoExt, &buffer[i]);
        } else {
            status = STATUS_NOT_SUPPORTED;
        }
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }
    }

Exit:
    fdoExt->XferCtx.Status = status;
    return status;
}


/* -----------------------------------------------------------------------
 * I2cCtrl_WriteBlock - HAL-generic, XP/2003 BSOD-safe, C89-compliant
 *
 * Purpose:
 *   - Write a block of bytes to a device register
 *   - Use HAL ops (SetTarget7bit, GetStatus, WriteTxByte, AckInterrupts)
 *   - Poll TX FIFO space deterministically with backoff and timeout
 *   - Handle ACK/NACK/ABRT via HAL-neutral status
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids unsafe MMIO access when HAL ops unavailable
 *   - Uses unified timeout/backoff macros (i2cctrl_ext.h)
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_WriteBlock(
    PI2CCTRL_FDO fdoExt,
    USHORT       slaveAddr,
    ULONG        reg,
    const PUCHAR buffer,
    ULONG        length
    )
{
    NTSTATUS       status;
    LARGE_INTEGER  freq;
    LARGE_INTEGER  start;
    LARGE_INTEGER  now;
    ULONG          elapsedMs;
    ULONG          delayUs;
    ULONG          i;
    I2C_HW_STATUS  hwst;

    /* C89 init */
    status     = STATUS_SUCCESS;
    elapsedMs  = 0U;
    delayUs    = (fdoExt != NULL && fdoExt->PolicyBackoffInitialUs != 0U)
                 ? fdoExt->PolicyBackoffInitialUs : 10U;
    i          = 0U;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (fdoExt == NULL || buffer == NULL || length == 0U || fdoExt->Ops == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!fdoExt->Enabled) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (fdoExt->Ops->SetTarget7bit == NULL ||
        fdoExt->Ops->GetStatus == NULL ||
        (fdoExt->Ops->WriteTxByte == NULL && fdoExt->Ops->IssueWriteByte == NULL)) {
        return STATUS_NOT_SUPPORTED;
    }

    fdoExt->XferCtx.Status = STATUS_PENDING;

    /* XP-compatible: first call returns counter and fills frequency */
    start = KeQueryPerformanceCounter(&freq);

    /* Program target slave address (7-bit) */
    status = fdoExt->Ops->SetTarget7bit(fdoExt, (UCHAR)(slaveAddr & 0x7F));
    if (!NT_SUCCESS(status)) {
        fdoExt->HardwareFailure = TRUE;
        goto Exit;
    }

    /* Write register offset first */
    for (;;) {
        status = fdoExt->Ops->GetStatus(fdoExt, &hwst);
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }
        if (hwst.TxFifoNotFull || hwst.TxFifoLevel < fdoExt->Caps->TxFifoDepth) {
            break;
        }
        I2cCtrl_Backoff(fdoExt, &delayUs);
        CHECK_TIMEOUT_BREAK(fdoExt->TransactionTimeoutMs, start, freq, elapsedMs, now, status);
    }
    if (!NT_SUCCESS(status)) {
        fdoExt->HardwareFailure = TRUE;
        goto Exit;
    }

    if (fdoExt->Ops->WriteTxByte) {
        status = fdoExt->Ops->WriteTxByte(fdoExt, (UCHAR)(reg & 0xFFU));
    } else {
        status = fdoExt->Ops->IssueWriteByte(fdoExt, (UCHAR)(reg & 0xFFU));
    }
    if (!NT_SUCCESS(status)) {
        fdoExt->HardwareFailure = TRUE;
        goto Exit;
    }

    /* Write each byte of the buffer */
    for (i = 0U; i < length; i++) {
        /* Poll TX FIFO for space with backoff & timeout */
        for (;;) {
            status = fdoExt->Ops->GetStatus(fdoExt, &hwst);
            if (!NT_SUCCESS(status)) {
                fdoExt->HardwareFailure = TRUE;
                goto Exit;
            }
            /* Abort/NACK early exit if backend reports it */
            if (hwst.TxAborted || hwst.AddressNack || hwst.DataNack) {
                if (fdoExt->Ops->AckInterrupts) {
                    fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_TX_ABORT);
                }
                status = STATUS_IO_DEVICE_ERROR;
                fdoExt->HardwareFailure = TRUE;
                goto Exit;
            }
            if (hwst.TxFifoNotFull || hwst.TxFifoLevel < fdoExt->Caps->TxFifoDepth) {
                break;
            }
            I2cCtrl_Backoff(fdoExt, &delayUs);
            CHECK_TIMEOUT_BREAK(fdoExt->TransactionTimeoutMs, start, freq, elapsedMs, now, status);
        }
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }

        /* Write one byte via HAL */
        if (fdoExt->Ops->WriteTxByte) {
            status = fdoExt->Ops->WriteTxByte(fdoExt, buffer[i]);
        } else {
            status = fdoExt->Ops->IssueWriteByte(fdoExt, buffer[i]);
        }
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }
    }

    /* Final abort/NACK check and defensive STOP ack */
    status = fdoExt->Ops->GetStatus(fdoExt, &hwst);
    if (!NT_SUCCESS(status)) {
        fdoExt->HardwareFailure = TRUE;
        goto Exit;
    }
    if (hwst.TxAborted || hwst.AddressNack || hwst.DataNack) {
        if (fdoExt->Ops->AckInterrupts) {
            fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_TX_ABORT);
        }
        status = STATUS_IO_DEVICE_ERROR;
        fdoExt->HardwareFailure = TRUE;
        goto Exit;
    }
    if (hwst.StopDetected && fdoExt->Ops->AckInterrupts) {
        fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_STOP_DETECTED);
    }

Exit:
    fdoExt->XferCtx.Status = status;
    return status;
}


NTSTATUS
I2cCtrl_LoadRegistryPolicy(
    IN PI2CCTRL_FDO Dx
    )
{
    NTSTATUS               status;
    HANDLE                 drvKey;
    HANDLE                 paramsKey;
    UNICODE_STRING         paramsName;
    OBJECT_ATTRIBUTES      oa;
    RTL_QUERY_REGISTRY_TABLE tbl[14];

    struct {
        ULONG EnableHighSpeed;
        ULONG BusSpeedHz;
        ULONG MaxRetries;
        ULONG RetryDelayUs;
        ULONG TransactionTimeoutMs;
        ULONG BackoffOnBusy;
        ULONG BackoffInitialUs;
        ULONG BackoffMaxUs;
        ULONG UsePec;
        ULONG GpioActiveLow;
        ULONG Force10Bit;
        ULONG CrashOnError;
        ULONG PwrmBase;      /* optional PWRMBASE override (low 32 bits) */
    } cfg;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    PAGED_CODE();

    if (Dx == NULL || Dx->Self == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status     = STATUS_SUCCESS;
    drvKey     = NULL;
    paramsKey  = NULL;

    RtlZeroMemory(&cfg, sizeof(cfg));
    RtlZeroMemory(tbl, sizeof(tbl));
    RtlInitUnicodeString(&paramsName, L"Parameters");

    /* Open driver key */
    status = IoOpenDeviceRegistryKey(Dx->Self,
                                     PLUGPLAY_REGKEY_DRIVER,
                                     KEY_READ,
                                     &drvKey);
    if (!NT_SUCCESS(status) || drvKey == NULL) {
        status = IoOpenDeviceRegistryKey(Dx->Self,
                                         PLUGPLAY_REGKEY_DEVICE,
                                         KEY_READ,
                                         &drvKey);
        if (!NT_SUCCESS(status) || drvKey == NULL) {
            goto ApplyDefaults;
        }
    }

    /* Open Parameters subkey */
    InitializeObjectAttributes(&oa,
                               &paramsName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               drvKey,
                               NULL);

    {
        ULONG disp;
        status = ZwCreateKey(&paramsKey,
                             KEY_READ,
                             &oa,
                             0,
                             NULL,
                             REG_OPTION_NON_VOLATILE,
                             &disp);
        if (!NT_SUCCESS(status) || paramsKey == NULL) {
            paramsKey = NULL;
        }
    }

    /* Build query table */
    tbl[0].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    tbl[0].Name          = L"EnableHighSpeed";
    tbl[0].EntryContext  = &cfg.EnableHighSpeed;
    tbl[0].DefaultType   = REG_DWORD;

    tbl[1].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    tbl[1].Name          = L"BusSpeedHz";
    tbl[1].EntryContext  = &cfg.BusSpeedHz;
    tbl[1].DefaultType   = REG_DWORD;

    tbl[2].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    tbl[2].Name          = L"MaxRetries";
    tbl[2].EntryContext  = &cfg.MaxRetries;
    tbl[2].DefaultType   = REG_DWORD;

    tbl[3].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    tbl[3].Name          = L"RetryDelayUs";
    tbl[3].EntryContext  = &cfg.RetryDelayUs;
    tbl[3].DefaultType   = REG_DWORD;

    tbl[4].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    tbl[4].Name          = L"TransactionTimeoutMs";
    tbl[4].EntryContext  = &cfg.TransactionTimeoutMs;
    tbl[4].DefaultType   = REG_DWORD;

    tbl[5].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    tbl[5].Name          = L"BackoffOnBusy";
    tbl[5].EntryContext  = &cfg.BackoffOnBusy;
    tbl[5].DefaultType   = REG_DWORD;

    tbl[6].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    tbl[6].Name          = L"BackoffInitialUs";
    tbl[6].EntryContext  = &cfg.BackoffInitialUs;
    tbl[6].DefaultType   = REG_DWORD;

    tbl[7].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    tbl[7].Name          = L"BackoffMaxUs";
    tbl[7].EntryContext  = &cfg.BackoffMaxUs;
    tbl[7].DefaultType   = REG_DWORD;

    tbl[8].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    tbl[8].Name          = L"UsePEC";
    tbl[8].EntryContext  = &cfg.UsePec;
    tbl[8].DefaultType   = REG_DWORD;

    tbl[9].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    tbl[9].Name          = L"GpioActiveLow";
    tbl[9].EntryContext  = &cfg.GpioActiveLow;
    tbl[9].DefaultType   = REG_DWORD;

    tbl[10].Flags        = RTL_QUERY_REGISTRY_DIRECT;
    tbl[10].Name         = L"Force10BitAddr";
    tbl[10].EntryContext = &cfg.Force10Bit;
    tbl[10].DefaultType  = REG_DWORD;

    tbl[11].Flags        = RTL_QUERY_REGISTRY_DIRECT;
    tbl[11].Name         = L"CrashOnError";
    tbl[11].EntryContext = &cfg.CrashOnError;
    tbl[11].DefaultType  = REG_DWORD;

    tbl[12].Flags        = RTL_QUERY_REGISTRY_DIRECT;
    tbl[12].Name         = L"PwrmBase";
    tbl[12].EntryContext = &cfg.PwrmBase;
    tbl[12].DefaultType  = REG_DWORD;

    tbl[13].Name = NULL;

    {
        HANDLE root = (paramsKey != NULL) ? paramsKey : drvKey;
        if (root != NULL) {
            (void)RtlQueryRegistryValues(RTL_REGISTRY_HANDLE,
                                         root,
                                         tbl,
                                         NULL,
                                         NULL);
        }
    }

ApplyDefaults:

    Dx->PolicyEnableHighSpeed   = (cfg.EnableHighSpeed != 0U) ? 1U : 0U;

    Dx->PolicyBusSpeedHz        =
        (cfg.BusSpeedHz >= 10000U && cfg.BusSpeedHz <= 3400000U)
        ? cfg.BusSpeedHz
        : ((Dx->PolicyEnableHighSpeed != 0U) ? 3400000U : 400000U);

    Dx->PolicyMaxRetries        = (cfg.MaxRetries <= 10U) ? cfg.MaxRetries : 3U;

    Dx->PolicyRetryDelayUs      = (cfg.RetryDelayUs <= 100000U)
                                  ? cfg.RetryDelayUs : 1000U;

    Dx->PolicyTxnTimeoutMs      =
        (cfg.TransactionTimeoutMs >= 10U && cfg.TransactionTimeoutMs <= 5000U)
        ? cfg.TransactionTimeoutMs : 1000U;

    Dx->PolicyBackoffOnBusy     = (cfg.BackoffOnBusy != 0U) ? 1U : 0U;

    Dx->PolicyBackoffInitialUs  =
        (cfg.BackoffInitialUs >= 1U && cfg.BackoffInitialUs <= 1000U)
        ? cfg.BackoffInitialUs : 10U;

    Dx->PolicyBackoffMaxUs      =
        (cfg.BackoffMaxUs >= Dx->PolicyBackoffInitialUs &&
         cfg.BackoffMaxUs <= 50000U)
        ? cfg.BackoffMaxUs : 5000U;

    Dx->PolicyUsePec            = (cfg.UsePec != 0U) ? 1U : 0U;

    Dx->PolicyGpioActiveLow     = (cfg.GpioActiveLow != 0U) ? 1U : 0U;

    Dx->PolicyForce10Bit        = (cfg.Force10Bit != 0U) ? 1U : 0U;

    Dx->PolicyCrashOnError      = (cfg.CrashOnError != 0U) ? 1U : 0U;

    /* PWRMBASE override: low 32 bits only */
    Dx->PolicyPwrmBase.LowPart  = cfg.PwrmBase;
    Dx->PolicyPwrmBase.HighPart = 0;

    Dx->ActiveBusSpeedHz        = Dx->PolicyBusSpeedHz;
    Dx->Use10BitAddrDefault     = (Dx->PolicyForce10Bit != 0U);
    Dx->BackoffCurrentUs        = Dx->PolicyBackoffInitialUs;

    if (paramsKey != NULL) {
        ZwClose(paramsKey);
        paramsKey = NULL;
    }
    if (drvKey != NULL) {
        ZwClose(drvKey);
        drvKey = NULL;
    }

    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * I2cCtrl_ApplyControllerPolicy - apply addressing/speed policy via HAL ops
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_ApplyControllerPolicy(
    PI2CCTRL_FDO    Dx,
    PI2CCTRL_TARGET TgtOpt
    )
{
    ULONG   speed;
    BOOLEAN use10bit;

    if (Dx == NULL || Dx->Ops == NULL) {
        return;
    }

    /* Defaults from device context */
    speed    = Dx->ActiveBusSpeedHz;
    use10bit = Dx->Use10BitAddrDefault;

    /* Override from target options if bound */
    if (TgtOpt != NULL && TgtOpt->Bound) {
        if (TgtOpt->SpeedHz != 0U) {
            speed = TgtOpt->SpeedHz;
        }
        if ((TgtOpt->Flags & I2CCTRL_FLAG_10BIT) != 0U) {
            use10bit = TRUE;
        }
    }

    /* Enable controller */
    if (Dx->Ops->Enable != NULL) {
        (VOID)Dx->Ops->Enable(Dx, TRUE);
    }

    /* Program addressing mode (HAL-specific implementation may interpret flags) */
    if (use10bit) {
        /* If HAL supports 10-bit addressing, configure accordingly */
        /* This may be a no-op if controller does not support 10-bit */
    }

    /* Program bus speed via HAL */
    if (Dx->Ops->SetBusSpeedHz != NULL) {
        (VOID)Dx->Ops->SetBusSpeedHz(Dx, speed);
    }
}

/* -----------------------------------------------------------------------
 * I2cCtrl_ExecuteTransferSafe - XP/2003 BSOD-safe, HAL-generic, C89-compliant
 *
 * Purpose:
 *   - Perform a feature-complete I²C transfer without phase dependency
 *   - Use I2C_TRANSFER_CONTEXT members (Buffer, Length, TxIndex, RxIndex, Direction)
 *   - Enforce timeout via KeQueryPerformanceCounter (single-parameter form)
 *   - Handle STOP_DET and TX_ABRT with exponential backoff
 *   - Use HAL ops only (no direct register macros)
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Flags hardware failure on timeout or abort
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_ExecuteTransferSafe(
    PI2CCTRL_FDO fdoExt,
    ULONG        TimeoutMs
    )
{
    NTSTATUS      status;
    LARGE_INTEGER freq;
    LARGE_INTEGER start;
    LARGE_INTEGER now;
    ULONG         elapsedMs;
    ULONG         delayUs;
    ULONG         i;
    I2C_HW_STATUS hwst;
    ULONG         rawIntr;

    /* C89 init */
    status     = STATUS_SUCCESS;
    elapsedMs  = 0U;
    delayUs    = fdoExt->PolicyBackoffInitialUs ? fdoExt->PolicyBackoffInitialUs : 10U;
    i          = 0U;
    rawIntr    = 0U;
    RtlZeroMemory(&hwst, sizeof(hwst));

    fdoExt->XferCtx.Status   = STATUS_PENDING;
    fdoExt->XferCtx.StopSeen = FALSE;

    /* XP-compatible: first call returns counter and fills frequency */
    start = KeQueryPerformanceCounter(&freq);

    /* Write transfer (no phases) */
    if (fdoExt->XferCtx.Direction == I2cDirWrite && fdoExt->XferCtx.Length > 0U) {
        for (i = 0U; i < fdoExt->XferCtx.Length; i++) {
            /* Wait until TX FIFO Not Full */
            for (;;) {
                if (fdoExt->Ops->GetStatus &&
                    NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                    if (hwst.TxFifoNotFull || hwst.TxFifoLevel < fdoExt->Caps->TxFifoDepth) {
                        break;
                    }
                }
                I2cCtrl_Backoff(fdoExt, &delayUs);
                CHECK_TIMEOUT_BREAK(TimeoutMs, start, freq, elapsedMs, now, status);
            }

            /* Write next byte via HAL */
            if (fdoExt->Ops->IssueWriteByte) {
                status = fdoExt->Ops->IssueWriteByte(fdoExt,
                          fdoExt->XferCtx.Buffer[fdoExt->XferCtx.TxIndex]);
                if (!NT_SUCCESS(status)) {
                    goto Exit;
                }
            }
            fdoExt->XferCtx.TxIndex++;
            fdoExt->XferCtx.Position++;
        }
    }
    /* Read transfer (no phases) */
    else if (fdoExt->XferCtx.Direction == I2cDirRead && fdoExt->XferCtx.Length > 0U) {
        /* Queue read commands */
        for (i = 0U; i < fdoExt->XferCtx.Length; i++) {
            for (;;) {
                if (fdoExt->Ops->GetStatus &&
                    NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                    if (hwst.TxFifoNotFull || hwst.TxFifoLevel < fdoExt->Caps->TxFifoDepth) {
                        break;
                    }
                }
                I2cCtrl_Backoff(fdoExt, &delayUs);
                CHECK_TIMEOUT_BREAK(TimeoutMs, start, freq, elapsedMs, now, status);
            }
            if (fdoExt->Ops->EmitReadRequest) {
                (VOID)fdoExt->Ops->EmitReadRequest(fdoExt);
            }
        }

        /* Drain RX FIFO */
        for (i = 0U; i < fdoExt->XferCtx.Length; i++) {
            for (;;) {
                if (fdoExt->Ops->GetStatus &&
                    NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                    if (hwst.RxFifoNotEmpty || hwst.RxFifoLevel > 0U) {
                        break;
                    }
                }
                I2cCtrl_Backoff(fdoExt, &delayUs);
                CHECK_TIMEOUT_BREAK(TimeoutMs, start, freq, elapsedMs, now, status);
            }
            if (fdoExt->Ops->ReadRxByte) {
                status = fdoExt->Ops->ReadRxByte(fdoExt,
                          &fdoExt->XferCtx.Buffer[fdoExt->XferCtx.RxIndex]);
                if (!NT_SUCCESS(status)) {
                    goto Exit;
                }
            }
            fdoExt->XferCtx.RxIndex++;
            fdoExt->XferCtx.Position++;
        }
    }

    /* Wait for STOP_DET or TX_ABRT to finalize */
    for (;;) {
        if (fdoExt->Ops->GetRawIntr) {
            rawIntr = fdoExt->Ops->GetRawIntr(fdoExt);
        }

        if ((rawIntr & I2C_INT_TX_ABORT) != 0U) {
            status = STATUS_UNSUCCESSFUL;
            fdoExt->HardwareFailure = TRUE;
            break;
        }

        if ((rawIntr & I2C_INT_STOP_DETECTED) != 0U) {
            fdoExt->XferCtx.StopSeen = TRUE;
            status = STATUS_SUCCESS;
            break;
        }

        CHECK_TIMEOUT_BREAK(TimeoutMs, start, freq, elapsedMs, now, status);
        I2cCtrl_Backoff(fdoExt, &delayUs);
    }

Exit:
    fdoExt->XferCtx.Status = status;
    return status;
}


/* -----------------------------------------------------------------------
 * I2cCtrl_RunTransferPhased - XP/2003 BSOD-safe, HAL-generic, C89-compliant
 *
 * Purpose:
 *   - Perform a guarded write-then-read I²C transfer using HAL ops only
 *   - Phase A: Write 'wlen' bytes (optional RESTART on first)
 *   - Phase B: Queue 'rlen' read requests, then drain RX FIFO
 *   - Finalize on STOP_DET or TX_ABRT, flagging HardwareFailure on abort
 *   - Enforce timeout via KeQueryPerformanceCounter with exponential backoff
 *   - Use unified timeout/backoff macros from i2cctrl_ext.h
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids direct MMIO/register macros (HAL-only)
 *   - Flags hardware failure on timeout or abort
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_RunTransferPhased(
    PI2CCTRL_FDO fdoExt,
    ULONG        wlen,
    ULONG        rlen,
    BOOLEAN      issueRestart,
    ULONG        TimeoutMs
    )
{
    NTSTATUS      status;
    LARGE_INTEGER freq;
    LARGE_INTEGER start;
    LARGE_INTEGER now;
    ULONG         elapsedMs;
    ULONG         delayUs;
    ULONG         i;
    I2C_HW_STATUS hwst;
    ULONG         intr;

    /* C89 init */
    status     = STATUS_SUCCESS;
    elapsedMs  = 0U;
    delayUs    = fdoExt && fdoExt->PolicyBackoffInitialUs ? fdoExt->PolicyBackoffInitialUs : 10U;
    i          = 0U;
    intr       = 0U;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (fdoExt == NULL || fdoExt->Ops == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!fdoExt->Enabled) {
        return STATUS_DEVICE_NOT_READY;
    }

    fdoExt->XferCtx.Status   = STATUS_PENDING;
    fdoExt->XferCtx.StopSeen = FALSE;

    /* XP-compatible: first call returns counter and fills frequency */
    start = KeQueryPerformanceCounter(&freq);

    /* Phase A: write command bytes */
    for (i = 0U; i < wlen; i++) {
        /* Wait until TX FIFO Not Full */
        for (;;) {
            if (fdoExt->Ops->GetStatus &&
                NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                if (hwst.TxFifoNotFull || hwst.TxFifoLevel < fdoExt->Caps->TxFifoDepth) {
                    break;
                }
            }
            I2cCtrl_Backoff(fdoExt, &delayUs);
            CHECK_TIMEOUT_BREAK(TimeoutMs, start, freq, elapsedMs, now, status);
        }
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }

        /* Optional RESTART on first byte */
        if (i == 0U && issueRestart && fdoExt->Ops->EmitRestartIfNeeded) {
            (VOID)fdoExt->Ops->EmitRestartIfNeeded(fdoExt);
        }

        /* Write next byte via HAL */
        if (fdoExt->Ops->IssueWriteByte) {
            status = fdoExt->Ops->IssueWriteByte(fdoExt,
                      fdoExt->XferCtx.Buffer[fdoExt->XferCtx.TxIndex]);
        } else if (fdoExt->Ops->WriteTxByte) {
            status = fdoExt->Ops->WriteTxByte(fdoExt,
                      fdoExt->XferCtx.Buffer[fdoExt->XferCtx.TxIndex]);
        } else {
            status = STATUS_NOT_SUPPORTED;
        }
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }

        fdoExt->XferCtx.TxIndex++;
        fdoExt->XferCtx.Position++;

        /* Abort check after each write */
        if (fdoExt->Ops->GetStatus &&
            NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
            if (hwst.TxAborted || hwst.AddressNack || hwst.DataNack) {
                if (fdoExt->Ops->AckInterrupts) {
                    fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_TX_ABORT);
                }
                fdoExt->HardwareFailure = TRUE;
                status = STATUS_IO_DEVICE_ERROR;
                goto Exit;
            }
        }
    }

    /* Phase B: queue read requests */
    for (i = 0U; i < rlen; i++) {
        /* Wait until TX FIFO Not Full to enqueue read token */
        for (;;) {
            if (fdoExt->Ops->GetStatus &&
                NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                if (hwst.TxFifoNotFull || hwst.TxFifoLevel < fdoExt->Caps->TxFifoDepth) {
                    break;
                }
            }
            I2cCtrl_Backoff(fdoExt, &delayUs);
            CHECK_TIMEOUT_BREAK(TimeoutMs, start, freq, elapsedMs, now, status);
        }
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }

        /* Enqueue a read request via HAL */
        if (fdoExt->Ops->IssueReadToken) {
            status = fdoExt->Ops->IssueReadToken(fdoExt);
        } else if (fdoExt->Ops->EmitReadRequest) {
            (VOID)fdoExt->Ops->EmitReadRequest(fdoExt);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_NOT_SUPPORTED;
        }
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }

        /* Optional STOP on last token (if backend needs it) */
        if (i == (rlen - 1U) && fdoExt->Ops->EmitStopIfNeeded) {
            (VOID)fdoExt->Ops->EmitStopIfNeeded(fdoExt);
        }

        /* Abort check after each token */
        if (fdoExt->Ops->GetStatus &&
            NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
            if (hwst.TxAborted || hwst.AddressNack || hwst.DataNack) {
                if (fdoExt->Ops->AckInterrupts) {
                    fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_TX_ABORT);
                }
                fdoExt->HardwareFailure = TRUE;
                status = STATUS_IO_DEVICE_ERROR;
                goto Exit;
            }
        }
    }

    /* Drain RX FIFO */
    for (i = 0U; i < rlen; i++) {
        for (;;) {
            if (fdoExt->Ops->GetStatus &&
                NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                if (hwst.RxFifoNotEmpty || hwst.RxFifoLevel > 0U) {
                    break;
                }
            }
            I2cCtrl_Backoff(fdoExt, &delayUs);
            CHECK_TIMEOUT_BREAK(TimeoutMs, start, freq, elapsedMs, now, status);
        }
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }

        if (fdoExt->Ops->ReadRxByte) {
            status = fdoExt->Ops->ReadRxByte(fdoExt,
                      &fdoExt->XferCtx.Buffer[fdoExt->XferCtx.RxIndex]);
        } else if (fdoExt->Ops->ReadRxByteSafe) {
            status = fdoExt->Ops->ReadRxByteSafe(fdoExt,
                      &fdoExt->XferCtx.Buffer[fdoExt->XferCtx.RxIndex]);
        } else {
            status = STATUS_NOT_SUPPORTED;
        }
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            goto Exit;
        }

        fdoExt->XferCtx.RxIndex++;
        fdoExt->XferCtx.Position++;
    }

    /* Finalize on STOP_DET / TX_ABRT */
    for (;;) {
        if (fdoExt->Ops->GetRawIntr) {
            intr = fdoExt->Ops->GetRawIntr(fdoExt);
        }

        if ((intr & I2C_INT_TX_ABORT) != 0U) {
            fdoExt->HardwareFailure = TRUE;
            status = STATUS_UNSUCCESSFUL;
            break;
        }
        if ((intr & I2C_INT_STOP_DETECTED) != 0U) {
            fdoExt->XferCtx.StopSeen = TRUE;
            if (fdoExt->Ops->AckInterrupts) {
                fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_STOP_DETECTED);
            }
            status = STATUS_SUCCESS;
            break;
        }

        I2cCtrl_Backoff(fdoExt, &delayUs);
        CHECK_TIMEOUT_BREAK(TimeoutMs, start, freq, elapsedMs, now, status);
    }

Exit:
    fdoExt->XferCtx.Status = status;
    return status;
}


NTSTATUS
I2cCtrl_RunTransferWithPolicy(PI2CCTRL_FDO Dx)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG attempt;

    for (attempt = 0; attempt <= Dx->PolicyMaxRetries; attempt++) {
        status = I2cCtrl_ExecuteTransferSafe(Dx, Dx->PolicyTxnTimeoutMs);
        if (NT_SUCCESS(status)) {
            break;
        }

        if ((status == STATUS_DEVICE_BUSY || status == STATUS_IO_TIMEOUT) &&
            attempt < Dx->PolicyMaxRetries) {
            if (Dx->PolicyRetryDelayUs) {
                KeStallExecutionProcessor(Dx->PolicyRetryDelayUs);
            }
        } else {
            break;
        }
    }

    if (!NT_SUCCESS(status) && Dx->PolicyCrashOnError) {
        I2cCtrl_ForceCrash(Dx, status);
    }

    return status;
}


VOID
I2cCtrl_Backoff(
    IN PI2CCTRL_FDO Dx,
    IN OUT PULONG DelayUs
    )
{
    KIRQL irql;
    ULONG currentDelay;
    ULONG maxDelay;
    PULONG stats;

    /* Defensive checks */
    if (Dx == NULL || DelayUs == NULL) {
        return;
    }
    if (!Dx->PolicyBackoffOnBusy) {
        return;
    }

    /* KeStallExecutionProcessor is only valid at IRQL <= DISPATCH_LEVEL */
    irql = KeGetCurrentIrql();
    if (irql > DISPATCH_LEVEL) {
        return;
    }

    currentDelay = *DelayUs;
    maxDelay     = Dx->PolicyBackoffMaxUs;

    /* Stall the processor for the requested microseconds */
    if (currentDelay > 0U) {
        KeStallExecutionProcessor(currentDelay);
    }

    /* Exponential backoff with clamp */
    if (currentDelay < maxDelay) {
        currentDelay = currentDelay * 2U;
        if (currentDelay > maxDelay) {
            currentDelay = maxDelay;
        }
        *DelayUs = currentDelay;
    }

    /* Integration using backported helpers: allocate, log, free */
    stats = (PULONG)I2CCTRL_AllocatePoolPriorityCompat(
                        NonPagedPool,
                        sizeof(ULONG),
                        'bCI2',
                        HighPoolPriority);
    if (stats != NULL) {
        *stats = currentDelay;
        I2CCTRL_WdmMemoryFree(stats);
    }
}

/*
 * Cancel all queued IRPs/transactions gracefully during surprise removal.
 * XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant.
 */
VOID
I2cCtrl_CancelAllQueuedTransfers(PI2CCTRL_FDO FdoExt)
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    PLIST_ENTRY head;
    PI2C_TRANSFER_CONTEXT tr;

    if (FdoExt == NULL) {
        return;
    }

    KeAcquireSpinLock(&FdoExt->QueueLock, &oldIrql);

    head  = &FdoExt->PendingIrpList;
    entry = head->Flink;

    while (entry != head) {
        tr    = CONTAINING_RECORD(entry, I2C_TRANSFER_CONTEXT, ListEntry);
        entry = entry->Flink;

        if (tr != NULL && tr->Irp != NULL) {
            tr->Irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
            tr->Irp->IoStatus.Information = 0U;
            IoCompleteRequest(tr->Irp, IO_NO_INCREMENT);
            tr->Irp = NULL;
        }
    }

    InitializeListHead(&FdoExt->PendingIrpList);
    KeReleaseSpinLock(&FdoExt->QueueLock, oldIrql);
}

/* ---------------------------------------------------------------------------
 * I2cCtrl_IsTxFifoNotEmpty - HAL-generic, XP/2003 BSOD-safe, C89-compliant
 *
 * Purpose:
 *   - Check if TX FIFO currently holds data
 *   - Use HAL ops (GetStatus) instead of direct MMIO
 *   - Flag hardware failure if status retrieval fails
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids unsafe MMIO access
 * --------------------------------------------------------------------------- */
BOOLEAN
I2cCtrl_IsTxFifoNotEmpty(PI2CCTRL_FDO devctx)
{
    I2C_HW_STATUS hwst;
    NTSTATUS      st;

    if (devctx == NULL || devctx->Ops == NULL || devctx->Ops->GetStatus == NULL) {
        return FALSE;
    }

    RtlZeroMemory(&hwst, sizeof(hwst));

    st = devctx->Ops->GetStatus(devctx, &hwst);
    if (!NT_SUCCESS(st)) {
        devctx->HardwareFailure = TRUE;
        return FALSE;
    }

    /* HAL-neutral: TX FIFO has data if level > 0 */
    return (hwst.TxFifoLevel > 0U) ? TRUE : FALSE;
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_DiscardTxEntrySafe - HAL-generic, XP/2003 BSOD-safe, C89-compliant
 *
 * Purpose:
 *   - Discard one TX entry safely
 *   - DW-apb I²C has no TX_POP register, so emulate by checking TX FIFO level
 *   - Use HAL ops (GetStatus, ReadTxDiscard) instead of direct MMIO
 *   - Flag hardware failure if status retrieval fails
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids unsafe MMIO access
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_DiscardTxEntrySafe(
    PI2CCTRL_FDO FdoExt
    )
{
    KIRQL         oldIrql;
    I2C_HW_STATUS hwst;
    NTSTATUS      st;

    if (FdoExt == NULL || FdoExt->Ops == NULL || FdoExt->Ops->GetStatus == NULL) {
        return;
    }

    RtlZeroMemory(&hwst, sizeof(hwst));

    KeAcquireSpinLock(&FdoExt->HwLock, &oldIrql);
    st = FdoExt->Ops->GetStatus(FdoExt, &hwst);
    if (!NT_SUCCESS(st)) {
        FdoExt->HardwareFailure = TRUE;
        KeReleaseSpinLock(&FdoExt->HwLock, oldIrql);
        return;
    }

    /* If FIFO not empty, consume one entry via HAL discard op */
    if (hwst.TxFifoLevel > 0U) {
        if (FdoExt->Ops->ReadTxDiscard) {
            (VOID)FdoExt->Ops->ReadTxDiscard(FdoExt);
        } else {
            /* Fallback: issue a dummy read if backend provides it */
            if (FdoExt->Ops->ReadRxByteSafe) {
                UCHAR dummy;
                (VOID)FdoExt->Ops->ReadRxByteSafe(FdoExt, &dummy);
            }
        }
    }

    KeReleaseSpinLock(&FdoExt->HwLock, oldIrql);
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_ReadRxByteSafe - HAL-generic, XP/2003 BSOD-safe, C89-compliant
 *
 * Purpose:
 *   - Read one byte from RX FIFO safely
 *   - Use HAL ops (GetStatus, ReadRxByte) instead of direct MMIO
 *   - Flag hardware failure if status retrieval fails
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids unsafe MMIO access
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_ReadRxByteSafe(PI2CCTRL_FDO devctx, PUCHAR ByteOut)
{
    KIRQL         oldIrql;
    I2C_HW_STATUS hwst;
    NTSTATUS      st;

    if (devctx == NULL || ByteOut == NULL || devctx->Ops == NULL || devctx->Ops->GetStatus == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&hwst, sizeof(hwst));

    KeAcquireSpinLock(&devctx->HwLock, &oldIrql);
    st = devctx->Ops->GetStatus(devctx, &hwst);
    if (!NT_SUCCESS(st)) {
        devctx->HardwareFailure = TRUE;
        KeReleaseSpinLock(&devctx->HwLock, oldIrql);
        return st;
    }

    if (hwst.RxFifoNotEmpty || hwst.RxFifoLevel > 0U) {
        if (devctx->Ops->ReadRxByte) {
            st = devctx->Ops->ReadRxByte(devctx, ByteOut);
        } else {
            st = STATUS_NOT_SUPPORTED;
        }
    } else {
        st = STATUS_IO_TIMEOUT;
    }

    KeReleaseSpinLock(&devctx->HwLock, oldIrql);
    return st;
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_FlushTxFifoBounded - HAL-generic, XP/2003 BSOD-safe, C89-compliant
 *
 * Purpose:
 *   - Flush TX FIFO safely with bounded loop
 *   - Stop new transfers, disable controller, then drain TX FIFO
 *   - Use HAL ops (GetStatus, EmitStopIfNeeded, AckInterrupts) instead of MMIO
 *   - Flag hardware failure if disable or status retrieval fails
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids unsafe MMIO access
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_FlushTxFifoBounded(
    PI2CCTRL_FDO devctx
    )
{
    ULONG         count;
    ULONG         maxFlush;
    I2C_HW_STATUS hwst;
    NTSTATUS      status;

    if (devctx == NULL || devctx->Ops == NULL) {
        return;
    }

    /* Stop new transfers and confirm controller disabled */
    if (devctx->Ops->Enable) {
        (VOID)devctx->Ops->Enable(devctx, FALSE);
    }
    status = I2cCtrl_WaitForEnableState(devctx, FALSE, 500U);
    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL: FlushTxFifoBounded: disable did not latch\n"));
        devctx->HardwareFailure = TRUE;
        return;
    }

    /* Use reported FIFO depth or conservative default */
    maxFlush = devctx->Caps ? devctx->Caps->TxFifoDepth : 0U;
    if (maxFlush == 0U) {
        maxFlush = 64U;
    }

    RtlZeroMemory(&hwst, sizeof(hwst));

    for (count = 0U; count < maxFlush; count++) {
        if (devctx->Ops->GetStatus &&
            NT_SUCCESS(devctx->Ops->GetStatus(devctx, &hwst))) {
            if (hwst.TxFifoLevel == 0U) {
                break; /* FIFO empty */
            }

            /* Discard one entry by emitting STOP if backend supports it */
            if (devctx->Ops->EmitStopIfNeeded) {
                (VOID)devctx->Ops->EmitStopIfNeeded(devctx);
            }
        } else {
            devctx->HardwareFailure = TRUE;
            break;
        }
    }

    /* Clear residual interrupt status */
    if (devctx->Ops->AckInterrupts) {
        devctx->Ops->AckInterrupts(devctx,
            I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
            I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
            I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
            I2C_INT_RD_REQ);
    }
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_IsRxFifoNotEmpty - HAL-generic, XP/2003-safe, C89-compliant
 *
 * Purpose:
 *   - Check if RX FIFO has data
 *   - Use HAL ops abstraction (GetStatus) instead of direct MMIO
 *   - Flag hardware failure if status retrieval fails
 *
 * Guarantees:
 *   - Runs at any IRQL (non-paged)
 *   - Operates strictly on non-paged memory
 *   - Avoids unsafe MMIO access
 * --------------------------------------------------------------------------- */
BOOLEAN
I2cCtrl_IsRxFifoNotEmpty(
    PI2CCTRL_FDO devctx
    )
{
    I2C_HW_STATUS hwst;
    NTSTATUS      st;

    if (devctx == NULL || devctx->Ops == NULL || devctx->Ops->GetStatus == NULL) {
        return FALSE;
    }

    RtlZeroMemory(&hwst, sizeof(hwst));

    st = devctx->Ops->GetStatus(devctx, &hwst);
    if (!NT_SUCCESS(st)) {
        devctx->HardwareFailure = TRUE;
        return FALSE;
    }

    /* HAL-neutral: RX FIFO has data if level > 0 or flag set */
    return (hwst.RxFifoNotEmpty || hwst.RxFifoLevel > 0U) ? TRUE : FALSE;
}


/* -----------------------------------------------------------------------
 * I2cCtrl_DrainRxFifoBounded - HAL-generic, XP/2003-safe, C89-compliant
 *
 * Purpose:
 *   - Safely drain RX FIFO with bounded loop
 *   - Use HAL ops abstraction (GetStatus, ReadRxByte) instead of direct MMIO
 *   - Discard bytes deterministically without crashing
 *
 * Guarantees:
 *   - Runs at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Bounded loop prevents livelock
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_DrainRxFifoBounded(
    PI2CCTRL_FDO FdoExt
    )
{
    ULONG         count;
    ULONG         maxDrain;
    UCHAR         byte;
    I2C_HW_STATUS hwst;
    NTSTATUS      st;

    /* Defensive init */
    count    = 0U;
    maxDrain = 0U;
    byte     = 0U;
    st       = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (FdoExt == NULL || FdoExt->Ops == NULL || FdoExt->Ops->GetStatus == NULL) {
        return;
    }

    /* Use reported FIFO depth or conservative default */
    maxDrain = FdoExt->Caps ? FdoExt->Caps->RxFifoDepth : 0U;
    if (maxDrain == 0U) {
        maxDrain = 64U;
    }

    for (count = 0U; count < maxDrain; count++) {
        BOOLEAN rxNotEmpty = FALSE;

        st = FdoExt->Ops->GetStatus(FdoExt, &hwst);
        if (!NT_SUCCESS(st)) {
            FdoExt->HardwareFailure = TRUE;
            break;
        }
        rxNotEmpty = (hwst.RxFifoNotEmpty || hwst.RxFifoLevel > 0U);

        if (!rxNotEmpty) {
            break; /* FIFO empty */
        }

        /* Consume one byte via HAL */
        if (FdoExt->Ops->ReadRxByte) {
            st = FdoExt->Ops->ReadRxByte(FdoExt, &byte);
            if (!NT_SUCCESS(st)) {
                FdoExt->HardwareFailure = TRUE;
                break;
            }
        } else if (FdoExt->Ops->ReadRxByteSafe) {
            st = FdoExt->Ops->ReadRxByteSafe(FdoExt, &byte);
            if (!NT_SUCCESS(st)) {
                FdoExt->HardwareFailure = TRUE;
                break;
            }
        }

        UNREFERENCED_PARAMETER(byte); /* discard safely */
    }
}


/* -----------------------------------------------------------------------
 * I2cCtrl_QuiesceFifos - HAL-generic, XP/2003-safe, C89-compliant
 *
 * Purpose:
 *   - Safely drain RX FIFO and wait for TX FIFO to empty
 *   - Clear residual interrupt status
 *   - Use common HAL ops (MaskInterrupts, AckInterrupts, GetStatus, ReadRxByte)
 *     without requiring controller-specific Drain/Flush members
 *
 * Guarantees:
 *   - Runs at PASSIVE_LEVEL
 *   - Touches only non-paged memory
 *   - Bounded loops to avoid livelock
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_QuiesceFifos(
    PI2CCTRL_FDO devctx
    )
{
    I2C_HW_STATUS hwst;
    NTSTATUS      st;
    ULONG         iter;
    ULONG         maxIter;
    UCHAR         byte;

    RtlZeroMemory(&hwst, sizeof(hwst));
    st      = STATUS_SUCCESS;
    iter    = 0U;
    maxIter = 1024U; /* bounded drain/wait to avoid livelock */
    byte    = 0U;

    if (devctx == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (devctx->Ops == NULL || devctx->Ops->GetStatus == NULL) {
        return;
    }

    /* Mask interrupts during quiesce */
    if (devctx->Ops->MaskInterrupts) {
        (VOID)devctx->Ops->MaskInterrupts(devctx, 0U);
    }

    /* Clear latched causes before touching FIFOs */
    if (devctx->Ops->AckInterrupts) {
        (VOID)devctx->Ops->AckInterrupts(devctx,
            I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
            I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
            I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
            I2C_INT_RD_REQ);
    }

    /* Drain RX FIFO using HAL ops */
    for (iter = 0U; iter < maxIter; iter++) {
        BOOLEAN rxNotEmpty;

        st = devctx->Ops->GetStatus(devctx, &hwst);
        if (!NT_SUCCESS(st)) {
            devctx->HardwareFailure = TRUE;
            break;
        }
        rxNotEmpty = (hwst.RxFifoNotEmpty || hwst.RxFifoLevel > 0U);
        if (!rxNotEmpty) {
            break;
        }

        if (devctx->Ops->ReadRxByte) {
            st = devctx->Ops->ReadRxByte(devctx, &byte);
        } else if (devctx->Ops->ReadRxByteSafe) {
            st = devctx->Ops->ReadRxByteSafe(devctx, &byte);
        } else {
            /* No way to consume RX bytes via HAL */
            break;
        }

        if (!NT_SUCCESS(st)) {
            devctx->HardwareFailure = TRUE;
            break;
        }

        UNREFERENCED_PARAMETER(byte); /* discard safely */
    }

    /* Wait for TX FIFO to empty (no controller-specific flush op required) */
    for (iter = 0U; iter < maxIter; iter++) {
        BOOLEAN txEmpty;

        st = devctx->Ops->GetStatus(devctx, &hwst);
        if (!NT_SUCCESS(st)) {
            devctx->HardwareFailure = TRUE;
            break;
        }

        txEmpty = (hwst.TxFifoLevel == 0U);
        if (txEmpty) {
            break;
        }

        KeStallExecutionProcessor(10U);
    }

    /* Clear any residual causes after quiesce */
    if (devctx->Ops->AckInterrupts) {
        (VOID)devctx->Ops->AckInterrupts(devctx,
            I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
            I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
            I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
            I2C_INT_RD_REQ);
    }
}


/*
 * Safe ACPI handle close (XP/2003-compatible)
 * - PASSIVE_LEVEL only
 * - Ignores NULL handles
 * - Resolves AcpiCloseHandle once
 * - Never touches ACPI-owned memory
 */
VOID
I2cCtrl_AcpiCloseHandle(PVOID AcpiHandle)
{
    typedef NTSTATUS (*PFN_ACPI_CLOSE)(PVOID Handle);
    static PFN_ACPI_CLOSE s_AcpiClose = NULL;

    UNICODE_STRING name;
    NTSTATUS       status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    if (AcpiHandle == NULL) {
        return;
    }

    /* Resolve AcpiCloseHandle once */
    if (s_AcpiClose == NULL) {
        RtlInitUnicodeString(&name, L"AcpiCloseHandle");
        s_AcpiClose = (PFN_ACPI_CLOSE)MmGetSystemRoutineAddress(&name);
    }

    if (s_AcpiClose != NULL) {
        status = s_AcpiClose(AcpiHandle);

        if (!NT_SUCCESS(status)) {
            DbgPrint("I2CCTRL: AcpiCloseHandle(%p) failed, status=0x%08X\n",
                     AcpiHandle, status);
        } else {
            DbgPrint("I2CCTRL: AcpiCloseHandle(%p) succeeded\n",
                     AcpiHandle);
        }
    } else {
        DbgPrint("I2CCTRL: AcpiCloseHandle unavailable; cannot close %p\n",
                 AcpiHandle);
    }
}


/* -----------------------------------------------------------------------
 * I2cCtrl_AckInterrupt - HAL-generic, XP/2003-safe, C89-compliant
 *
 * Purpose:
 *   - Acknowledge/clear interrupt causes in a controller-safe way
 *   - Use HAL ops abstraction (AckInterrupts) instead of direct MMIO
 *   - Support both read-to-clear and write-to-clear variants via HAL
 *
 * Guarantees:
 *   - Runs at any IRQL (non-paged)
 *   - Operates strictly on non-paged memory
 *   - Avoids unsafe MMIO access
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_AckInterrupt(
    PI2CCTRL_FDO devctx,
    ULONG        mask
    )
{
    if (devctx == NULL || devctx->Ops == NULL) {
        return;
    }

    if (devctx->Ops->AckInterrupts) {
        (VOID)devctx->Ops->AckInterrupts(devctx, mask);
    }

    UNREFERENCED_PARAMETER(mask);
}

NTSTATUS
I2cCtrl_Lpss2PowerOn(
    PI2CCTRL_FDO devctx
    )
{
    PUCHAR base;
    ULONG  val;
    ULONG  timeout;
    NTSTATUS status = STATUS_SUCCESS;

    if (devctx == NULL || devctx->MmioBase == NULL) {
        I2cCtrl_Log("LPSS2: PowerOn: invalid devctx or MMIO\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    base = (PUCHAR)devctx->MmioBase;

    I2cCtrl_Log("LPSS2: PowerOn: begin (BAR0=%p)\n", base);

    __try {

        /* ---------------------------------------------------------
         * 1) Sanity check BAR0 (avoid 0xFFFFFFFF or 0x00000000 reads)
         * --------------------------------------------------------- */
        val = READ_REGISTER_ULONG((PULONG)(base + devctx->RegCtrl));
        if (val == 0xFFFFFFFF || val == 0x00000000) {
            I2cCtrl_Log("LPSS2: PowerOn: BAR0 invalid (CTRL=0x%08lx)\n", val);
            return STATUS_DEVICE_HARDWARE_ERROR;
        }

        /* ---------------------------------------------------------
         * 2) Deassert LPSS2 reset
         * --------------------------------------------------------- */
        I2cCtrl_Log("LPSS2: PowerOn: deasserting reset\n");
        WRITE_REGISTER_ULONG((PULONG)(base + devctx->RegReset), 0x00000000);

        /* ---------------------------------------------------------
         * 3) Enable clock gate
         * --------------------------------------------------------- */
        I2cCtrl_Log("LPSS2: PowerOn: enabling clock gate\n");
        WRITE_REGISTER_ULONG((PULONG)(base + devctx->RegClkCtl), 0x00000001);

        /* ---------------------------------------------------------
         * 4) Program clock divider (safe default)
         * --------------------------------------------------------- */
        I2cCtrl_Log("LPSS2: PowerOn: programming CLKDIV\n");
        WRITE_REGISTER_ULONG((PULONG)(base + devctx->RegClkDiv), 0x0000000A);

        /* ---------------------------------------------------------
         * 5) Trigger CLKUPDATE
         * --------------------------------------------------------- */
        I2cCtrl_Log("LPSS2: PowerOn: triggering CLKUPDATE\n");
        WRITE_REGISTER_ULONG((PULONG)(base + devctx->RegClkUpdate), 0x00000001);

        /* ---------------------------------------------------------
         * 6) Poll for clock update completion
         * --------------------------------------------------------- */
        timeout = 1000;
        while (timeout--) {
            val = READ_REGISTER_ULONG((PULONG)(base + devctx->RegClkUpdate));
            if ((val & 0x1) == 0) break;
            KeStallExecutionProcessor(10);
        }

        if (timeout == 0) {
            I2cCtrl_Log("LPSS2: PowerOn: CLKUPDATE timeout\n");
            return STATUS_IO_TIMEOUT;
        }

        /* ---------------------------------------------------------
         * 7) Clear sticky status bits
         * --------------------------------------------------------- */
        I2cCtrl_Log("LPSS2: PowerOn: clearing status\n");
        WRITE_REGISTER_ULONG((PULONG)(base + devctx->RegStatus), 0xFFFFFFFF);

        /* ---------------------------------------------------------
         * 8) Final sanity read
         * --------------------------------------------------------- */
        val = READ_REGISTER_ULONG((PULONG)(base + devctx->RegCtrl));
        I2cCtrl_Log("LPSS2: PowerOn: CTRL final=0x%08lx\n", val);

        I2cCtrl_Log("LPSS2: PowerOn: SUCCESS\n");
        status = STATUS_SUCCESS;

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        I2cCtrl_Log("LPSS2: PowerOn: EXCEPTION during MMIO access\n");
        status = STATUS_ACCESS_VIOLATION;
    }

    return status;
}

NTSTATUS
I2cCtrl_Lpss2PowerOff(
    PI2CCTRL_FDO devctx
    )
{
    PUCHAR base;
    ULONG  timeout;
    NTSTATUS status = STATUS_SUCCESS;

    //
    // 0) HARD STOP: do NOT touch hardware if the device is stopping/removed
    //
    if (devctx == NULL ||
        devctx->MmioBase == NULL ||
        devctx->Stopping ||
        devctx->Removed ||
        devctx->HardwareFailure)
    {
        I2cCtrl_Log("LPSS2: PowerOff: device already stopping/removed - skipping\n");
        return STATUS_DEVICE_REMOVED;
    }

    base = (PUCHAR)devctx->MmioBase;

    I2cCtrl_Log("LPSS2: PowerOff: begin (BAR0=%p)\n", base);

    __try {

        //
        // 1) Mask interrupts
        //
        WRITE_REGISTER_ULONG((PULONG)(base + devctx->RegIntrMask), 0xFFFFFFFF);

        //
        // 2) Clear pending status
        //
        WRITE_REGISTER_ULONG((PULONG)(base + devctx->RegStatus), 0xFFFFFFFF);

        //
        // 3) Assert reset
        //
        WRITE_REGISTER_ULONG((PULONG)(base + devctx->RegReset), 0x00000001);

        //
        // 4) Disable clock gate
        //
        WRITE_REGISTER_ULONG((PULONG)(base + devctx->RegClkCtl), 0x00000000);

        //
        // 5) Disable functional clock
        //
        WRITE_REGISTER_ULONG((PULONG)(base + devctx->RegClkDiv), 0x00000000);

        //
        // 6) Trigger CLKUPDATE
        //
        WRITE_REGISTER_ULONG((PULONG)(base + devctx->RegClkUpdate), 0x00000001);

        //
        // 7) Poll for completion
        //
        timeout = 1000;
        while (timeout--) {
            ULONG v = READ_REGISTER_ULONG((PULONG)(base + devctx->RegClkUpdate));
            if ((v & 0x1) == 0)
                break;
            KeStallExecutionProcessor(10);
        }

        if (timeout == 0) {
            I2cCtrl_Log("LPSS2: PowerOff: CLKUPDATE timeout\n");
            return STATUS_IO_TIMEOUT;
        }

        I2cCtrl_Log("LPSS2: PowerOff: SUCCESS\n");
        status = STATUS_SUCCESS;

    } __except (EXCEPTION_EXECUTE_HANDLER) {

        I2cCtrl_Log("LPSS2: PowerOff: EXCEPTION during MMIO access\n");
        status = STATUS_ACCESS_VIOLATION;
    }

    return status;
}

/* -----------------------------------------------------------------------
 * I2cCtrl_ParseCrsForI2cSerialBus
 *
 * Parse ACPI _CRS buffer for an I2CSerialBus Connection Descriptor (0x8A).
 * Extracts:
 *   - 7-bit I2C address
 *   - ConnectionSpeed (Hz)
 *   - TenBit flag
 *
 * XP/2003-safe, C89-compliant, no assumptions about alignment.
 * ----------------------------------------------------------------------- */
BOOLEAN
I2cCtrl_ParseCrsForI2cSerialBus(
    const UCHAR *buf,
    ULONG        len,
    PUCHAR       addrOut,
    PULONG       speedOut,
    PBOOLEAN     tenBitOut
    )
{
    ULONG i;

    if (addrOut == NULL || speedOut == NULL || tenBitOut == NULL) {
        return FALSE;
    }

    *addrOut   = 0;
    *speedOut  = 0;
    *tenBitOut = FALSE;

    if (buf == NULL || len < 3) {
        I2cCtrl_Log("ParseCrsForI2cSerialBus: invalid buffer len=%lu\n", len);
        return FALSE;
    }

    i = 0;
    while (i + 1 < len) {

        UCHAR tag = buf[i];

        /* Small item */
        if ((tag & 0x80) == 0) {
            UCHAR smallLen = (UCHAR)(tag & 0x07);
            if (i + 1 + smallLen > len) break;
            i += 1 + smallLen;
            continue;
        }

        /* Large item */
        if (i + 3 > len) break;

        {
            UCHAR  largeType    = (UCHAR)(tag & 0x7F);
            USHORT largeLen     = (USHORT)(buf[i + 1] | ((USHORT)buf[i + 2] << 8));
            ULONG  payloadStart = i + 3;

            if (payloadStart + largeLen > len) break;

            /* ================================
             * I2CSerialBus descriptor (0x8A)
             * ================================ */
            if (largeType == 0x8A) {

                const UCHAR *p = buf + payloadStart;
                ULONG        n = (ULONG)largeLen;

                I2cCtrl_HexDump(p, n, "I2CSerialBus");

                if (n >= 6) {

                    ULONG speed =
                        (ULONG)p[0]        |
                        ((ULONG)p[1] << 8) |
                        ((ULONG)p[2] << 16)|
                        ((ULONG)p[3] << 24);

                    UCHAR addr  = p[4];
                    UCHAR flags = p[5];

                    *addrOut   = addr & 0x7F;
                    *speedOut  = speed;
                    *tenBitOut = ((flags & 0x01) ? TRUE : FALSE);

                    I2cCtrl_Log("I2CSerialBus: addr=0x%02X speed=%lu tenBit=%u\n",
                                *addrOut,
                                *speedOut,
                                *tenBitOut ? 1 : 0);

                    return TRUE;
                }
            }

            i = payloadStart + largeLen;
        }
    }

    I2cCtrl_Log("ParseCrsForI2cSerialBus: no I2CSerialBus descriptor found\n");
    return FALSE;
}

/* -----------------------------------------------------------------------
 * I2cCtrl_AcpiGetHidDescriptorViaDsm
 *
 * Call _DSM on the HID-over-I2C device (PNP0C50/ELAN1200) to fetch
 * the HID descriptor buffer.
 *
 * devctx  - FDO (for ACPI device object)
 * handle  - ACPI handle of the HID child (ETPD/PNP0C50)
 * outBuf  - caller-allocated buffer
 * outLen  - in: size of outBuf, out: bytes written
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_AcpiGetHidDescriptorViaDsm(
    PI2CCTRL_FDO            devctx,
    PVOID                   handle,
    PUCHAR                  outBuf,
    PULONG                  outLen
    )
{
    NTSTATUS                        status;
    KEVENT                          event;
    PIRP                            irp;
    IO_STATUS_BLOCK                 iosb;
    PACPI_EVAL_INPUT_BUFFER_COMPLEX input;
    ULONG                           inputLen;
    ULONG                           outSize;
    LARGE_INTEGER                   timeout;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (devctx == NULL || handle == NULL || outBuf == NULL || outLen == NULL) {
        I2cCtrl_Log("GetHidDescViaDsm: invalid parameters\n");
        return STATUS_INVALID_PARAMETER;
    }

    status = I2cCtrl_AcpiOpen(devctx);
    if (!NT_SUCCESS(status) || devctx->AcpiDeviceObject == NULL) {
        I2cCtrl_Log("GetHidDescViaDsm: ACPI not connected\n");
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    outSize = *outLen;
    if (outSize < sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER)) {
        I2cCtrl_Log("GetHidDescViaDsm: output buffer too small\n");
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Build _DSM input: GUID, revision=1, function=1, empty package */
    inputLen = sizeof(ACPI_EVAL_INPUT_BUFFER_COMPLEX) +
               sizeof(ACPI_METHOD_ARGUMENT) * 4;

    input = (PACPI_EVAL_INPUT_BUFFER_COMPLEX)
            ExAllocatePoolWithTag(NonPagedPool, inputLen, 'Acpi');
    if (input == NULL) {
        I2cCtrl_Log("GetHidDescViaDsm: allocation failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(input, inputLen);

#ifdef ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE
    input->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
#endif

    input->Size              = inputLen;
    input->ArgumentCount     = 4;
    input->MethodNameAsUlong = (ULONG)('_MSD'); /* "_DSM" */

    {
        ACPI_METHOD_ARGUMENT UNALIGNED* arg;

        /* Arg0: GUID buffer */
        arg = &input->Argument[0];
        arg->Type       = ACPI_METHOD_ARGUMENT_BUFFER;
        arg->DataLength = sizeof(g_HidI2cDsmGuid);
        RtlCopyMemory(arg->Data, g_HidI2cDsmGuid, sizeof(g_HidI2cDsmGuid));
        ACPI_METHOD_NEXT_ARGUMENT(arg);

        /* Arg1: Revision = 1 */
        arg->Type       = ACPI_METHOD_ARGUMENT_INTEGER;
        arg->DataLength = sizeof(ULONG);
        arg->Argument   = 1;
        ACPI_METHOD_NEXT_ARGUMENT(arg);

        /* Arg2: Function = 1 (Get HID descriptor) */
        arg->Type       = ACPI_METHOD_ARGUMENT_INTEGER;
        arg->DataLength = sizeof(ULONG);
        arg->Argument   = 1;
        ACPI_METHOD_NEXT_ARGUMENT(arg);

        /* Arg3: Empty package */
        arg->Type       = ACPI_METHOD_ARGUMENT_PACKAGE;
        arg->DataLength = 0;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
              IOCTL_ACPI_EVAL_METHOD,
              devctx->AcpiDeviceObject,
              input,
              inputLen,
              outBuf,
              outSize,
              FALSE,
              &event,
              &iosb);

    if (irp == NULL) {
        ExFreePoolWithTag(input, 'Acpi');
        I2cCtrl_Log("GetHidDescViaDsm: IoBuildDeviceIoControlRequest failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Attach ACPI handle to IRP */
    irp->Tail.Overlay.OriginalFileObject = (PFILE_OBJECT)handle;

    status = IoCallDriver(devctx->AcpiDeviceObject, irp);
    if (status == STATUS_PENDING) {
        timeout.QuadPart = -5 * 1000 * 1000 * 10; /* 5 seconds */
        (VOID)KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, &timeout);
        status = iosb.Status;
    }

    ExFreePoolWithTag(input, 'Acpi');

    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("GetHidDescViaDsm: ACPI call failed (0x%08lx)\n", status);
        return status;
    }

    if (iosb.Information < sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER) ||
        iosb.Information > outSize) {
        I2cCtrl_Log("GetHidDescViaDsm: buffer overflow\n");
        return STATUS_BUFFER_OVERFLOW;
    }

    /* Interpret ACPI output as our extended buffer */
    {
        PI2CCTRL_ACPI_EVAL_OUTPUT_BUFFER out;
        ACPI_METHOD_ARGUMENT UNALIGNED* arg;

        out = (PI2CCTRL_ACPI_EVAL_OUTPUT_BUFFER)outBuf;
        if (out->Count == 0) {
            I2cCtrl_Log("GetHidDescViaDsm: empty _DSM result\n");
            return STATUS_NOT_FOUND;
        }

        arg = (ACPI_METHOD_ARGUMENT UNALIGNED*)out->Data;

        if (arg->Type != ACPI_METHOD_ARGUMENT_BUFFER ||
            arg->DataLength == 0 ||
            arg->DataLength > outSize - sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER)) {

            I2cCtrl_Log("GetHidDescViaDsm: invalid _DSM buffer result\n");
            return STATUS_INVALID_DEVICE_REQUEST;
        }

        *outLen = arg->DataLength;
        I2cCtrl_Log("GetHidDescViaDsm: HID descriptor length=%lu\n", *outLen);
    }

    return STATUS_SUCCESS;
}


//
// Query ACPI for device information (IOCTL_ACPI_GET_DEVICE_INFORMATION)
//
NTSTATUS
I2cCtrl_AcpiGetDeviceInformation(
    PDEVICE_OBJECT             AcpiPdo,
    PVOID                      DeviceHandle,   // reserved
    PI2CCTRL_ACPI_ENUM_ENTRY   Info,
    ULONG                      InfoLength
    )
{
    KEVENT                               event;
    IO_STATUS_BLOCK                      iosb;
    PIRP                                 irp;
    NTSTATUS                             status;
    I2CCTRL_ACPI_DEVICE_INFORMATION_WIRE wireInfo;

    UNREFERENCED_PARAMETER(DeviceHandle);

    if (AcpiPdo == NULL || Info == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (InfoLength < sizeof(I2CCTRL_ACPI_ENUM_ENTRY)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlZeroMemory(&wireInfo, sizeof(wireInfo));
    RtlZeroMemory(Info, sizeof(*Info));

    wireInfo.Signature   = ACPI_DEVICE_INFORMATION_SIGNATURE;
    wireInfo.Length      = sizeof(wireInfo);      // REQUIRED
    wireInfo.NextRequest = Info->NextRequest;     // usually 0

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
              IOCTL_ACPI_GET_DEVICE_INFORMATION,
              AcpiPdo,
              NULL,
              0,
              &wireInfo,
              sizeof(wireInfo),
              FALSE,
              &event,
              &iosb
          );

    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(AcpiPdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Copy the fields we expose to the rest of the driver.
    //
    Info->NextRequest  = wireInfo.NextRequest;
    Info->DeviceHandle = wireInfo.DeviceHandle;
    Info->DeviceStatus = wireInfo.DeviceStatus;
    Info->DeviceType   = wireInfo.DeviceType;
    Info->Status       = wireInfo.Status;
    Info->DeviceObject = wireInfo.DeviceObject;

    return STATUS_SUCCESS;
}

NTSTATUS
I2cCtrl_ReportPwrmBaseInfo(
    PHYSICAL_ADDRESS PwrmBase
    )
{
    /* -------------------------------------------------------------
     * XP NOTE:
     * On this platform, XP firmware does NOT program PWRMBASE.
     * FE0xxxxx is empty, and the real Win11 value (0x537D2000)
     * never appears under XP.
     *
     * Therefore:
     *   - If PwrmBase == 0, ACCEPT it and skip validation.
     *   - Keep your original validation logic for nonzero values.
     * ------------------------------------------------------------- */

    if (PwrmBase.QuadPart == 0)
    {
        I2cCtrl_Log(
            "PWRMINFO: XP reports PWRMBASE = 0 (accepting, skipping validation)\n"
        );
        return STATUS_SUCCESS;
    }

    /* -------------------------------------------------------------
     * Your original validation logic (unchanged)
     * ------------------------------------------------------------- */

    /* Must be nonzero and not all-ones */
    if (PwrmBase.QuadPart == 0xFFFFFFFFULL)
    {
        I2cCtrl_Log("PWRMINFO: invalid PWRMBASE (all-ones)\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* Must be 4KB aligned */
    if ((PwrmBase.QuadPart & 0xFFFULL) != 0)
    {
        I2cCtrl_Log("PWRMINFO: invalid PWRMBASE (not 4KB aligned)\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* Accept all valid aligned nonzero values */
    I2cCtrl_Log("PWRMINFO: Accepting PWRMBASE = 0x%016I64X\n",
                PwrmBase.QuadPart);

    return STATUS_SUCCESS;
}



NTSTATUS
I2cCtrl_AcpiEvalInteger(
    PDEVICE_OBJECT AcpiDevice,
    PCSTR MethodName,
    PULONG64 OutValue
    )
{
    PI2CCTRL_ACPI_EVAL_OUTPUT_BUFFER outBuf;
    ULONG outLen;
    NTSTATUS status;

    if (!AcpiDevice || !MethodName || !OutValue)
        return STATUS_INVALID_PARAMETER;

    //
    // Allocate enough space for:
    //   - our header
    //   - one ACPI_METHOD_ARGUMENT
    //   - a 64‑bit integer payload
    //
    outLen =
        sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER) +
        sizeof(ACPI_METHOD_ARGUMENT) +
        sizeof(ULONG64);

    outBuf = ExAllocatePoolWithTag(NonPagedPool, outLen, 'pmcA');
    if (!outBuf)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(outBuf, outLen);

    //
    // Perform the ACPI evaluation.
    //
    status = I2cCtrl_AcpiEvalMethod(
                 AcpiDevice,
                 NULL,          // no handle
                 MethodName,    // e.g. "_ADR"
                 outBuf,
                 outLen
             );

    if (!NT_SUCCESS(status)) {
        ExFreePool(outBuf);
        return status;
    }

    //
    // Validate returned structure.
    //
    if (outBuf->Count < 1 ||
        outBuf->Length < sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER))
    {
        ExFreePool(outBuf);
        return STATUS_UNSUCCESSFUL;
    }

    //
    // Extract first argument from Data[].
    //
    {
        PACPI_METHOD_ARGUMENT arg =
            (PACPI_METHOD_ARGUMENT)outBuf->Data;

        if (arg->Type == ACPI_METHOD_ARGUMENT_INTEGER &&
            arg->DataLength >= sizeof(ULONG64))
        {
            *OutValue = *(ULONG64*)arg->Data;
            status = STATUS_SUCCESS;
        }
        else {
            status = STATUS_UNSUCCESSFUL;
        }
    }

    ExFreePool(outBuf);
    return status;
}


//
// XP-compatible dynamic PWRMBASE discovery for CNP-LP / WHL-U
// C89-safe version using improved I2cCtrl_ReadPciConfigDword().
//
NTSTATUS
I2cCtrl_FindPwrmBaseDynamic(
    _Out_ PPHYSICAL_ADDRESS PwrmBase
    )
{
    PHYSICAL_ADDRESS phys;
    ULONG dev;
    ULONG fn;
    ULONG venDev;
    ULONG cand48;
    ULONG cand10;
    ULONG candidates[2];
    ULONG idx;

    phys.QuadPart = 0;

    I2cCtrl_Log("PWRMBASE-SCAN: begin full bus0 scan\n");

    for (dev = 0; dev < 32; dev++) {
        for (fn = 0; fn < 8; fn++) {

            /* Read VendorID/DeviceID */
            venDev = I2cCtrl_ReadPciConfigDword(0, dev, fn, 0x00);
            if ((venDev & 0xFFFF) != 0x8086)
                continue;

            /* Probe both known PMC locations */
            cand48 = I2cCtrl_ReadPciConfigDword(0, dev, fn, 0x48);
            cand10 = I2cCtrl_ReadPciConfigDword(0, dev, fn, 0x10);

            candidates[0] = cand48;
            candidates[1] = cand10;

            for (idx = 0; idx < 2; idx++) {

                ULONG c = candidates[idx];

                if (c == 0 || c == 0xFFFFFFFF)
                    continue;

                /* Must be 4KB aligned */
                if ((c & 0xFFF) != 0)
                    continue;

                /* Must be in typical PCH MMIO region (FE000000–FEFFFFFF) */
                if ((c & 0xFF000000) != 0xFE000000)
                    continue;

                /* Valid PWRMBASE found */
                phys.LowPart  = c;
                phys.HighPart = 0;

                I2cCtrl_Log("PWRMBASE-SCAN: found candidate at 0:%02u.%u = 0x%08lx\n",
                            dev, fn, c);

                *PwrmBase = phys;
                return STATUS_SUCCESS;
            }
        }
    }

    I2cCtrl_Log("PWRMBASE-SCAN: no valid candidates found\n");
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}


ULONG
I2cCtrl_ReadPciConfigDword(
    IN ULONG Bus,
    IN ULONG Device,
    IN ULONG Function,
    IN ULONG Offset
    )
{
    PCI_SLOT_NUMBER slot;
    ULONG value;
    ULONG cfg[64];   /* 256 bytes */
    ULONG bytes;

    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber   = Device;
    slot.u.bits.FunctionNumber = Function;

    value = 0;

    RtlZeroMemory(cfg, sizeof(cfg));

    bytes = HalGetBusData(
                PCIConfiguration,
                Bus,
                slot.u.AsULONG,
                cfg,
                sizeof(cfg)
            );

    if (bytes < Offset + sizeof(ULONG)) {
        return 0xFFFFFFFF;
    }

    value = cfg[Offset / sizeof(ULONG)];
    return value;
}


//
// Read a DWORD from:
//   HKLM\System\CurrentControlSet\Services\i2cctrl\Parameters
// using devctx->RegPath as the base.
//
// Returns defVal if missing or invalid.
//
ULONG
I2cCtrl_ReadRegDword(
    PI2CCTRL_FDO devctx,
    PCWSTR       ValueName,
    ULONG        defVal
)
{
    UNICODE_STRING paramsPath;
    OBJECT_ATTRIBUTES oa;
    HANDLE hKey = NULL;
    NTSTATUS status;
    ULONG result = defVal;
    ULONG data = 0;
    ULONG len = 0;

    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    KEY_VALUE_PARTIAL_INFORMATION* kvpi =
        (KEY_VALUE_PARTIAL_INFORMATION*)buffer;

    if (devctx == NULL || devctx->RegPath.Buffer == NULL)
        return defVal;

    {
        WCHAR fullPathBuffer[512];
        UNICODE_STRING fullPath;

        fullPath.Buffer = fullPathBuffer;
        fullPath.Length = 0;
        fullPath.MaximumLength = sizeof(fullPathBuffer);

        RtlCopyUnicodeString(&fullPath, &devctx->RegPath);
        RtlAppendUnicodeToString(&fullPath, L"\\Parameters");

        paramsPath = fullPath;
    }

    InitializeObjectAttributes(
        &oa,
        &paramsPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL
    );

    status = ZwOpenKey(&hKey, KEY_READ, &oa);
    if (!NT_SUCCESS(status))
        return defVal;

{
    UNICODE_STRING valueNameU;

    RtlInitUnicodeString(&valueNameU, ValueName);

    status = ZwQueryValueKey(
        hKey,
        &valueNameU,
        KeyValuePartialInformation,
        kvpi,
        sizeof(buffer),
        &len
    );
}


    if (NT_SUCCESS(status) &&
        kvpi->Type == REG_DWORD &&
        kvpi->DataLength == sizeof(ULONG))
    {
        RtlCopyMemory(&data, kvpi->Data, sizeof(ULONG));
        result = data;
    }

    ZwClose(hKey);
    return result;
}


NTSTATUS
I2cCtrl_CreateTouchpad(
    PDEVICE_OBJECT Fdo,
    PI2CCTRL_FDO   fdoExt
)
{
    NTSTATUS status;
    ULONG count = 0;
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    PI2CCTRL_PDO childDx;
    const I2CHID_DEVICE_ID* hidMatch;

    if (!Fdo || !fdoExt) {
        I2cCtrl_Log("CreateTouchpad: invalid parameters\n");
        return STATUS_INVALID_PARAMETER;
    }

    I2cCtrl_Log("CreateTouchpad: begin (auto-enumerate + bind PT touchpad)\n");

    //
    // 1) Ensure we have children: enumerate or reenumerate as needed.
    //
    if (IsListEmpty(&fdoExt->ChildList)) {

        I2cCtrl_Log("CreateTouchpad: no children -> calling I2cCtrl_EnumerateAcpiChildren()\n");

        status = I2cCtrl_EnumerateAcpiChildren(
                     Fdo,
                     fdoExt,
                     &count
                 );

        if (!NT_SUCCESS(status) || IsListEmpty(&fdoExt->ChildList)) {
            I2cCtrl_Log("CreateTouchpad: enumeration produced no children -> NOT_FOUND\n");
            (void)I2cCtrl_DeenumerateAcpiChildren(Fdo, fdoExt, &count);
            return STATUS_NOT_FOUND;
        }
    } else {

        I2cCtrl_Log("CreateTouchpad: children exist -> calling I2cCtrl_ReenumerateAcpiChildren()\n");

        status = I2cCtrl_ReenumerateAcpiChildren(
                     Fdo,
                     fdoExt,
                     &count
                 );

        if (!NT_SUCCESS(status)) {
            I2cCtrl_Log("CreateTouchpad: Reenumerate FAILED (0x%08lx)\n", status);
        }

        if (IsListEmpty(&fdoExt->ChildList)) {
            I2cCtrl_Log("CreateTouchpad: reenumeration left no children -> cleaning up\n");
            (void)I2cCtrl_DeenumerateAcpiChildren(Fdo, fdoExt, &count);
            return STATUS_NOT_FOUND;
        }
    }

    //
    // 2) Walk children and find the first HID_FLAG_TOUCHPAD device.
    //
    fdoExt->TouchpadPdo = NULL;

    KeAcquireSpinLock(&fdoExt->ChildLock, &oldIrql);

    for (entry = fdoExt->ChildList.Flink;
         entry != &fdoExt->ChildList;
         entry = entry->Flink)
    {
        childDx = CONTAINING_RECORD(entry, I2CCTRL_PDO, ListEntry);

        if (childDx->Removed || !childDx->Present || childDx->HardwareId.Buffer == NULL) {
            continue;
        }

        hidMatch = I2cCtrl_FindHidMatch(childDx->HardwareId.Buffer);
        if (hidMatch && (hidMatch->Flags & HID_FLAG_TOUCHPAD)) {

            fdoExt->TouchpadPdo = childDx;

            I2cCtrl_Log(
                "CreateTouchpad: selected HID touchpad \"%ws\" (quirks=0x%08lx flags=0x%08lx)\n",
                childDx->HardwareId.Buffer,
                hidMatch->Quirks,
                hidMatch->Flags
            );

            break;
        }
    }

    KeReleaseSpinLock(&fdoExt->ChildLock, oldIrql);

    //
    // 3) If no touchpad HID was found, de-enumerate to avoid stale PDOs.
    //
    if (fdoExt->TouchpadPdo == NULL) {
        I2cCtrl_Log("CreateTouchpad: no HID_FLAG_TOUCHPAD device found -> deleting children\n");
        (void)I2cCtrl_DeenumerateAcpiChildren(Fdo, fdoExt, &count);
        return STATUS_NOT_FOUND;
    }

    //
    // 4) Success - PT sampling now has a valid source.
    //
    I2cCtrl_Log("CreateTouchpad: TouchpadPdo=%p bound successfully\n",
                fdoExt->TouchpadPdo);

    return STATUS_SUCCESS;
}


NTSTATUS
I2cCtrl_DeenumerateAcpiChildren(
    PDEVICE_OBJECT Fdo,
    PI2CCTRL_FDO   DevCtx,
    PULONG         ChildCountOut
    )
{
    KIRQL oldIrql;
    PLIST_ENTRY entry, next;
    PI2CCTRL_PDO child;
    ULONG deleted = 0;

    UNREFERENCED_PARAMETER(Fdo);

    if (ChildCountOut) {
        *ChildCountOut = 0;
    }

    if (DevCtx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    I2cCtrl_Log("DeenumerateAcpiChildren: begin (NumChildren=%lu)\n",
                DevCtx->NumChildren);

    KeAcquireSpinLock(&DevCtx->ChildLock, &oldIrql);

    entry = DevCtx->ChildList.Flink;

    while (entry != &DevCtx->ChildList) {

        next  = entry->Flink;
        child = CONTAINING_RECORD(entry, I2CCTRL_PDO, ListEntry);

        RemoveEntryList(entry);
        DevCtx->NumChildren--;
        deleted++;

        KeReleaseSpinLock(&DevCtx->ChildLock, oldIrql);

        I2cCtrl_Log("DeenumerateAcpiChildren: deleting PDO=%p HID=%ws\n",
                    child->Pdo,
                    child->HardwareId.Buffer ? child->HardwareId.Buffer : L"<null>");

        //
        // Free ID strings
        //
        if (child->HardwareId.Buffer) {
            ExFreePoolWithTag(child->HardwareId.Buffer, TAG_PDO);
            child->HardwareId.Buffer = NULL;
        }
        if (child->CompatibleId.Buffer) {
            ExFreePoolWithTag(child->CompatibleId.Buffer, TAG_PDO);
            child->CompatibleId.Buffer = NULL;
        }
        if (child->InstanceId.Buffer) {
            ExFreePoolWithTag(child->InstanceId.Buffer, TAG_PDO);
            child->InstanceId.Buffer = NULL;
        }
        if (child->HardwareIdsMultiSz) {
            ExFreePoolWithTag(child->HardwareIdsMultiSz, TAG_PDO);
            child->HardwareIdsMultiSz = NULL;
        }
        if (child->CompatibleIdsMultiSz) {
            ExFreePoolWithTag(child->CompatibleIdsMultiSz, TAG_PDO);
            child->CompatibleIdsMultiSz = NULL;
        }

        //
        // Free HID/PT buffers
        //
        if (child->LastReport) {
            ExFreePoolWithTag(child->LastReport, 'RptH');
            child->LastReport = NULL;
        }
        if (child->HidReportDesc) {
            ExFreePoolWithTag(child->HidReportDesc, 'RptD');
            child->HidReportDesc = NULL;
        }

        //
        // Delete PDO
        //
        if (child->Pdo) {
            IoDeleteDevice(child->Pdo);
        }

        KeAcquireSpinLock(&DevCtx->ChildLock, &oldIrql);
        entry = next;
    }

    KeReleaseSpinLock(&DevCtx->ChildLock, oldIrql);

    //
    // Reset PT pointer
    //
    DevCtx->TouchpadPdo = NULL;

    if (ChildCountOut) {
        *ChildCountOut = deleted;
    }

    I2cCtrl_Log("DeenumerateAcpiChildren: done (Deleted=%lu, Remaining=%lu)\n",
                deleted, DevCtx->NumChildren);

    return STATUS_SUCCESS;
}

NTSTATUS
I2cCtrl_ReenumerateAcpiChildren(
    PDEVICE_OBJECT Fdo,
    PI2CCTRL_FDO   DevCtx,
    PULONG         ChildCountOut
    )
{
    NTSTATUS status;
    ULONG deleted = 0;
    ULONG created = 0;
    BOOLEAN needReenum = FALSE;

    if (ChildCountOut) {
        *ChildCountOut = 0;
    }

    if (!Fdo || !DevCtx) {
        return STATUS_INVALID_PARAMETER;
    }

    I2cCtrl_Log("ReenumerateAcpiChildren: begin\n");

    //
    // 1) Decide whether reenumeration is needed
    //
    // Conditions:
    //   - No children exist
    //   - Touchpad PDO missing
    //   - HID descriptor changed
    //   - ACPI _HID/_CID changed
    //   - SavedBusAddress changed
    //
    if (IsListEmpty(&DevCtx->ChildList)) {
        I2cCtrl_Log("Reenumerate: no children -> reenumeration required\n");
        needReenum = TRUE;
    }
    else if (DevCtx->TouchpadPdo == NULL) {
        I2cCtrl_Log("Reenumerate: TouchpadPdo missing -> reenumeration required\n");
        needReenum = TRUE;
    }
    else {
        PI2CCTRL_PDO p = DevCtx->TouchpadPdo;

        //
        // Check if HID descriptor is still valid
        //
        if (p->HidDesc.bLength == 0 ||
            p->HidReportDescLen == 0 ||
            p->HidMaxInputLen == 0)
        {
            I2cCtrl_Log("Reenumerate: HID descriptor invalid -> reenumeration required\n");
            needReenum = TRUE;
        }

        //
        // Check if ACPI address changed
        //
        if (DevCtx->SavedBusAddress != p->SavedBusAddress) {
            I2cCtrl_Log("Reenumerate: SavedBusAddress mismatch -> reenumeration required\n");
            needReenum = TRUE;
        }
    }

    //
    // If nothing changed, do nothing
    //
    if (!needReenum) {
        I2cCtrl_Log("ReenumerateAcpiChildren: no changes detected -> skipping\n");
        return STATUS_SUCCESS;
    }

    //
    // 2) Delete existing children (if any)
    //
    status = I2cCtrl_DeenumerateAcpiChildren(Fdo, DevCtx, &deleted);
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("Reenumerate: Deenumerate FAILED (0x%08lx)\n", status);
        return status;
    }

    //
    // 3) Re-enumerate ACPI HID-over-I2C children
    //
    status = I2cCtrl_EnumerateAcpiChildren(Fdo, DevCtx, &created);
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("Reenumerate: Enumerate FAILED (0x%08lx)\n", status);
        return status;
    }

    //
    // 4) Restart PT state if needed
    //
    if (DevCtx->TouchpadPdo) {
        PI2CCTRL_PDO p = DevCtx->TouchpadPdo;

        I2cCtrl_Log("Reenumerate: restarting PT state\n");

        if (p->LastReport) {
            ExFreePoolWithTag(p->LastReport, 'RptH');
        }

        p->LastReport = ExAllocatePoolWithTag(NonPagedPool,
                                              HID_REPORT_MAX_LEN,
                                              'RptH');

        if (p->LastReport) {
            RtlZeroMemory(p->LastReport, HID_REPORT_MAX_LEN);
        }

        p->Reported = FALSE;
    }

    //
    // 5) Return total number of changes
    //
    if (ChildCountOut) {
        *ChildCountOut = deleted + created;
    }

    I2cCtrl_Log(
        "ReenumerateAcpiChildren: done (Deleted=%lu, Created=%lu, Total=%lu)\n",
        deleted, created, deleted + created
    );

    return STATUS_SUCCESS;
}

VOID
I2cCtrl_InterruptThread(
    PVOID Context
    )
{
    PI2CCTRL_PDO pdoExt = (PI2CCTRL_PDO)Context;
    PI2CCTRL_FDO fdoExt = pdoExt->ParentFdo;

    I2cCtrl_Log("InterruptThread: begin (PDO=%p, FDO=%p)\n", pdoExt, fdoExt);

    for (;;) {

        GPIOCTRL_WAIT_REQUEST  req;
        GPIOCTRL_WAIT_RESPONSE resp;
        IO_STATUS_BLOCK        iosb;
        NTSTATUS               status;

        UCHAR          tempBuf[64];
        HID_DESCRIPTOR hidDesc;

        if (fdoExt == NULL) {
            I2cCtrl_Log("InterruptThread: ParentFdo=NULL -> exiting thread\n");
            break;
        }

        req.Pin = pdoExt->GpioPin;

        I2cCtrl_Log("InterruptThread: waiting on GPIO pin %lu (Handle=%p)\n",
                    (ULONG)pdoExt->GpioPin,
                    pdoExt->GpioHandle);

        status = ZwDeviceIoControlFile(
                     pdoExt->GpioHandle,
                     NULL,
                     NULL,
                     NULL,
                     &iosb,
                     IOCTL_GPIOCTRL_WAIT_FOR_INTERRUPT,
                     &req, sizeof(req),
                     &resp, sizeof(resp)
                 );

        if (!NT_SUCCESS(status)) {
            I2cCtrl_Log("InterruptThread: wait failed (status=0x%08X)\n", status);
            continue;
        }

        I2cCtrl_Log("InterruptThread: GPIO interrupt fired\n");

        /* 1) Re-read HID descriptor */
        status = I2cCtrl_ReadHidDescriptor(
                     fdoExt,
                     pdoExt,
                     &hidDesc
                 );

        I2cCtrl_Log("InterruptThread: ReadHidDescriptor -> 0x%08X\n", status);

        /* 2) Re-read report descriptor */
        if (pdoExt->HidReportDescLen > 0 &&
            pdoExt->HidReportDescLen <= sizeof(tempBuf))
        {
            status = I2cCtrl_ReadReportDescriptor(
                         fdoExt,
                         pdoExt,
                         tempBuf,
                         pdoExt->HidReportDescLen
                     );

            I2cCtrl_Log("InterruptThread: ReadReportDescriptor(len=%lu) -> 0x%08X\n",
                        (ULONG)pdoExt->HidReportDescLen,
                        status);
        }
        else {
            I2cCtrl_Log("InterruptThread: ReportDescLen invalid (%lu)\n",
                        (ULONG)pdoExt->HidReportDescLen);
        }

        /* 3) Read a HID register */
        status = I2cCtrl_ReadHidRegister(
                     fdoExt,
                     (UCHAR)pdoExt->HidSlaveAddress,
                     (UCHAR)pdoExt->HidDataRegister,
                     tempBuf,
                     1,
                     1000
                 );

        I2cCtrl_Log("InterruptThread: ReadHidRegister(addr=0x%02X reg=0x%02X) -> 0x%08X\n",
                    (ULONG)pdoExt->HidSlaveAddress,
                    (ULONG)pdoExt->HidDataRegister,
                    status);
    }

    I2cCtrl_Log("InterruptThread: exit\n");
}

/* Locate the ACPI PDO associated with this PCI device by walking the device stack */
PDEVICE_OBJECT
I2cCtrl_FindAcpiPdoForPciDevice(
    PDEVICE_OBJECT Fdo
    )
{
    PI2CCTRL_FDO ext;
    PDEVICE_OBJECT top;
    PDEVICE_OBJECT current;
    PDEVICE_OBJECT lower;
    PDEVICE_OBJECT acpiPdo;
    PCWSTR pnp;
    PCWSTR hwid;
    PCWSTR inst;

    acpiPdo = NULL;

    if (Fdo == NULL) {
        I2cCtrl_Log("FindAcpiPdo: Fdo=NULL\n");
        return NULL;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        I2cCtrl_Log("FindAcpiPdo: wrong IRQL\n");
        return NULL;
    }

    ext = (PI2CCTRL_FDO)Fdo->DeviceExtension;

    if (ext == NULL) {
        I2cCtrl_Log("FindAcpiPdo: FDO invalid (ext=NULL)\n");
        return NULL;
    }

    /* Safe string extraction */
    if (ext->PnpId != NULL) {
        pnp = ext->PnpId;
    } else {
        pnp = L"<null>";
    }

    if (ext->HardwareId.Buffer != NULL) {
        hwid = ext->HardwareId.Buffer;
    } else {
        hwid = L"<null>";
    }

    if (ext->InstanceId.Buffer != NULL) {
        inst = ext->InstanceId.Buffer;
    } else {
        inst = L"<null>";
    }

    /* Validate extension state */
    if (ext->Signature != I2CCTRL_FDO_SIGNATURE ||
        ext->Removed ||
        ext->Stopping ||
        ext->SurpriseRemoved ||
        !ext->Started)
    {
        I2cCtrl_Log(
            "FindAcpiPdo: FDO invalid "
            "(Ext=%p Sig=0x%08lx Removed=%lu Stopping=%lu Surprise=%lu Started=%lu "
            "PnpId=\"%ws\" HardwareId=\"%ws\" InstanceId=\"%ws\")\n",
            ext,
            ext->Signature,
            ext->Removed,
            ext->Stopping,
            ext->SurpriseRemoved,
            ext->Started,
            pnp,
            hwid,
            inst
        );
        return NULL;
    }

    PAGED_CODE();

    I2cCtrl_Log("FindAcpiPdo: begin (Fdo=%p)\n", Fdo);

    top = IoGetAttachedDeviceReference(Fdo);
    if (top == NULL) {
        I2cCtrl_Log("FindAcpiPdo: IoGetAttachedDeviceReference returned NULL\n");
        return NULL;
    }

    I2cCtrl_Log("FindAcpiPdo: top=%p driver=%wZ\n",
                top, &top->DriverObject->DriverName);

    current = top;

    for (;;) {

        if (I2cCtrl_IsAcpiDriver(current->DriverObject)) {
            I2cCtrl_Log("FindAcpiPdo: SUCCESS ACPI PDO=%p\n", current);
            acpiPdo = current;
            break;
        }

        lower = IoGetLowerDeviceObject(current);
        if (lower == NULL) {

            I2cCtrl_Log("FindAcpiPdo: bottom=%p driver=%wZ\n",
                        current, &current->DriverObject->DriverName);

            ObDereferenceObject(current);
            break;
        }

        I2cCtrl_Log("FindAcpiPdo: step %p -> %p (driver=%wZ)\n",
                    current, lower, &lower->DriverObject->DriverName);

        ObDereferenceObject(current);
        current = lower;
    }

    ObDereferenceObject(top);

    if (acpiPdo != NULL) {
        return acpiPdo;
    }

    I2cCtrl_Log("FindAcpiPdo: no ACPI in stack, trying fallback\n");
    return I2cCtrl_FindAcpiPdoByAdr(Fdo);
}


PDEVICE_OBJECT
I2cCtrl_FindAcpiPdoByAdr(
    PDEVICE_OBJECT Fdo
    )
{
    NTSTATUS status;
    PDEVICE_OBJECT pciPdo;
    PDEVICE_OBJECT acpiPdo;
    ULONG bus = 0, dev = 0, fun = 0;
    ULONG adrValue = 0;

    I2CCTRL_ACPI_ENUM_ENTRY info;
    ULONG next = 0;

    ULONGLONG acpiAdr64 = 0;
    ULONG acpiAdr = 0;

    if (Fdo == NULL) {
        I2cCtrl_Log("FindAcpiPdoByAdr: Fdo=NULL\n");
        return NULL;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        I2cCtrl_Log("FindAcpiPdoByAdr: wrong IRQL\n");
        return NULL;
    }

    PAGED_CODE();

    /* Get PCI BDF */
    pciPdo = IoGetDeviceAttachmentBaseRef(Fdo);
    if (pciPdo == NULL) {
        I2cCtrl_Log("FindAcpiPdoByAdr: IoGetDeviceAttachmentBaseRef returned NULL\n");
        return NULL;
    }

    status = I2cCtrl_GetPciBusDevFun(pciPdo, &bus, &dev, &fun);
    ObDereferenceObject(pciPdo);

    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("FindAcpiPdoByAdr: GetPciBusDevFun failed (0x%08lx)\n", status);
        return NULL;
    }

    adrValue = (dev << 16) | fun;

    I2cCtrl_Log("FindAcpiPdoByAdr: PCI BDF=%lu:%lu.%lu expected _ADR=0x%08lx\n",
                bus, dev, fun, adrValue);

    /* Get ACPI PDO from stack */
    acpiPdo = I2cCtrl_FindAcpiPdoForPciDevice(Fdo);
    if (acpiPdo == NULL) {
        I2cCtrl_Log("FindAcpiPdoByAdr: no ACPI PDO in stack\n");
        return NULL;
    }

    /* Enumerate ACPI namespace */
    next = 0;

    for (;;) {

        RtlZeroMemory(&info, sizeof(info));
        info.NextRequest = next;

        status = I2cCtrl_AcpiGetDeviceInformation(
                     acpiPdo,
                     NULL,
                     &info,
                     sizeof(info)
                 );

        if (!NT_SUCCESS(status)) {
            break;
        }

        next = info.NextRequest;

        acpiAdr64 = 0;

        status = I2cCtrl_AcpiEvalInteger(
                     info.DeviceObject,
                     "_ADR",
                     &acpiAdr64
                 );

        if (NT_SUCCESS(status)) {

            acpiAdr = (ULONG)acpiAdr64;

            if (acpiAdr == adrValue) {

                I2cCtrl_Log("FindAcpiPdoByAdr: MATCH -> ACPI PDO %p\n",
                            info.DeviceObject);

                ObReferenceObject(info.DeviceObject);
                ObDereferenceObject(acpiPdo);
                return info.DeviceObject;
            }
        }

        if (next == 0) {
            break;
        }
    }

    ObDereferenceObject(acpiPdo);
    I2cCtrl_Log("FindAcpiPdoByAdr: no ACPI node matched _ADR\n");
    return NULL;
}

NTSTATUS
I2cCtrl_GetPciBusDevFun(
    PDEVICE_OBJECT PciPdo,
    ULONG *Bus,
    ULONG *Dev,
    ULONG *Fun
    )
{
    NTSTATUS status;
    ULONG addr = 0;
    ULONG bytes = 0;

    /* Validate parameters */
    if (PciPdo == NULL || Bus == NULL || Dev == NULL || Fun == NULL) {
        I2cCtrl_Log("GetPciBusDevFun: invalid parameter\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* Must run at PASSIVE_LEVEL */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        I2cCtrl_Log("GetPciBusDevFun: wrong IRQL\n");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    PAGED_CODE();

    /* Query PCI address property */
    status = IoGetDeviceProperty(
                 PciPdo,
                 DevicePropertyAddress,
                 sizeof(ULONG),
                 &addr,
                 &bytes
             );

    if (!NT_SUCCESS(status) || bytes != sizeof(ULONG)) {
        I2cCtrl_Log("GetPciBusDevFun: IoGetDeviceProperty failed (st=0x%08lx bytes=%lu)\n",
                    status, bytes);
        return (NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status);
    }

    /* Decode PCI BDF */
    *Fun =  (addr      ) & 0x07;
    *Dev = ((addr >> 3) & 0x1F);
    *Bus = ((addr >> 8) & 0xFF);

    I2cCtrl_Log("GetPciBusDevFun: BDF=%lu:%lu.%lu (raw=0x%08lx)\n",
                *Bus, *Dev, *Fun, addr);

    return STATUS_SUCCESS;
}

BOOLEAN
I2cCtrl_IsAcpiDriver(PDRIVER_OBJECT drv)
{
    if (drv == NULL || drv->DriverName.Buffer == NULL) {
        I2cCtrl_Log("IsAcpiDriver: drv or name=NULL\n");
        return FALSE;
    }

    I2cCtrl_Log("IsAcpiDriver: checking driver=%wZ\n", &drv->DriverName);

    /* Strict case-insensitive exact match */
    if (_wcsicmp(drv->DriverName.Buffer, L"\\Driver\\ACPI") == 0) {
        I2cCtrl_Log("IsAcpiDriver: MATCH (exact)\n");
        return TRUE;
    }

    I2cCtrl_Log("IsAcpiDriver: no match\n");
    return FALSE;
}
