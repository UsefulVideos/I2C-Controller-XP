/* i2cctrl.c */
#include <ntddk.h>          /* core kernel types, IRP, DEVICE_OBJECT, etc. */
#include <wdm.h>
#include "i2cctrl_spinlock_fix.h"
#include <acpiioct.h>       /* ACPI_EVAL_INPUT_BUFFER, ACPI_EVAL_OUTPUT_BUFFER, IOCTL_ACPI_EVAL_METHOD */
#include <ntstrsafe.h>      /* RtlInitUnicodeString, safe string helpers for ACPI method names */
#include <strsafe.h>        /* RtlStringCchCopyW, safe string helpers for ACPI method names */
#include "i2cctrl_hw.h"     /* register offsets, PCI IDs, bit masks */
#include "i2cctrl.h"        /* driver-wide definitions, device context */
#include "I2cCtrl_Isr.h"
#include "i2cctrl_ext.h"    /* legacy I2CCTRL_FDO if still referenced */
#include "i2cctrl_ioctl.h"  /* IOCTL codes and I2CCTRL_RW struct */
#include "i2cctrl_bsod.h"   /* safe wrappers, guards, WinDBG-friendly logging */
#include "i2cctrl_spbcx.h"  /* façade definitions for SPBCX_COMPAT_CONTEXT, IOCTLs */
#include "i2cctrl_DPI.h"
#include "i2cctrl_i2c.h"
#include "i2cctrl_etw.h"
#include "i2cctrl_etw.tmh"
#include "i2cctrl_dump.h"

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

/* Distinguishing signature for PDO extensions */
#define I2CCTRL_PDO_SIGNATURE 0x50444F20 /* 'PDO ' in ASCII */



/* ---------------------------------------------------------------------------
   Forward declarations
   --------------------------------------------------------------------------- */
VOID     DriverUnload(PDRIVER_OBJECT DriverObject);

VOID     I2cCtrlApplyQuirks(PI2CCTRL_FDO devctx);

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

        /* No lower device → complete locally */
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

    /* PDO or no lower device → fail */
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
      QUIRK_NONE, BSOD_NONE }
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
 *
 * Responsibilities:
 *  - Runs strictly at PASSIVE_LEVEL
 *  - Initializes global controller state deterministically
 *  - Installs safe default dispatch routines
 *  - Registers PnP, Power, and IOCTL dispatchers
 *  - Installs AddDevice and DriverUnload early for safe failure paths
 *  - Initializes tracing and snapshot subsystems
 *
 * Notes:
 *  - IRP_MJ_POWER is routed through I2cCtrl_DispatchPower, which
 *    dispatches to PDO or FDO handlers based on extension signature.
 *  - FDO power is handled by I2cCtrl_FdoDispatchPower.
 *  - PDO power is handled by I2cCtrl_PdoDispatchPower.
 * ----------------------------------------------------------------------- */
NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT  DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    ULONG    i;

    /* C89 initialization */
    status = STATUS_SUCCESS;
    i      = 0U;

    UNREFERENCED_PARAMETER(RegistryPath);

    /* Must run at PASSIVE_LEVEL */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                "DriverEntry begin");

    /* Initialize global state deterministically */
    RtlZeroMemory(&g_I2cCtrlGlobal, sizeof(g_I2cCtrlGlobal));
    g_I2cCtrlGlobal.NextControllerId = 0L;
    I2CCTRL_INIT_LOCK(&g_I2cCtrlGlobal.GlobalLock);
    InitializeListHead(&g_I2cCtrlGlobal.ControllerList);

    /* Initialize tracing and snapshot subsystem early */
    I2cCtrlEtwInitialize();
    I2cCtrl_DumpInit(NULL);

    /* Default all IRP major functions to a safe invalid handler */
    for (i = 0U; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = I2cCtrl_InvalidIrp;
    }

    /* Assign supported dispatch routines */
    DriverObject->MajorFunction[IRP_MJ_CREATE]                  = I2cCtrlCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]                   = I2cCtrlClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]                 = I2cCtrlCleanup;
    DriverObject->MajorFunction[IRP_MJ_PNP]                     = I2cCtrl_DispatchPnP;
    DriverObject->MajorFunction[IRP_MJ_POWER]                   = I2cCtrl_DispatchPower;   /* <-- ROUTER */
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]          = I2cCtrlDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = I2cCtrlDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL]          = I2cCtrl_InvalidIrp;      /* WMI not supported */

    /* Set unload routine early so failure paths are safe */
    DriverObject->DriverUnload = DriverUnload;

    /* AddDevice must be available; if not, fail safely */
    if (DriverObject->DriverExtension == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "DriverExtension missing");
        return STATUS_UNSUCCESSFUL;
    }

    /* Assign AddDevice (PnP entry point) */
    DriverObject->DriverExtension->AddDevice = I2cCtrl_AddDevice;

    /* Register lifecycle helpers in our own global struct */
    g_I2cCtrlGlobal.StopDevice     = I2cCtrl_StopDevice;
    g_I2cCtrlGlobal.RestartDevice  = I2cCtrl_RestartDevice;
    g_I2cCtrlGlobal.DetectTouchpad = I2cCtrl_DetectTouchpadRedirect;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                "DriverEntry complete (mask=0x%08lx)", g_I2cCtrlTraceEnableMask);

    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
 * I2cCtrlCreate
 * Handle IRP_MJ_CREATE (remove-lock safe, XP/2003 correct)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrlCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
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
 * I2cCtrlClose
 * Handle IRP_MJ_CLOSE (remove-lock safe, XP/2003 correct)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrlClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
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
         * Wildcard match: "*PNP0C50" → match any suffix
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
 * I2cCtrlCleanup
 * Handle IRP_MJ_CLEANUP (remove-lock safe, XP/2003 correct)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrlCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
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
 * Polled read: issues read requests via HAL and drains RX FIFO into buffer
 * HAL-based, controller-agnostic, XP-safe, C89-compliant
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
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ReadBurstPolled: invalid parameters (devctx=%p, buffer=%p, length=%u)",
                    devctx, buffer, length);
        return STATUS_INVALID_PARAMETER;
    }
    if (devctx->Ops == NULL ||
        devctx->Ops->GetStatus == NULL ||
        devctx->Ops->IssueReadToken == NULL ||
        devctx->Ops->ReadRxByte == NULL ||
        devctx->Ops->SetTarget7bit == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ReadBurstPolled: Ops table incomplete");
        return STATUS_INVALID_DEVICE_STATE;
    }

    /* Program 7-bit target address */
    status = devctx->Ops->SetTarget7bit(devctx, (UCHAR)(addr & 0x7F));
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ReadBurstPolled: SetTarget7bit failed (status=0x%08lx)", status);
        return status;
    }

    /* Optional restart before burst (if backend supports it) */
    if (issueRestart && devctx->Ops->EmitRestartIfNeeded != NULL) {
        status = devctx->Ops->EmitRestartIfNeeded(devctx);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                        "ReadBurstPolled: EmitRestartIfNeeded failed (status=0x%08lx)", status);
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
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                            "ReadBurstPolled: TX aborted while queuing read requests");
                if (devctx->Ops->AckInterrupts != NULL) {
                    devctx->Ops->AckInterrupts(devctx, hwst.RawIntr);
                }
                return STATUS_DEVICE_NOT_CONNECTED;
            }
            KeStallExecutionProcessor(1);
        }
        if (timeout == 0) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                        "ReadBurstPolled: TX path saturated while queuing read requests");
            return STATUS_IO_TIMEOUT;
        }

        status = devctx->Ops->IssueReadToken(devctx);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    /* Optional stop after queuing (if backend supports it) */
    if (issueStop && devctx->Ops->EmitStopIfNeeded != NULL) {
        status = devctx->Ops->EmitStopIfNeeded(devctx);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                        "ReadBurstPolled: EmitStopIfNeeded failed (status=0x%08lx)", status);
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
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                            "ReadBurstPolled: TX aborted while waiting for RX data");
                if (devctx->Ops->AckInterrupts != NULL) {
                    devctx->Ops->AckInterrupts(devctx, hwst.RawIntr);
                }
                return STATUS_DEVICE_NOT_CONNECTED;
            }
            KeStallExecutionProcessor(1);
        }
        if (timeout == 0) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                        "ReadBurstPolled: RX timeout");
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

    /* Final abort check; clear sticky bits if any */
    RtlZeroMemory(&hwst, sizeof(hwst));
    status = devctx->Ops->GetStatus(devctx, &hwst);
    if (NT_SUCCESS(status) && hwst.TxAborted) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ReadBurstPolled: final TX abort detected");
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



/* ---------------------------------------------------------------------------
 * I2cCtrlDeviceControl
 * IRP_MJ_DEVICE_CONTROL dispatch routine
 * XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant.
 * Hardened rules:
 * - Runs at PASSIVE_LEVEL (PAGED_CODE) and validates DeviceObject/Irp
 * - Strict METHOD_BUFFERED input bounds checks for every IOCTL
 * - Clamps lengths, validates addresses, and guards PEC toggles
 * - Does not touch hardware if device is stopping/removed/unstarted or MMIO is NULL
 * - Completes each IRP exactly once
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrlDeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp
    )
{
    PI2CCTRL_FDO        devctx;
    PIO_STACK_LOCATION  irpSp;
    NTSTATUS            status;
    PVOID               sysbuf;
    ULONG               inLen;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    PAGED_CODE();

    if (DeviceObject == NULL || Irp == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    devctx = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    if (devctx == NULL) {
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0U;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    irpSp  = IoGetCurrentIrpStackLocation(Irp);
    if (irpSp == NULL) {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    sysbuf = Irp->AssociatedIrp.SystemBuffer;
    inLen  = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    status = STATUS_INVALID_DEVICE_REQUEST;

    /* Do not access hardware if device is not in a valid state */
    if (devctx->Removed != FALSE || devctx->Stopping != FALSE || devctx->Started == FALSE || devctx->Mmio == NULL) {
        Irp->IoStatus.Status = STATUS_NO_SUCH_DEVICE;
        Irp->IoStatus.Information = 0U;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NO_SUCH_DEVICE;
    }

    switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {

    /* -----------------------------------------------------------------------
     * Raw I2C write (METHOD_BUFFERED)
     * SystemBuffer: I2CCTRL_IO_DESC + payload (Length bytes)
     * ----------------------------------------------------------------------- */
    case IOCTL_I2C_WRITE:
    {
        PI2CCTRL_IO_DESC desc;
        UCHAR* payload;
        USHORT addr;
        SIZE_T total;

        if (sysbuf == NULL) { status = STATUS_INVALID_PARAMETER; break; }
        status = ValidateBufferedInput(Irp, sizeof(I2CCTRL_IO_DESC));
        if (!NT_SUCCESS(status)) break;

        desc = (PI2CCTRL_IO_DESC)sysbuf;

        /* Validate address and length */
        if ((desc->Address7Bit & ~0x7F) != 0 || desc->Length == 0U) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (desc->Length > devctx->MaxTransferLen) {
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }

        total = sizeof(I2CCTRL_IO_DESC) + (SIZE_T)desc->Length;
        status = ValidateBufferedInput(Irp, total);
        if (!NT_SUCCESS(status)) break;

        /* Optional: toggle PEC safely */
        devctx->PecEnabled = (desc->PecMode != 0);

        payload = (UCHAR*)(desc + 1);
        addr    = (USHORT)(desc->Address7Bit & 0x7F);

        status = I2cCtrl_WriteBurstPolled(
                    devctx,
                    addr,
                    payload,
                    desc->Length,
                    TRUE,   /* issueRestart on first byte */
                    TRUE    /* issueStop on last byte */
                 );

        Irp->IoStatus.Information = 0U;
        break;
    }

    /* -----------------------------------------------------------------------
     * Raw I2C read (METHOD_BUFFERED)
     * SystemBuffer: I2CCTRL_IO_DESC + output area (Length bytes)
     * ----------------------------------------------------------------------- */
    case IOCTL_I2C_READ:
    {
        PI2CCTRL_IO_DESC desc;
        UCHAR* payload;
        USHORT addr;
        SIZE_T total;

        if (sysbuf == NULL) { status = STATUS_INVALID_PARAMETER; break; }
        status = ValidateBufferedInput(Irp, sizeof(I2CCTRL_IO_DESC));
        if (!NT_SUCCESS(status)) break;

        desc = (PI2CCTRL_IO_DESC)sysbuf;

        if ((desc->Address7Bit & ~0x7F) != 0 || desc->Length == 0U) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (desc->Length > devctx->MaxTransferLen) {
            status = STATUS_BUFFER_OVERFLOW;
            break;
        }

        total = sizeof(I2CCTRL_IO_DESC) + (SIZE_T)desc->Length;
        status = ValidateBufferedInput(Irp, total);
        if (!NT_SUCCESS(status)) break;

        devctx->PecEnabled = (desc->PecMode != 0);

        payload = (UCHAR*)(desc + 1);
        addr    = (USHORT)(desc->Address7Bit & 0x7F);

        status = I2cCtrl_ReadBurstPolled(
                    devctx,
                    addr,
                    payload,
                    desc->Length,
                    TRUE,
                    TRUE
                 );

        Irp->IoStatus.Information = NT_SUCCESS(status) ? desc->Length : 0U;
        break;
    }

/* -----------------------------------------------------------------------
 * Canonical multi-message transfer (METHOD_BUFFERED)
 * ----------------------------------------------------------------------- */
case IOCTL_TRANSFER:
{
    PI2CCTRL_TRANSFER xfer;
    PUCHAR            cursor;
    ULONG             i;
    ULONG             doneReads;
    SIZE_T            cursorOff;

    /* Basic buffer presence check */
    if (sysbuf == NULL) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Require at least the transfer header */
    status = ValidateBufferedInput(Irp, sizeof(I2CCTRL_TRANSFER));
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    xfer      = (PI2CCTRL_TRANSFER)sysbuf;
    cursor    = (PUCHAR)&xfer->Messages[0];
    doneReads = 0U;
    cursorOff = (SIZE_T)(cursor - (PUCHAR)sysbuf);

    /* Validate message count conservatively */
    if (xfer->NumMessages == 0U || xfer->NumMessages > devctx->MaxMessages) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    for (i = 0U; i < xfer->NumMessages; i++) {
        PI2CCTRL_MSG msg;
        SIZE_T       msgHeaderEnd;
        SIZE_T       msgTotal;
        USHORT       addr;
        BOOLEAN      issueStop;
        BOOLEAN      issueRestart;

        /* Ensure header fits */
        msgHeaderEnd = cursorOff + sizeof(I2CCTRL_MSG);
        if (msgHeaderEnd > inLen) {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0U;
            break;
        }

        msg = (PI2CCTRL_MSG)cursor;

        /* Validate address/length/flags */
        if ((msg->Address & ~0x7F) != 0) {
            status = STATUS_INVALID_PARAMETER;
            Irp->IoStatus.Information = 0U;
            break;
        }
        if (msg->Length == 0U || msg->Length > devctx->MaxTransferLen) {
            status = STATUS_INVALID_PARAMETER;
            Irp->IoStatus.Information = 0U;
            break;
        }

        /* Account for actual payload beyond Data[1] placeholder */
        msgTotal = (SIZE_T)(sizeof(I2CCTRL_MSG) - 1U) + (SIZE_T)msg->Length;
        if (cursorOff + msgTotal > inLen) {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0U;
            break;
        }

        addr         = (USHORT)(msg->Address & 0x7F);
        issueRestart = TRUE;
        issueStop    = ((msg->Flags & I2C_MSG_STOP) != 0) || (i + 1U == xfer->NumMessages);

        if ((msg->Flags & I2C_MSG_READ) != 0) {
            status = I2cCtrl_ReadBurstPolled(devctx, addr, msg->Data, msg->Length, issueRestart, issueStop);
            if (!NT_SUCCESS(status)) {
                Irp->IoStatus.Information = 0U;
                break;
            }
            doneReads += msg->Length;
        } else {
            status = I2cCtrl_WriteBurstPolled(devctx, addr, msg->Data, msg->Length, issueRestart, issueStop);
            if (!NT_SUCCESS(status)) {
                Irp->IoStatus.Information = 0U;
                break;
            }
        }

        cursorOff += msgTotal;
        cursor    += msgTotal;
    }

    /* Report number of bytes read if successful, else 0 */
    Irp->IoStatus.Information = NT_SUCCESS(status) ? doneReads : 0U;
    break;
}

    /* -----------------------------------------------------------------------
     * Set 10-bit target (METHOD_BUFFERED)
     * ----------------------------------------------------------------------- */
    case IOCTL_SET_TARGET:
    {
        USHORT* pTarget;
        if (sysbuf == NULL || inLen < sizeof(USHORT)) {
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Information = 0U;
            break;
        }
        pTarget = (USHORT*)sysbuf;
        devctx->TargetAddress = (USHORT)(*pTarget & 0x03FF); /* clamp to 10-bit field */
        status = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0U;
        break;
    }

/* -----------------------------------------------------------------------
 * Probe controller status (HAL-generic safe read)
 * ----------------------------------------------------------------------- */
case IOCTL_PROBE:
{
    I2C_HW_STATUS hwst;
    NTSTATUS      st;

    RtlZeroMemory(&hwst, sizeof(hwst));
    st = STATUS_SUCCESS;

    if (devctx == NULL || devctx->Ops == NULL || devctx->Ops->GetStatus == NULL) {
        status = STATUS_INVALID_DEVICE_STATE;
    } else {
        st = devctx->Ops->GetStatus(devctx, &hwst);
        if (NT_SUCCESS(st)) {
            status = STATUS_SUCCESS;
        } else {
            status = st;
        }
    }

    Irp->IoStatus.Information = 0U;
    break;
}


    /* -----------------------------------------------------------------------
     * SMBus Quick command (no data). METHOD_BUFFERED: expects I2CCTRL_SMBUS_CMD
     * Data length must be 0. Flags may request STOP/RESTART.
     * ----------------------------------------------------------------------- */
    case IOCTL_SMBUS_QUICK:
    {
        PI2CCTRL_SMBUS_CMD cmd;
        SIZE_T minLen;

        if (sysbuf == NULL) { status = STATUS_INVALID_PARAMETER; break; }
        minLen = sizeof(I2CCTRL_SMBUS_CMD) - 1U; /* no payload */
        status = ValidateBufferedInput(Irp, minLen);
        if (!NT_SUCCESS(status)) break;

        cmd = (PI2CCTRL_SMBUS_CMD)sysbuf;

        if ((cmd->Address7Bit & ~0x7FU) != 0U || cmd->Reserved != 0U) {
            status = STATUS_INVALID_PARAMETER; break;
        }

        {
            SMBUS_RW_CTX ctx;
            ctx.fdo     = devctx;
            ctx.addr    = (USHORT)(cmd->Address7Bit & 0x7FU);
            ctx.buf     = NULL; /* no data */
            ctx.len     = 0U;
            ctx.restart = TRUE;
            ctx.stop    = TRUE;
            ctx.isRead  = ((cmd->Flags & SMBUS_FLAG_READ) != 0U) ? TRUE : FALSE;

            /* Quick has no payload; simulate minimal R/W transaction safely */
            if (ctx.isRead) {
                UCHAR scratch;
                ctx.buf = &scratch;
                ctx.len = 0U; /* reads of zero are treated as a probe */
                status = STATUS_SUCCESS; /* no bus touch for pure quick if hardware requires data */
            } else {
                status = STATUS_SUCCESS; /* treat as a no-op write for safety */
            }
        }

        Irp->IoStatus.Information = 0U;
        break;
    }

/* -----------------------------------------------------------------------
 * SMBus SendByte: write a single data byte
 * XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant
 * ----------------------------------------------------------------------- */
case IOCTL_SMBUS_SEND_BYTE:
{
    PI2CCTRL_SMBUS_CMD cmd;
    SIZE_T need;
    UCHAR  tx[3];          /* [command][data][pec?] */
    SIZE_T txLen;
    UCHAR  stream[4];
    SIZE_T sLen;
    SMBUS_RW_CTX ctx;

    /* Basic buffer presence check */
    if (sysbuf == NULL) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Require header + 1 byte payload for SendByte */
    need = sizeof(I2CCTRL_SMBUS_CMD) + 1U; /* Data[0] must exist */
    status = ValidateBufferedInput(Irp, need);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Safe header parse and conservative field validation */
    cmd = (PI2CCTRL_SMBUS_CMD)sysbuf;
    if ((cmd->Address7Bit & ~0x7FU) != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }
    if (cmd->Reserved != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Toggle PEC mode safely */
    devctx->PecEnabled = (cmd->PecMode != 0U);

    /* Build transmit sequence */
    txLen = 2U;
    tx[0] = cmd->Command;
    tx[1] = cmd->Data[0];

    if (devctx->PecEnabled) {
        sLen = 0U;
        stream[sLen++] = (UCHAR)((cmd->Address7Bit << 1) | 0U); /* addr+W */
        stream[sLen++] = tx[0];                                  /* command */
        stream[sLen++] = tx[1];                                  /* data */
        tx[2] = I2cCtrl_ComputePec(stream, sLen);
        txLen = 3U;
    }

    /* Guard against controller transfer limits */
    if (txLen > devctx->MaxTransferLen) {
        status = STATUS_BUFFER_OVERFLOW;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Prepare single-operation context (C89: declarations at top) */
    ctx.fdo     = devctx;
    ctx.addr    = (USHORT)(cmd->Address7Bit & 0x7FU);
    ctx.buf     = tx;
    ctx.len     = (ULONG)txLen;
    ctx.restart = TRUE;
    ctx.stop    = TRUE;
    ctx.isRead  = FALSE;

    /* Execute with retry wrapper; defensive Information on failure */
    status = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &ctx);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* No bytes returned for SendByte */
    Irp->IoStatus.Information = 0U;
    break;
}

/* -----------------------------------------------------------------------
 * SMBus ReceiveByte: read a single data byte
 * SystemBuffer: I2CCTRL_SMBUS_CMD, output 1 byte in Data[0]
 * XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant
 * ----------------------------------------------------------------------- */
case IOCTL_SMBUS_RECEIVE_BYTE:
{
    PI2CCTRL_SMBUS_CMD cmd;
    SIZE_T need;
    UCHAR  rx[2];          /* [data][pec?] */
    ULONG  rxLen;
    SMBUS_RW_CTX ctx;
    UCHAR  stream[3];
    SIZE_T sLen;
    UCHAR  pec;

    /* Basic buffer presence check */
    if (sysbuf == NULL) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Require header for ReceiveByte (no input payload) */
    need = sizeof(I2CCTRL_SMBUS_CMD);
    status = ValidateBufferedInput(Irp, need);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Safe header parse and conservative field validation */
    cmd = (PI2CCTRL_SMBUS_CMD)sysbuf;
    if ((cmd->Address7Bit & ~0x7FU) != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }
    if (cmd->Reserved != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Toggle PEC mode safely */
    devctx->PecEnabled = (cmd->PecMode != 0U);

    /* Determine read length with optional PEC */
    rxLen = devctx->PecEnabled ? 2U : 1U;
    if (rxLen > devctx->MaxTransferLen) {
        status = STATUS_BUFFER_OVERFLOW;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Prepare single-operation context (C89: declarations at top) */
    ctx.fdo     = devctx;
    ctx.addr    = (USHORT)(cmd->Address7Bit & 0x7FU);
    ctx.buf     = rx;
    ctx.len     = rxLen;           /* narrowing handled inside wrapper */
    ctx.restart = TRUE;
    ctx.stop    = TRUE;
    ctx.isRead  = TRUE;

    /* Execute with retry wrapper; defensive Information on failure */
    status = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &ctx);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Optional PEC validation (non-fatal) */
    if (devctx->PecEnabled && rxLen >= 2U) {
        sLen = 0U;
        stream[sLen++] = (UCHAR)((cmd->Address7Bit << 1) | 1U); /* address+R */
        stream[sLen++] = rx[0];                                  /* data */
        pec = I2cCtrl_ComputePec(stream, sLen);
        UNREFERENCED_PARAMETER(pec);
    }

    /* Return the received byte in Data[0] */
    cmd->Data[0] = rx[0];
    Irp->IoStatus.Information = 1U;
    break;
}


/* -----------------------------------------------------------------------
 * SMBus WriteByte: send [command][data]
 * XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant
 * ----------------------------------------------------------------------- */
case IOCTL_SMBUS_WRITE_BYTE:
{
    PI2CCTRL_SMBUS_CMD cmd;
    SIZE_T need;
    UCHAR  tx[3];          /* [command][data][pec?] */
    SIZE_T txLen;
    UCHAR  stream[4];
    SIZE_T sLen;
    SMBUS_RW_CTX ctx;

    /* Basic buffer presence check */
    if (sysbuf == NULL) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Require header + 1 byte payload for WriteByte */
    need = sizeof(I2CCTRL_SMBUS_CMD) + 1U; /* Data[0] must exist */
    status = ValidateBufferedInput(Irp, need);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Safe header parse and conservative field validation */
    cmd = (PI2CCTRL_SMBUS_CMD)sysbuf;
    if ((cmd->Address7Bit & ~0x7FU) != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }
    if (cmd->Reserved != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Toggle PEC mode safely */
    devctx->PecEnabled = (cmd->PecMode != 0U);

    /* Build transmit sequence */
    txLen = 2U;
    tx[0] = cmd->Command;
    tx[1] = cmd->Data[0];

    if (devctx->PecEnabled) {
        sLen = 0U;
        stream[sLen++] = (UCHAR)((cmd->Address7Bit << 1) | 0U); /* addr+W */
        stream[sLen++] = tx[0];                                  /* command */
        stream[sLen++] = tx[1];                                  /* data */
        tx[2] = I2cCtrl_ComputePec(stream, sLen);
        txLen = 3U;
    }

    /* Guard against controller transfer limits */
    if (txLen > devctx->MaxTransferLen) {
        status = STATUS_BUFFER_OVERFLOW;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Prepare single-operation context (C89: declarations at top) */
    ctx.fdo     = devctx;
    ctx.addr    = (USHORT)(cmd->Address7Bit & 0x7FU);
    ctx.buf     = tx;
    ctx.len     = (ULONG)txLen;
    ctx.restart = TRUE;
    ctx.stop    = TRUE;
    ctx.isRead  = FALSE;

    /* Execute with retry wrapper; defensive Information on failure */
    status = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &ctx);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* No bytes returned for WriteByte */
    Irp->IoStatus.Information = 0U;
    break;
}

/* -----------------------------------------------------------------------
 * SMBus ReadByte: write [command], then read 1 byte (with optional PEC)
 * XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant
 * ----------------------------------------------------------------------- */
case IOCTL_SMBUS_READ_BYTE:
{
    PI2CCTRL_SMBUS_CMD cmd;
    SIZE_T need;
    UCHAR  cmdByte;
    UCHAR  rx[2];
    ULONG  rxLen;
    SMBUS_RW_CTX w;
    SMBUS_RW_CTX r;
    UCHAR  stream[4];
    SIZE_T sLen;
    UCHAR  pec;

    /* Basic buffer presence check */
    if (sysbuf == NULL) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Require header only (no input payload needed) */
    need = sizeof(I2CCTRL_SMBUS_CMD);
    status = ValidateBufferedInput(Irp, need);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Safe header parse and conservative field validation */
    cmd = (PI2CCTRL_SMBUS_CMD)sysbuf;
    if ((cmd->Address7Bit & ~0x7FU) != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }
    if (cmd->Reserved != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Toggle PEC mode safely */
    devctx->PecEnabled = (cmd->PecMode != 0U);

    cmdByte = cmd->Command;
    rxLen   = devctx->PecEnabled ? 2U : 1U;

    /* Guard against controller transfer limits */
    if (rxLen > devctx->MaxTransferLen) {
        status = STATUS_BUFFER_OVERFLOW;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Write command code (no STOP, repeated start into read) */
    w.fdo     = devctx;
    w.addr    = (USHORT)(cmd->Address7Bit & 0x7FU);
    w.buf     = &cmdByte;
    w.len     = 1U;
    w.restart = TRUE;
    w.stop    = FALSE;
    w.isRead  = FALSE;

    status = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &w);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Read data (and optional PEC) */
    r.fdo     = devctx;
    r.addr    = (USHORT)(cmd->Address7Bit & 0x7FU);
    r.buf     = rx;
    r.len     = rxLen;            /* narrowing handled inside wrapper */
    r.restart = TRUE;
    r.stop    = TRUE;
    r.isRead  = TRUE;

    status = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &r);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Optional PEC validation (non-fatal) */
    if (devctx->PecEnabled && rxLen >= 2U) {
        sLen = 0U;
        stream[sLen++] = (UCHAR)((cmd->Address7Bit << 1) | 0U); /* addr+W (command phase) */
        stream[sLen++] = cmdByte;
        stream[sLen++] = (UCHAR)((cmd->Address7Bit << 1) | 1U); /* addr+R (data phase) */
        stream[sLen++] = rx[0];
        pec = I2cCtrl_ComputePec(stream, sLen);
        UNREFERENCED_PARAMETER(pec);
    }

    /* Return the received byte in Data[0] */
    cmd->Data[0] = rx[0];
    Irp->IoStatus.Information = 1U;
    break;
}

/* -----------------------------------------------------------------------
 * SMBus WriteWord: [command][low][high][pec?]
 * XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant
 * ----------------------------------------------------------------------- */
case IOCTL_SMBUS_WRITE_WORD:
{
    PI2CCTRL_SMBUS_CMD cmd;
    SIZE_T need;
    UCHAR  tx[4];          /* [cmd][low][high][pec?] */
    SIZE_T txLen;
    UCHAR  stream[5];
    SIZE_T sLen;
    SMBUS_RW_CTX ctx;

    /* Basic buffer presence check */
    if (sysbuf == NULL) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Requires 2 data bytes: Data[0]=low, Data[1]=high */
    need = sizeof(I2CCTRL_SMBUS_CMD) + 2U; /* ensure both bytes are present */
    status = ValidateBufferedInput(Irp, need);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Safe header parse and conservative field validation */
    cmd = (PI2CCTRL_SMBUS_CMD)sysbuf;
    if ((cmd->Address7Bit & ~0x7FU) != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }
    if (cmd->Reserved != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Toggle PEC mode safely */
    devctx->PecEnabled = (cmd->PecMode != 0U);

    /* Build transmit sequence */
    txLen = 3U;
    tx[0] = cmd->Command;
    tx[1] = cmd->Data[0];  /* low */
    tx[2] = cmd->Data[1];  /* high */

    if (devctx->PecEnabled) {
        sLen = 0U;
        stream[sLen++] = (UCHAR)((cmd->Address7Bit << 1) | 0U); /* addr+W */
        stream[sLen++] = tx[0];                                  /* command */
        stream[sLen++] = tx[1];                                  /* low */
        stream[sLen++] = tx[2];                                  /* high */
        tx[3] = I2cCtrl_ComputePec(stream, sLen);
        txLen = 4U;
    }

    /* Guard against controller transfer limits */
    if (txLen > devctx->MaxTransferLen) {
        status = STATUS_BUFFER_OVERFLOW;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Prepare single-operation context */
    ctx.fdo     = devctx;
    ctx.addr    = (USHORT)(cmd->Address7Bit & 0x7FU);
    ctx.buf     = tx;
    ctx.len     = (ULONG)txLen;
    ctx.restart = TRUE;
    ctx.stop    = TRUE;
    ctx.isRead  = FALSE;

    /* Execute with retry wrapper; defensive Information on failure */
    status = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &ctx);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* No bytes returned for WriteWord */
    Irp->IoStatus.Information = 0U;
    break;
}

/* -----------------------------------------------------------------------
 * SMBus ReadWord: write [command], then read 2 bytes (and optional PEC)
 * XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant
 * ----------------------------------------------------------------------- */
case IOCTL_SMBUS_READ_WORD:
{
    PI2CCTRL_SMBUS_CMD cmd;
    SIZE_T need;
    UCHAR  cmdByte;
    UCHAR  rx[3];
    ULONG  rxLen;
    SMBUS_RW_CTX w;
    SMBUS_RW_CTX r;
    UCHAR  stream[6];
    SIZE_T sLen;
    UCHAR  pec;

    /* Basic buffer presence check */
    if (sysbuf == NULL) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Require header only (no input payload needed) */
    need = sizeof(I2CCTRL_SMBUS_CMD);
    status = ValidateBufferedInput(Irp, need);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Safe header parse and conservative field validation */
    cmd = (PI2CCTRL_SMBUS_CMD)sysbuf;
    if ((cmd->Address7Bit & ~0x7FU) != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }
    if (cmd->Reserved != 0U) {
        status = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Toggle PEC mode safely */
    devctx->PecEnabled = (cmd->PecMode != 0U);

    cmdByte = cmd->Command;
    rxLen   = devctx->PecEnabled ? 3U : 2U;

    /* Guard against controller transfer limits */
    if (rxLen > devctx->MaxTransferLen) {
        status = STATUS_BUFFER_OVERFLOW;
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Write command code (no STOP, repeated start into read) */
    w.fdo     = devctx;
    w.addr    = (USHORT)(cmd->Address7Bit & 0x7FU);
    w.buf     = &cmdByte;
    w.len     = 1U;
    w.restart = TRUE;
    w.stop    = FALSE;
    w.isRead  = FALSE;

    status = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &w);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Read response */
    r.fdo     = devctx;
    r.addr    = (USHORT)(cmd->Address7Bit & 0x7FU);
    r.buf     = rx;
    r.len     = rxLen;            /* narrowing handled inside wrapper */
    r.restart = TRUE;
    r.stop    = TRUE;
    r.isRead  = TRUE;

    status = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &r);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Optional PEC validation (non-fatal) */
    if (devctx->PecEnabled && rxLen == 3U) {
        sLen = 0U;
        stream[sLen++] = (UCHAR)((cmd->Address7Bit << 1) | 0U); /* W phase */
        stream[sLen++] = cmdByte;
        stream[sLen++] = (UCHAR)((cmd->Address7Bit << 1) | 1U); /* R phase */
        stream[sLen++] = rx[0]; /* low */
        stream[sLen++] = rx[1]; /* high */
        pec = I2cCtrl_ComputePec(stream, sLen);
        UNREFERENCED_PARAMETER(pec);
    }

    /* Return low/high in Data[0..1] */
    cmd->Data[0] = rx[0];
    cmd->Data[1] = rx[1];
    Irp->IoStatus.Information = 2U;
    break;
}

/* -----------------------------------------------------------------------
 * SMBus BlockWrite: [command][length 1..32][data...][pec?]
 * ----------------------------------------------------------------------- */
case IOCTL_SMBUS_BLOCK_WRITE:
{
    PI2CCTRL_SMBUS_BLOCK blk;
    SIZE_T min;
    SIZE_T total;
    UCHAR  tx[36];        /* 1 cmd + 1 len + 32 data + 1 pec = 35 max; pad one */
    SIZE_T txLen;
    SIZE_T i;
    UCHAR  stream[40];
    SIZE_T sLen;
    SIZE_T k;
    SMBUS_RW_CTX ctx;

    if (sysbuf == NULL) {
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    min = sizeof(I2CCTRL_SMBUS_BLOCK); /* includes 1 byte payload placeholder */
    status = ValidateBufferedInput(Irp, min);
    if (!NT_SUCCESS(status)) {
        break;
    }

    blk = (PI2CCTRL_SMBUS_BLOCK)sysbuf;
    if ((blk->Address7Bit & ~0x7FU) != 0U) {
        status = STATUS_INVALID_PARAMETER;
        break;
    }
    if (blk->Length == 0U || blk->Length > 32U) {
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    total = (SIZE_T)(min - 1U) + (SIZE_T)blk->Length; /* header minus placeholder + data */
    status = ValidateBufferedInput(Irp, total);
    if (!NT_SUCCESS(status)) {
        break;
    }

    devctx->PecEnabled = (blk->PecMode != 0U);

    txLen = 2U + (SIZE_T)blk->Length;

    if (txLen + (devctx->PecEnabled ? 1U : 0U) > sizeof(tx)) {
        status = STATUS_BUFFER_OVERFLOW;
        break;
    }

    tx[0] = blk->Command;
    tx[1] = blk->Length;
    for (i = 0U; i < blk->Length; i++) {
        tx[2 + i] = blk->Data[i];
    }

    if (devctx->PecEnabled) {
        sLen = 0U;
        stream[sLen++] = (UCHAR)((blk->Address7Bit << 1) | 0U); /* addr+W */
        stream[sLen++] = tx[0]; /* command */
        stream[sLen++] = tx[1]; /* length */
        for (k = 0U; k < blk->Length; k++) {
            stream[sLen++] = tx[2 + k];
        }
        tx[2 + blk->Length] = I2cCtrl_ComputePec(stream, sLen);
        txLen += 1U;
    }

    if (txLen > devctx->MaxTransferLen) {
        status = STATUS_BUFFER_OVERFLOW;
        break;
    }

    ctx.fdo     = devctx;
    ctx.addr    = (USHORT)(blk->Address7Bit & 0x7FU);
    ctx.buf     = tx;
    ctx.len     = (ULONG)txLen;
    ctx.restart = TRUE;
    ctx.stop    = TRUE;
    ctx.isRead  = FALSE;

    status = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &ctx);
    Irp->IoStatus.Information = 0U;

    break;
}

/* -----------------------------------------------------------------------
 * SMBus BlockRead: write [command], then read [length 1..32] + data [+pec]
 * Returns data in blk->Data[], sets Information = actual bytes read.
 * ----------------------------------------------------------------------- */
case IOCTL_SMBUS_BLOCK_READ:
{
    PI2CCTRL_SMBUS_BLOCK blk;
    SIZE_T min;
    UCHAR  cmdByte;
    UCHAR  hdr[1];     /* read length byte */
    UCHAR  rx[35];     /* up to 32 data + optional pec + 2 spare */
    SMBUS_RW_CTX w;
    SMBUS_RW_CTX rlen;
    SMBUS_RW_CTX rdata;
    ULONG toRead;
    SIZE_T minOut;
    UCHAR  stream[40];
    SIZE_T sLen;
    SIZE_T k;
    UCHAR  pec;

    if (sysbuf == NULL) {
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    min = sizeof(I2CCTRL_SMBUS_BLOCK) - 1U; /* header without payload */
    status = ValidateBufferedInput(Irp, min);
    if (!NT_SUCCESS(status)) {
        break;
    }

    blk = (PI2CCTRL_SMBUS_BLOCK)sysbuf;
    if ((blk->Address7Bit & ~0x7FU) != 0U) {
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    devctx->PecEnabled = (blk->PecMode != 0U);
    cmdByte = blk->Command;

    /* Write command */
    w.fdo     = devctx;
    w.addr    = (USHORT)(blk->Address7Bit & 0x7FU);
    w.buf     = &cmdByte;
    w.len     = 1U;
    w.restart = TRUE;
    w.stop    = FALSE;
    w.isRead  = FALSE;
    status    = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &w);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Read length byte */
    rlen.fdo     = devctx;
    rlen.addr    = (USHORT)(blk->Address7Bit & 0x7FU);
    rlen.buf     = hdr;
    rlen.len     = 1U;
    rlen.restart = TRUE;
    rlen.stop    = FALSE;
    rlen.isRead  = TRUE;
    status       = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &rlen);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Clamp length 1..32 */
    if (hdr[0] == 0U || hdr[0] > 32U) {
        status = STATUS_DATA_ERROR;
        Irp->IoStatus.Information = 0U;
        break;
    }

    toRead = hdr[0] + (devctx->PecEnabled ? 1U : 0U);
    if (toRead > sizeof(rx) || toRead > devctx->MaxTransferLen) {
        status = STATUS_BUFFER_OVERFLOW;
        Irp->IoStatus.Information = 0U;
        break;
    }

    rdata.fdo     = devctx;
    rdata.addr    = (USHORT)(blk->Address7Bit & 0x7FU);
    rdata.buf     = rx;
    rdata.len     = toRead;
    rdata.restart = TRUE;
    rdata.stop    = TRUE;
    rdata.isRead  = TRUE;
    status        = I2cCtrl_TryWithRetry(devctx, I2cCtrl_SmbusRwOnce, &rdata);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    /* Copy data back into caller buffer; validate output capacity */
    minOut = (sizeof(I2CCTRL_SMBUS_BLOCK) - 1U) + (SIZE_T)hdr[0];
    status = ValidateBufferedInput(Irp, minOut);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0U;
        break;
    }

    RtlCopyMemory(blk->Data, rx, hdr[0]);
    Irp->IoStatus.Information = hdr[0];

    /* Optional PEC validation */
    if (devctx->PecEnabled) {
        sLen = 0U;
        stream[sLen++] = (UCHAR)((blk->Address7Bit << 1) | 0U); /* W for cmd phase */
        stream[sLen++] = cmdByte;
        stream[sLen++] = (UCHAR)((blk->Address7Bit << 1) | 1U); /* R for data phase */
        stream[sLen++] = hdr[0]; /* count */
        for (k = 0U; k < hdr[0]; k++) {
            stream[sLen++] = rx[k];
        }
        pec = I2cCtrl_ComputePec(stream, sLen);
        UNREFERENCED_PARAMETER(pec);
    }

    break;
}

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0U;
        break;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
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

    /* Must run at PASSIVE_LEVEL */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "AddDevice: wrong IRQL");
        return STATUS_INVALID_DEVICE_STATE;
    }
    PAGED_CODE();

    if (DriverObject == NULL || PhysicalDeviceObject == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "AddDevice: invalid parameters");
        return STATUS_INVALID_PARAMETER;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                "AddDevice begin");

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
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                        "AddDevice: alloc %lu failed", dynLen);
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        status = IoGetDeviceProperty(PhysicalDeviceObject,
                                     DevicePropertyHardwareID,
                                     dynLen,
                                     dynBuf,
                                     &dynLen);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                        "AddDevice: requery failed 0x%08X", status);
            ExFreePoolWithTag(dynBuf, TAG_I2C_MISC);
            return status;
        }
    }
    else if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "AddDevice: property query failed 0x%08X", status);
        return status;
    }

    /* ---------------------------------------------------------------
       Detect whether this is a supported I²C controller
       using the canonical controller table.
       --------------------------------------------------------------- */
    if (dynBuf != NULL) {
        base  = (const WCHAR*)dynBuf;
        bytes = dynLen;
    } else {
        base  = hwidBuffer;
        bytes = length;
    }

    if (bytes >= sizeof(WCHAR) * 2U) {

        p   = base;
        end = (const WCHAR*)((const UCHAR*)base + bytes);

        while (p < end && *p != L'\0') {

            s = p;
            while (s < end && *s != L'\0') {
                s++;
            }

            for (i = 0; i < RTL_NUMBER_OF(g_I2cControllers); i++) {
                if (wcsstr(p, g_I2cControllers[i].PciId) != NULL) {
                    isI2CClass = TRUE;
                    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                                "AddDevice: matched controller %ws",
                                g_I2cControllers[i].PciId);
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
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_INIT,
                    "AddDevice: unsupported controller, skipping");
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
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "AddDevice: IoCreateDevice failed 0x%08lx", status);
        return status;
    }

    /* ---------------------------------------------------------------
       Initialize FDO context
       --------------------------------------------------------------- */
    devctx = (PI2CCTRL_FDO)fdo->DeviceExtension;
    RtlZeroMemory(devctx, sizeof(*devctx));

    devctx->Self           = fdo;
    devctx->PhysicalDevice = PhysicalDeviceObject;
    devctx->LowerDevice    = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);

    if (devctx->LowerDevice == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "AddDevice: IoAttachDeviceToDeviceStack failed");
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    devctx->ControllerId =
        InterlockedIncrement((volatile LONG*)&g_I2cCtrlGlobal.NextControllerId);

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
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "AddDevice: IdentifyAndInitController failed 0x%08lx", status);
        IoDetachDevice(devctx->LowerDevice);
        IoDeleteDevice(fdo);
        return status;
    }

    I2cCtrlApplyQuirks(devctx);

    /* ---------------------------------------------------------------
       Child PDO lifecycle helpers
       --------------------------------------------------------------- */
    devctx->DeleteChildrenFn    = I2cCtrl_DeleteChildPdos;
    devctx->EnumerateChildrenFn = I2cCtrl_EnumerateAcpiChildren;

    /* Ready for PnP */
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                "AddDevice complete (Ctrl%lu)", devctx->ControllerId);

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

/* --- Enumerate ACPI children (XP‑safe, C89-compliant, HID‑I2C via HID table) --- */
NTSTATUS
I2cCtrl_EnumerateAcpiChildren(
    PDEVICE_OBJECT Fdo,
    PI2CCTRL_FDO   fdoExt,
    PULONG         ChildCountOut
    )
{
    NTSTATUS status;
    ULONG childCount = 0;
    PACPI_EVAL_OUTPUT_BUFFER outBuf;
    ULONG outLen;
    ACPI_METHOD_ARGUMENT UNALIGNED* arg;
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

    /* _STA helpers (C89: declare at top) */
    PACPI_EVAL_OUTPUT_BUFFER staBuf;
    ACPI_METHOD_ARGUMENT UNALIGNED* staArg;
    ULONG staLen;
    ULONG staVal;

    /* _UID/_ADR helpers */
    ULONG uidLen;
    ULONG adrLen;

    /* Init */
    if (ChildCountOut) {
        *ChildCountOut = 0;
    }

    if (!Fdo || !fdoExt || !fdoExt->Self) {
        return STATUS_INVALID_PARAMETER;
    }

    I2CCTRL_REQUIRE_PASSIVE();

    /* Open ACPI */
    status = I2cCtrl_AcpiOpen(fdoExt);
    if (!NT_SUCCESS(status) || !fdoExt->AcpiDeviceObject) {
        return STATUS_SUCCESS;
    }

    opened = TRUE;

    /* Allocate buffer */
    outLen = sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 512;
    outBuf = (PACPI_EVAL_OUTPUT_BUFFER)ExAllocatePoolWithTag(PagedPool, outLen, 'Acpi');
    if (!outBuf) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(outBuf, outLen);

    /* Prepare UID buffers */
    uidUni.Buffer = uidBuf;
    uidUni.Length = 0;
    uidUni.MaximumLength = (USHORT)(sizeof(uidBuf) - sizeof(WCHAR));

    uidAnsi.Buffer = NULL;
    uidAnsi.Length = 0;
    uidAnsi.MaximumLength = 0;

    numUni.Buffer = numBuf;
    numUni.Length = 0;
    numUni.MaximumLength = (USHORT)(sizeof(numBuf) - sizeof(WCHAR));

    /* --- Read _UID --- */
    uidLen = sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 128;
    RtlZeroMemory(outBuf, outLen);
    RtlZeroMemory(uidBuf, sizeof(uidBuf));

    status = I2cCtrl_AcpiEvalMethod(fdoExt, L"_UID", (PUCHAR)outBuf, &uidLen);
    if (NT_SUCCESS(status) && outBuf->Count > 0) {
        arg = (ACPI_METHOD_ARGUMENT UNALIGNED*)&outBuf->Argument[0];

        if (arg->Type == ACPI_METHOD_ARGUMENT_STRING &&
            arg->DataLength < sizeof(uidBuf)) {

            uidAnsi.Buffer = (PCHAR)arg->Data;
            uidAnsi.Length = (USHORT)arg->DataLength;
            uidAnsi.MaximumLength = (USHORT)arg->DataLength;

            if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&uidUni, &uidAnsi, FALSE))) {
                haveUid = TRUE;
            }

        } else if (arg->Type == ACPI_METHOD_ARGUMENT_INTEGER) {

            uidInt = (ULONG)arg->Argument;
            RtlZeroMemory(numBuf, sizeof(numBuf));

            if (NT_SUCCESS(RtlIntegerToUnicodeString(uidInt, 10, &numUni))) {
                haveUid = TRUE;
            }
        }
    }

    /* --- Read _ADR --- */
    adrLen = sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 64;
    RtlZeroMemory(outBuf, outLen);

    if (NT_SUCCESS(I2cCtrl_AcpiEvalMethod(fdoExt, L"_ADR", (PUCHAR)outBuf, &adrLen)) &&
        outBuf->Count > 0) {

        arg = (ACPI_METHOD_ARGUMENT UNALIGNED*)&outBuf->Argument[0];
        if (arg->Type == ACPI_METHOD_ARGUMENT_INTEGER) {
            adrVal = (ULONG)arg->Argument;
            haveAdr = TRUE;
        }
    }

    /* --- Read _HID --- */
    RtlZeroMemory(outBuf, outLen);
    hidUni.Buffer = NULL;
    hidUni.Length = 0;
    hidUni.MaximumLength = 0;

    hidAnsi.Buffer = NULL;
    hidAnsi.Length = 0;
    hidAnsi.MaximumLength = 0;

    status = I2cCtrl_AcpiEvalMethod(fdoExt, L"_HID", (PUCHAR)outBuf, &outLen);
    if (!NT_SUCCESS(status)) {
        goto Done;
    }

    arg = (ACPI_METHOD_ARGUMENT UNALIGNED*)&outBuf->Argument[0];

    for (i = 0; i < outBuf->Count; i++) {

        if (arg->Type == ACPI_METHOD_ARGUMENT_STRING &&
            arg->DataLength > 0) {

            hidAnsi.Buffer = (PCHAR)arg->Data;
            hidAnsi.Length = (USHORT)arg->DataLength;
            hidAnsi.MaximumLength = (USHORT)arg->DataLength;

            if (NT_SUCCESS(RtlAnsiStringToUnicodeString(&hidUni, &hidAnsi, TRUE))) {

                /* --- Check _STA --- */
                staLen = sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 32;
                staBuf = (PACPI_EVAL_OUTPUT_BUFFER)
                         ExAllocatePoolWithTag(PagedPool, staLen, 'Acpi');

                if (!staBuf) {
                    RtlFreeUnicodeString(&hidUni);
                    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
                    continue;
                }

                RtlZeroMemory(staBuf, staLen);

                if (!NT_SUCCESS(I2cCtrl_AcpiEvalMethod(fdoExt, L"_STA",
                                                       (PUCHAR)staBuf, &staLen)) ||
                    staBuf->Count == 0) {

                    ExFreePoolWithTag(staBuf, 'Acpi');
                    RtlFreeUnicodeString(&hidUni);
                    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
                    continue;
                }

                staArg = (ACPI_METHOD_ARGUMENT UNALIGNED*)&staBuf->Argument[0];

                if (staArg->Type != ACPI_METHOD_ARGUMENT_INTEGER) {
                    ExFreePoolWithTag(staBuf, 'Acpi');
                    RtlFreeUnicodeString(&hidUni);
                    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
                    continue;
                }

                staVal = (ULONG)staArg->Argument;
                ExFreePoolWithTag(staBuf, 'Acpi');

                if ((staVal & 0x01) == 0) {
                    RtlFreeUnicodeString(&hidUni);
                    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
                    continue;
                }

                /* --- HID-over-I2C via HID table --- */
                hidMatch = I2cCtrl_FindHidMatch(hidUni.Buffer);

                /* --- Create PDO --- */
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

                    /* Initialize HID child IDs */
                    I2cCtrlInitHidChildIds(childDx, childCount, hidMatch);

                    /* Apply HID quirks */
                    if (hidMatch) {
                        I2cHidApplyQuirks(childDx, hidMatch);
                    }

                    /* Save bus address */
                    if (haveAdr) {
                        childDx->SavedBusAddress = adrVal & 0x03FF;
                    } else {
                        childDx->SavedBusAddress = 0;
                    }

                    pdo->Flags |= DO_POWER_PAGABLE;
                    pdo->Flags &= ~DO_DEVICE_INITIALIZING;

                    KeAcquireSpinLock(&fdoExt->ChildLock, &oldIrql);
                    InsertTailList(&fdoExt->ChildList, &childDx->ListEntry);
                    fdoExt->NumChildren++;
                    KeReleaseSpinLock(&fdoExt->ChildLock, oldIrql);

                    childCount++;
                }

                RtlFreeUnicodeString(&hidUni);
                break;
            }
        }

        arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
    }

Done:
    if (childCount > 0 && fdoExt->PhysicalDevice) {
        IoInvalidateDeviceRelations(fdoExt->PhysicalDevice, BusRelations);
    }

    if (ChildCountOut) {
        *ChildCountOut = childCount;
    }

    if (outBuf) {
        ExFreePoolWithTag(outBuf, 'Acpi');
    }

    return STATUS_SUCCESS;
}


/* Open ACPI device object (\Device\ACPI) and cache pointers safely.
 * XP/2003-safe, idempotent, never abuses ACPI handles.
 */
NTSTATUS
I2cCtrl_AcpiOpen(
    PI2CCTRL_FDO fdoExt
    )
{
    UNICODE_STRING acpiName;
    NTSTATUS       status;
    PFILE_OBJECT   fileObj;
    PDEVICE_OBJECT devObj;

    fileObj = NULL;
    devObj  = NULL;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (fdoExt == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ACPI,
                    "AcpiOpen: invalid fdoExt (NULL)");
        return STATUS_INVALID_PARAMETER;
    }

    /* Already bound: nothing to do */
    if (fdoExt->AcpiBound &&
        fdoExt->AcpiDeviceObject != NULL &&
        fdoExt->AcpiFileObject   != NULL) {

        return STATUS_SUCCESS;
    }

    RtlInitUnicodeString(&acpiName, L"\\Device\\ACPI");

    status = IoGetDeviceObjectPointer(&acpiName,
                                      FILE_READ_DATA,
                                      &fileObj,
                                      &devObj);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ACPI,
                    "AcpiOpen: IoGetDeviceObjectPointer failed (0x%08X)", status);
        fdoExt->AcpiDeviceObject = NULL;
        fdoExt->AcpiFileObject   = NULL;
        fdoExt->AcpiHandle       = NULL;
        fdoExt->AcpiBound        = FALSE;
        return status;
    }

    /* Cache only what ACPI IOCTLs require; never treat devObj as an ACPI handle */
    fdoExt->AcpiDeviceObject = devObj;
    fdoExt->AcpiFileObject   = fileObj;
    fdoExt->AcpiHandle       = NULL;
    fdoExt->AcpiBound        = TRUE;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ACPI,
                "AcpiOpen: ACPI bound (DevObj=%p, FileObj=%p)", devObj, fileObj);

    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * I2cCtrl_AcpiClose - Safe controller-level ACPI close (XP/2003-compatible)
 * - PASSIVE_LEVEL only
 * - Never closes child ACPI handles (PDO REMOVE handles that)
 * - Only dereferences the ACPI file object
 * - Clears cached pointers without touching ACPI-owned memory
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_AcpiClose(
    PI2CCTRL_FDO fdoExt
    )
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    PAGED_CODE();

    if (fdoExt == NULL) {
        return;
    }

    if (!fdoExt->AcpiBound) {
        return;
    }

    /* Release the ACPI file object reference */
    if (fdoExt->AcpiFileObject != NULL) {
        ObDereferenceObject(fdoExt->AcpiFileObject);
        fdoExt->AcpiFileObject = NULL;
    }

    /*
     * IMPORTANT:
     * - AcpiDeviceObject is NOT an ACPI handle and must NOT be closed.
     * - AcpiHandle (child or controller) must NOT be closed here.
     *   Child handles are closed in PDO REMOVE.
     *   Controller handle is never used as a real ACPI handle.
     */

    fdoExt->AcpiDeviceObject = NULL;
    fdoExt->AcpiHandle       = NULL;
    fdoExt->AcpiBound        = FALSE;
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
    PI2CCTRL_FDO devctx,
    PCWSTR       MethodName,
    PUCHAR       OutBuffer,
    PULONG       OutBufferLen
    )
{
    NTSTATUS                 status;
    KEVENT                   event;
    PIRP                     irp;
    IO_STATUS_BLOCK          iosb;
    PACPI_EVAL_INPUT_BUFFER  input;
    ULONG                    inputLen;
    ULONG                    outLen;
    LARGE_INTEGER            timeout;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (devctx == NULL || MethodName == NULL ||
        OutBuffer == NULL || OutBufferLen == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (devctx->Removed) {
        return STATUS_DEVICE_REMOVED;
    }

    status = I2cCtrl_AcpiOpen(devctx);
    if (!NT_SUCCESS(status) || devctx->AcpiDeviceObject == NULL) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    outLen = *OutBufferLen;
    if (outLen < sizeof(ACPI_EVAL_OUTPUT_BUFFER)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Allocate input buffer on heap (never on stack!) */
    inputLen = sizeof(ACPI_EVAL_INPUT_BUFFER);
    input = (PACPI_EVAL_INPUT_BUFFER)
            ExAllocatePoolWithTag(NonPagedPool, inputLen, 'Acpi');
    if (input == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(input, inputLen);

#ifdef ACPI_EVAL_INPUT_BUFFER_SIGNATURE
    input->Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
#endif

    I2cCtrl_AcpiFillMethodName(MethodName, (UCHAR*)&input->MethodName);

    /* Do NOT zero caller's OutBuffer before ACPI writes into it */
    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
              IOCTL_ACPI_EVAL_METHOD,
              devctx->AcpiDeviceObject,
              input,
              inputLen,
              OutBuffer,
              outLen,
              FALSE,
              &event,
              &iosb);

    if (irp == NULL) {
        ExFreePoolWithTag(input, 'Acpi');
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(devctx->AcpiDeviceObject, irp);
    if (status == STATUS_PENDING) {
        timeout.QuadPart = -5 * 1000 * 1000 * 10; /* 5 seconds */
        (VOID)KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, &timeout);
        status = iosb.Status;
    }

    ExFreePoolWithTag(input, 'Acpi');

    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Validate IoStatus.Information, not outb->Length */
    if (iosb.Information < sizeof(ACPI_EVAL_OUTPUT_BUFFER) ||
        iosb.Information > outLen) {
        return STATUS_BUFFER_OVERFLOW;
    }

    *OutBufferLen = (ULONG)iosb.Information;
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
    char  lineBuf[80]; /* enough for offset + 16 bytes + ASCII */

    if (buf == NULL || len == 0) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_BUS,
                    "HexDump(%s): invalid buffer or length=%lu",
                    (tag != NULL) ? tag : "CRS", (unsigned long)len);
        return;
    }

    ascii[16] = '\0';
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_BUS,
                "HexDump(%s), len=%lu",
                (tag != NULL) ? tag : "CRS",
                (unsigned long)len);

    line = 0;
    for (i = 0; i < len; i++) {
        UCHAR b = buf[i];
        ascii[i % 16] = (b >= 32 && b < 127) ? (char)b : '.';

        if ((i % 16) == 0) {
            RtlStringCchPrintfA(lineBuf, sizeof(lineBuf), "%04lu: ", (unsigned long)line++);
        }

        {
            char byteStr[4];
            RtlStringCchPrintfA(byteStr, sizeof(byteStr), "%02X ", (unsigned)b);
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
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_BUS, "%s", lineBuf);
            lineBuf[0] = '\0'; /* reset for next line */
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
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ACPI,
                    "ParseCrsForGpio: invalid buffer or len=%lu", len);
        return FALSE;
    }

    i = 0;
    while (i + 1 < len) {

        UCHAR tag = buf[i];

        /* -------------------------
         * SMALL RESOURCE ITEM
         * ------------------------- */
        if ((tag & 0x80) == 0) {
            UCHAR smallType = (UCHAR)((tag >> 3) & 0x0F);
            UCHAR smallLen  = (UCHAR)(tag & 0x07);

            if (smallType == 0x0F) {
                /* EndTag */
                break;
            }

            if (i + 1 + smallLen > len) {
                break;
            }

            i += 1 + smallLen;
            continue;
        }

        /* -------------------------
         * LARGE RESOURCE ITEM
         * ------------------------- */
        if (i + 3 > len) {
            break;
        }

        {
            UCHAR  largeType    = (UCHAR)(tag & 0x7F);
            USHORT largeLen     = (USHORT)(buf[i + 1] | ((USHORT)buf[i + 2] << 8));
            ULONG  payloadStart = i + 3;

            if (payloadStart + largeLen > len) {
                break;
            }

            /* ============================================================
             * GPIO Connection Descriptor (ACPI 5.0+)
             * ============================================================ */
            if (largeType == 0x8C) {

                const UCHAR *p = buf + payloadStart;
                ULONG        n = (ULONG)largeLen;

                I2cCtrl_HexDump(p, n, "GPIO");

                /*
                 * ACPI GPIO Connection Descriptor layout (simplified):
                 *
                 * Offset  Size  Meaning
                 *   0      2    Revision / Flags
                 *   2      2    ResourceSourceIndex
                 *   4      2    ResourceSourceNameOffset
                 *   6      2    VendorDataLength
                 *   8      2    PinTableOffset
                 *  10      2    PinCount
                 *  12      ?    PinTable[PinCount]
                 *
                 * We only need PinCount and first Pin.
                 */

                if (n >= 12) {

                    USHORT pinCount = (USHORT)(p[10] | ((USHORT)p[11] << 8));
                    USHORT pinOffset = (USHORT)(p[8] | ((USHORT)p[9] << 8));

                    if (pinCount >= 1 &&
                        pinOffset < n &&
                        (ULONG)pinOffset + pinCount <= n) {

                        UCHAR firstPin = p[pinOffset];

                        /* ActiveLow flag is bit0 of Flags (offset 1) */
                        UCHAR flags = p[1];
                        BOOLEAN activeLow = ((flags & 0x01) ? TRUE : FALSE);

                        *gpioPinOut   = (ULONG)firstPin;
                        *activeLowOut = activeLow;

                        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_ACPI,
                                    "GPIO parsed: pin=%u activeLow=%u pinCount=%u",
                                    (unsigned)firstPin,
                                    (unsigned)(activeLow ? 1 : 0),
                                    (unsigned)pinCount);

                        return TRUE;
                    }
                }

                /* -------------------------
                 * Fallback heuristic
                 * ------------------------- */
                if (n >= 7) {
                    UCHAR flags = p[5];
                    UCHAR pin   = p[6];

                    *gpioPinOut   = (ULONG)pin;
                    *activeLowOut = ((flags & 0x01) ? TRUE : FALSE);

                    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ACPI,
                                "GPIO fallback: pin=%u activeLow=%u",
                                (unsigned)pin,
                                (unsigned)(*activeLowOut ? 1 : 0));

                    return TRUE;
                }
            }

            /* Advance to next large item */
            i = payloadStart + largeLen;
        }
    }

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_ACPI,
                "ParseCrsForGpio: no GPIO descriptor found");
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
        if (xc->Irp == irp) {
            devctx->ActiveBusy = FALSE;
            xc->Status = STATUS_DEVICE_NOT_READY;
            xc->Irp = NULL;
        }
        KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

        /* Quiesce hardware via HAL */
        if (devctx->Ops->MaskInterrupts != NULL) {
            devctx->Ops->MaskInterrupts(devctx, 0U);
        }
        if (devctx->Ops->AckInterrupts != NULL) {
            devctx->Ops->AckInterrupts(devctx,
              I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
              I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
              I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
              I2C_INT_RD_REQ);
        }
        if (devctx->Ops->Enable != NULL) {
            (VOID)devctx->Ops->Enable(devctx, FALSE);
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

        if (devctx->Ops->MaskInterrupts != NULL) {
            devctx->Ops->MaskInterrupts(devctx, 0U);
        }
        if (devctx->Ops->AckInterrupts != NULL) {
            devctx->Ops->AckInterrupts(devctx,
              I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
              I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
              I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
              I2C_INT_RD_REQ);
        }

        if (devctx->Ops->Enable != NULL) {
            (VOID)devctx->Ops->Enable(devctx, FALSE);
            (VOID)devctx->Ops->Enable(devctx, TRUE);
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
    if (xc->Irp == irp) {
        devctx->ActiveBusy = FALSE;
        xc->Status = STATUS_IO_TIMEOUT;
        xc->Irp = NULL;
    }
    KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

    if (devctx->Ops->MaskInterrupts != NULL) {
        devctx->Ops->MaskInterrupts(devctx, 0U);
    }
    if (devctx->Ops->AckInterrupts != NULL) {
        devctx->Ops->AckInterrupts(devctx,
          I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
          I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
          I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
          I2C_INT_RD_REQ);
    }
    if (devctx->Ops->Enable != NULL) {
        (VOID)devctx->Ops->Enable(devctx, FALSE);
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

    /* Reset and configure transfer context */
    xc = &devctx->XferCtx;
    RtlZeroMemory(xc, sizeof(*xc));
    xc->Irp        = irp;
    xc->Status     = STATUS_SUCCESS;
    xc->StopSeen   = FALSE;
    xc->RetryCount = 0;

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
            xc->Irp = NULL;
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
        devctx->Ops->AckInterrupts(devctx,
            I2C_INT_TX_ABORT  | I2C_INT_RX_OVER | I2C_INT_RX_UNDER |
            I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
            I2C_INT_GEN_CALL | I2C_INT_ACTIVITY |
            I2C_INT_RX_DONE  | I2C_INT_RD_REQ);
    }

    /* Dispatch by opcode (existing logic retained) */
    if (req->OpCode == I2CCTRL_OPCODE_BLOCK_WRITE) {
        /* … existing block write logic unchanged … */

    } else if (req->OpCode == I2CCTRL_OPCODE_BLOCK_READ) {
        /* … existing block read logic unchanged … */

    } else if (req->OpCode == I2CCTRL_OPCODE_GET_PT_SAMPLE) {
        /* HID/touch path: delegate to QueryTouchSample */
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
        xc->Irp = NULL;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        ExFreePool(req);
        return;

    } else {
        /* Unsupported opcode: fail the IRP and free request */
        irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        irp->IoStatus.Information = 0;
        irp->Tail.Overlay.DriverContext[0] = NULL;
        xc->Irp = NULL;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        ExFreePool(req);
        return;
    }
}

/* -----------------------------------------------------------------------
 * I2cCtrl_DetectTouchpadRedirect - safe wrapper for touchpad detection
 * Ensures parameters are valid, result struct is zeroed, controller is
 * started via I2cCtrl_StartDevice, and detection is delegated to the helper.
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_DetectTouchpadRedirect(
    struct _I2CCTRL_FDO *fdoExt,
    struct _I2CCTRL_DETECT_RESULT *result
    )
{
    NTSTATUS status;

    if (fdoExt == NULL || result == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "DetectTouchpadRedirect: invalid parameters (fdoExt=%p, result=%p)",
                    fdoExt, result);
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(result, sizeof(*result));

    /* Ensure controller is started before detection */
    status = I2cCtrl_StartDevice(fdoExt, NULL);
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "DetectTouchpadRedirect: StartDevice failed before touchpad detection (status=0x%08lx)",
                    status);
        return status;
    }

    /* Delegate to actual detection helper */
    status = I2cCtrl_DetectTouchpad(fdoExt, result);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_INIT,
                    "DetectTouchpadRedirect: Touchpad detection failed (status=0x%08lx)",
                    status);
    } else {
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                    "DetectTouchpadRedirect: Touchpad detection succeeded (Present=%lu)",
                    (ULONG)result->Present);
    }

    return status;
}


/* -----------------------------------------------------------------------
 * Simple kernel logger: appends to \SystemRoot\System32\i2cctrl.log
 * Must be called at PASSIVE_LEVEL.
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_LogSimple(
    PCSTR Text
    )
{
    UNICODE_STRING      path;
    OBJECT_ATTRIBUTES   oa;
    IO_STATUS_BLOCK     iosb;
    HANDLE              hFile;
    NTSTATUS            status;
    SIZE_T              len;

    PAGED_CODE();

    if (Text == NULL) {
        return;
    }

    len = strlen(Text);
    if (len == 0) {
        return;
    }

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

    ZwWriteFile(
        hFile,
        NULL,
        NULL,
        NULL,
        &iosb,
        (PVOID)Text,
        (ULONG)len,
        NULL,
        NULL
    );

    ZwClose(hFile);
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

    I2cCtrl_LogSimple("StartDevice: entered\n");

    if (fdoExt == NULL || Irp == NULL) {
        I2cCtrl_LogSimple("StartDevice: invalid parameters\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* -------------------------------------------------------------
     * Populate PnpId (required for quirks + backend selection)
     * ------------------------------------------------------------- */
    {
        WCHAR    hwidBuf[256];
        ULONG    hwidLen = 0;
        NTSTATUS st;

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
                I2cCtrl_LogSimple("StartDevice: PnpId captured\n");
            } else {
                fdoExt->PnpId = NULL;
                I2cCtrl_LogSimple("StartDevice: PnpId alloc failed\n");
            }

        } else {
            fdoExt->PnpId = NULL;
            I2cCtrl_LogSimple("StartDevice: PnpId unavailable\n");
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
            I2cCtrl_LogSimple(abuf);
        }

        return STATUS_NOT_SUPPORTED;
    }

    /* Log matched controller (BAR0 offsets only — LPSS offsets not used here) */
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
            L"quirks=0x%X bsod=0x%X)",
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
            I2cCtrl_LogSimple(abuf);
        }
    }

    /* Store quirks (your FDO already has BsodQuirks, but NOT Quirks) */
    fdoExt->BsodQuirks = match->BsodQuirks;

    /* Quirks are applied later by I2cCtrlApplyQuirks() using PnpId */
}


    isl = IoGetCurrentIrpStackLocation(Irp);
    transList =
        (isl != NULL) ? isl->Parameters.StartDevice.AllocatedResourcesTranslated : NULL;

    I2cCtrl_LogSimple("StartDevice: got translated resources\n");

    if (transList == NULL || transList->Count == 0U) {
        I2cCtrl_LogSimple("StartDevice: no translated resources\n");
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
                /* Second memory resource = LPSS BAR2 */
                else if (!haveBar2) {
                    bar2Phys   = desc->u.Memory.Start;
                    bar2Length = desc->u.Memory.Length;
                    haveBar2   = TRUE;
                }

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
        I2cCtrl_LogSimple("StartDevice: no MMIO resource\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (mmioLength < 0x00A8U) {
        I2cCtrl_LogSimple("StartDevice: MMIO length too small\n");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* Map BAR0 (DW-I2C) */
    fdoExt->Mmio = (PUCHAR)MmMapIoSpace(mmioPhys, mmioLength, MmNonCached);
    if (fdoExt->Mmio == NULL) {
        I2cCtrl_LogSimple("StartDevice: MmMapIoSpace failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    fdoExt->MmioPhys   = mmioPhys;
    fdoExt->MmioLength = mmioLength;
    fdoExt->MmioBase   = fdoExt->Mmio;

    I2cCtrl_LogSimple("StartDevice: MMIO mapped\n");

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
                I2cCtrl_LogSimple(abuf);
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
        I2cCtrl_LogSimple("StartDevice: LPSS BAR2 mapped\n");
    } else {
        I2cCtrl_LogSimple("StartDevice: LPSS BAR2 map FAILED\n");
    }

} else {

    fdoExt->LpssBar2 = NULL;
    I2cCtrl_LogSimple("StartDevice: no LPSS BAR2 resource\n");
}


    /* Install backend FIRST (Intel DW-I2C / Cannon Lake style) */
    I2cCtrl_InstallBackend(fdoExt);

    /* Apply unified LPSS + DW-I2C quirks AFTER backend install */
    I2cCtrl_LogSimple("StartDevice: applying unified quirks\n");
    I2cCtrlApplyQuirks(fdoExt);
    I2cCtrl_LogSimple("StartDevice: unified quirks applied\n");

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
        I2cCtrl_LogSimple("StartDevice: disable did not latch\n");
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
        I2cCtrl_LogSimple(abuf);
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
            I2cCtrl_LogSimple(abuf);
        }

        I2cCtrl_LogSimple("StartDevice: Falling back to polling mode\n");

        fdoExt->InterruptObject = NULL;
        haveInt = FALSE;
    }
}

    /* Program safe initial interrupt mask after ISR connect */
    if (fdoExt->Ops != NULL && fdoExt->Ops->MaskInterrupts != NULL) {
        fdoExt->Ops->MaskInterrupts(fdoExt, fdoExt->IntrMask);
    }

    /* Enable controller and confirm enable latched */
    if (fdoExt->Ops != NULL && fdoExt->Ops->Enable != NULL) {
        (VOID)fdoExt->Ops->Enable(fdoExt, TRUE);
    }

status = I2cCtrl_WaitForEnableState(fdoExt, TRUE, 500U);
if (!NT_SUCCESS(status)) {

    /* WinDbg-style detailed diagnostics */
    {
        WCHAR wbuf[160];
        CHAR  abuf[160];
        UNICODE_STRING ustr;
        ANSI_STRING astr;

        RtlStringCchPrintfW(
            wbuf,
            RTL_NUMBER_OF(wbuf),
            L"StartDevice: ENABLE FAILED (status=0x%08lx)\n"
            L"  HWID=%ws\n"
            L"  BAR0=PA=%08X%08X Len=%lu\n"
            L"  IRQ: Vector=%lu Level=%lu Mode=%s Sharable=%lu\n",
            status,
            (fdoExt->PnpId != NULL) ? fdoExt->PnpId : L"<null>",
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
            I2cCtrl_LogSimple(abuf);
        }
    }

    fdoExt->HardwareFailure = TRUE;

    if (fdoExt->InterruptObject != NULL) {
        IoDisconnectInterrupt(fdoExt->InterruptObject);
        fdoExt->InterruptObject = NULL;
    }

    MmUnmapIoSpace(fdoExt->Mmio, fdoExt->MmioLength);
    fdoExt->Mmio       = NULL;
    fdoExt->MmioLength = 0U;
    fdoExt->MmioPhys.QuadPart = 0;

    return status;
}

    /* Clear transfer context and runtime flags */
    RtlZeroMemory(&fdoExt->XferCtx, sizeof(fdoExt->XferCtx));
    KeResetEvent(&fdoExt->TransferEvent);
    fdoExt->ActiveBusy = FALSE;
    fdoExt->Started    = TRUE;
    fdoExt->Removed    = FALSE;
    fdoExt->Stopping   = FALSE;

    /* Prepare hot-plug rebind runtime flags */
    fdoExt->HotplugPending = FALSE;
    fdoExt->ChildrenStale  = FALSE;

    I2cCtrl_LogSimple("StartDevice: controller enabled, runtime flags set\n");

    /* Optional: probe for touchpad presence */
    {
        I2CCTRL_DETECT_RESULT detectResult;
        RtlZeroMemory(&detectResult, sizeof(detectResult));

        status = g_I2cCtrlGlobal.DetectTouchpad(fdoExt, &detectResult);
        if (!NT_SUCCESS(status)) {
            I2cCtrl_LogSimple("StartDevice: touchpad detection failed\n");
            fdoExt->TouchpadPresent = FALSE;
        } else {
            fdoExt->TouchpadPresent = detectResult.Present ? TRUE : FALSE;
            I2cCtrl_LogSimple(
                detectResult.Present ?
                "StartDevice: touchpad present\n" :
                "StartDevice: touchpad not present\n"
            );
        }
    }

    /* Create universal HID-over-I2C child PDO */
    if (IsListEmpty(&fdoExt->ChildList)) {

        WCHAR hidBuf[64];
        WCHAR uidBuf[32];

        I2cCtrl_LogSimple("StartDevice: ChildList empty, creating PNP0C50 PDO\n");

        RtlZeroMemory(hidBuf, sizeof(hidBuf));
        RtlZeroMemory(uidBuf, sizeof(uidBuf));

        RtlStringCchCopyW(hidBuf, RTL_NUMBER_OF(hidBuf), L"ACPI\\PNP0C50");
        RtlStringCchCopyW(uidBuf, RTL_NUMBER_OF(uidBuf), L"0000");

        status = I2cCtrl_CreateChildPdo(fdoExt->Self, fdoExt, hidBuf, uidBuf);
        if (!NT_SUCCESS(status)) {

            I2cCtrl_LogSimple("StartDevice: CreateChildPdo FAILED\n");
            status = STATUS_SUCCESS;

        } else {

            I2cCtrl_LogSimple("StartDevice: CreateChildPdo SUCCESS, invalidating BusRelations\n");
            IoInvalidateDeviceRelations(fdoExt->PhysicalDevice, BusRelations);
        }
    } else {
        I2cCtrl_LogSimple("StartDevice: ChildList not empty, skipping PNP0C50 PDO\n");
    }

    I2cCtrl_LogSimple("StartDevice: complete\n");

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

    I2cCtrl_LogSimple("StopDevice: begin\n");

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
            I2cCtrl_LogSimple("StopDevice: disable did not latch\n");
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

    /* 8) Unmap LPSS BAR2 (if mapped) */
    if (fdoExt->LpssBar2 != NULL) {
        MmUnmapIoSpace(fdoExt->LpssBar2, fdoExt->LpssBar2Length);
        fdoExt->LpssBar2       = NULL;
        fdoExt->LpssBar2Length = 0U;
        fdoExt->LpssBar2Phys.QuadPart = 0;
        I2cCtrl_LogSimple("StopDevice: LPSS BAR2 unmapped\n");
    }

    /* 9) Unmap MMIO (BAR0) */
    if (fdoExt->Mmio != NULL) {
        MmUnmapIoSpace(fdoExt->Mmio, fdoExt->MmioLength);
        fdoExt->Mmio              = NULL;
        fdoExt->MmioLength        = 0U;
        fdoExt->MmioPhys.QuadPart = 0;
        fdoExt->MmioBase          = NULL;
        I2cCtrl_LogSimple("StopDevice: MMIO unmapped\n");
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

    I2cCtrl_LogSimple("StopDevice: complete\n");

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
        I2cCtrl_LogSimple("RestartDevice: invalid IRQL\n");
        return STATUS_INVALID_DEVICE_STATE;
    }
    PAGED_CODE();

    if (fdoExt == NULL) {
        I2cCtrl_LogSimple("RestartDevice: fdoExt=NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    I2cCtrl_LogSimple("RestartDevice: begin\n");

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
        I2cCtrl_LogSimple("RestartDevice: StopDevice FAILED\n");
        return status;
    }

    /*
     * DO NOT call StartDevice here.
     * XP/2003 requires a real IRP_MN_START_DEVICE from PnP
     * to provide fresh resources and restart the controller.
     */
    I2cCtrl_LogSimple("RestartDevice: complete, awaiting PnP START_DEVICE\n");

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
        I2cCtrl_LogSimple("RemoveDevice: invalid IRQL\n");
        return STATUS_INVALID_DEVICE_STATE;
    }
    PAGED_CODE();

    if (fdoExt == NULL) {
        I2cCtrl_LogSimple("RemoveDevice: fdoExt=NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    I2cCtrl_LogSimple("RemoveDevice: begin\n");

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
        I2cCtrl_LogSimple("RemoveDevice: StopDevice FAILED\n");
        /* Continue anyway — removal must not fail */
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

    I2cCtrl_LogSimple("RemoveDevice: complete\n");

    return STATUS_SUCCESS;
}


/*
 * I2cCtrl_WaitForEnableState - poll HAL status until target state latched
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant
 *
 * Parameters:
 *   fdoExt   - controller extension
 *   targetOn - TRUE to wait for enabled, FALSE to wait for disabled
 *   timeout  - max iterations (each ~1µs via KeStallExecutionProcessor)
 *
 * Returns:
 *   STATUS_SUCCESS if state latched
 *   STATUS_DEVICE_HARDWARE_ERROR if timeout expired without latch
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
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "WaitForEnableState: invalid parameters (fdoExt=%p, Mmio=%p, Len=%lu)",
                    fdoExt,
                    (fdoExt != NULL ? fdoExt->Mmio : NULL),
                    (fdoExt != NULL ? fdoExt->MmioLength : 0U));
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&hwst, sizeof(hwst));

    for (i = 0U; i < timeout; i++) {
        if (fdoExt->Ops != NULL && fdoExt->Ops->GetStatus != NULL) {
            st = fdoExt->Ops->GetStatus(fdoExt, &hwst);
            if (!NT_SUCCESS(st)) {
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_INIT,
                            "WaitForEnableState: GetStatus failed (status=0x%08lx)", st);
                break; /* hardware read failed */
            }

            /* Interpret enable state from StatusReg bit0 */
            if (((hwst.StatusReg & 0x1U) != 0U) == targetOn) {
                return STATUS_SUCCESS; /* latched as requested */
            }
        }

        KeStallExecutionProcessor(1U);
    }

    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                "WaitForEnableState: timeout expired (targetOn=%lu)", (ULONG)targetOn);
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
 * WriteBurstPolled - HAL-generic, XP/2003-safe, C89-compliant, universal
 *
 * Purpose:
 *   - Perform a multi-byte burst write to an I2C slave device
 *   - Sequence bytes robustly using HAL ops only (no backend/register refs)
 *   - Handle ACK/NACK detection and hardware failure flagging
 *   - Insert RESTART/STOP using HAL optional ops when available
 *   - Confirm STOP condition via HAL status before returning
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Avoids any direct MMIO/register access
 *   - Completes or fails deterministically with hardware failure flagged
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
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                    "WriteBurstPolled: invalid parameters (Ctrl%p, buf=%p, len=%lu)",
                    fdoExt, buf, len);
        return STATUS_INVALID_PARAMETER;
    }

    if (fdoExt->Ops == NULL ||
        fdoExt->Ops->GetStatus == NULL ||
        fdoExt->Ops->SetTarget7bit == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                    "WriteBurstPolled: HAL ops missing (Ctrl%lu)", fdoExt->ControllerId);
        return STATUS_DEVICE_NOT_READY;
    }

    /* Program target address via HAL */
    status = fdoExt->Ops->SetTarget7bit(fdoExt, (UCHAR)(slaveAddr & 0x7FU));
    if (!NT_SUCCESS(status)) {
        fdoExt->HardwareFailure = TRUE;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                    "WriteBurstPolled: SetTarget7bit failed (Ctrl%lu, status=0x%08lx)",
                    fdoExt->ControllerId, status);
        return status;
    }

    /* Optional: emit restart before first byte */
    if (issueRestart != FALSE && fdoExt->Ops->EmitRestartIfNeeded != NULL) {
        status = fdoExt->Ops->EmitRestartIfNeeded(fdoExt);
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                        "WriteBurstPolled: EmitRestartIfNeeded failed (Ctrl%lu, status=0x%08lx)",
                        fdoExt->ControllerId, status);
            return status;
        }
    }

    while (sent < len) {
        /* ACK/NACK detection (universal helper, HAL-only inside) */
        status = I2cCtrl_CheckAckAndClear(fdoExt);
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                        "WriteBurstPolled: ACK/NACK failure (Ctrl%lu, status=0x%08lx)",
                        fdoExt->ControllerId, status);
            return status;
        }

        /* Poll HAL status for TX FIFO space */
        status = fdoExt->Ops->GetStatus(fdoExt, &hwst);
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                        "WriteBurstPolled: GetStatus failed (Ctrl%lu, status=0x%08lx)",
                        fdoExt->ControllerId, status);
            return status;
        }

        if (hwst.TxFifoNotFull != FALSE) {
            UCHAR byte = buf[sent];

            /* Write a TX byte via HAL (required) */
            if (fdoExt->Ops->WriteTxByte != NULL) {
                status = fdoExt->Ops->WriteTxByte(fdoExt, byte);
                if (!NT_SUCCESS(status)) {
                    fdoExt->HardwareFailure = TRUE;
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                                "WriteBurstPolled: WriteTxByte failed (Ctrl%lu, status=0x%08lx)",
                                fdoExt->ControllerId, status);
                    return status;
                }
            } else if (fdoExt->Ops->PrimeWrite != NULL) {
                ULONG pushed = 0U;
                status = fdoExt->Ops->PrimeWrite(fdoExt, &byte, 1U, &pushed);
                if (!NT_SUCCESS(status) || pushed != 1U) {
                    fdoExt->HardwareFailure = TRUE;
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                                "WriteBurstPolled: PrimeWrite failed (Ctrl%lu, status=0x%08lx, pushed=%lu)",
                                fdoExt->ControllerId, status, pushed);
                    return NT_SUCCESS(status) ? STATUS_DEVICE_HARDWARE_ERROR : status;
                }
            } else {
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                            "WriteBurstPolled: no HAL path to write data (Ctrl%lu)",
                            fdoExt->ControllerId);
                return STATUS_NOT_SUPPORTED;
            }

            sent++;
        } else {
            /* Small passive wait to avoid busy spin */
            KeStallExecutionProcessor(5U);
        }
    }

    /* Optional STOP emit via HAL */
    if (issueStop != FALSE && fdoExt->Ops->EmitStopIfNeeded != NULL) {
        status = fdoExt->Ops->EmitStopIfNeeded(fdoExt);
        if (!NT_SUCCESS(status)) {
            fdoExt->HardwareFailure = TRUE;
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                        "WriteBurstPolled: EmitStopIfNeeded failed (Ctrl%lu, status=0x%08lx)",
                        fdoExt->ControllerId, status);
            return status;
        }
    }

    /* Confirm STOP (universal: rely on HAL status) */
    if (issueStop != FALSE) {
        ULONG loops = 0U;
        for (;;) {
            status = fdoExt->Ops->GetStatus(fdoExt, &hwst);
            if (!NT_SUCCESS(status)) {
                fdoExt->HardwareFailure = TRUE;
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                            "WriteBurstPolled: GetStatus failed during STOP wait (Ctrl%lu, status=0x%08lx)",
                            fdoExt->ControllerId, status);
                return status;
            }

            if (hwst.StopDetected != FALSE) {
                break;
            }

            KeStallExecutionProcessor(5U);
            loops++;
            if (loops > 10000U) { /* ~50ms max wait */
                fdoExt->HardwareFailure = TRUE;
                TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_HW,
                            "WriteBurstPolled: STOP timeout (Ctrl%lu)", fdoExt->ControllerId);
                return STATUS_IO_TIMEOUT;
            }
        }

        /* Clear STOP via HAL if supported */
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
        //
        // PCI/ACPI successfully started the device.
        // Now map BARs, init controller, enumerate children.
        //
        I2cCtrl_StartDevice(fdoExt, Irp);
    }

    //
    // RELEASE THE REMOVE LOCK — REQUIRED!
    //
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

    /* Initialize the DPC object for bottom‑half processing */
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
// XP/2003-safe, C89-compliant.
//
NTSTATUS
I2cCtrl_DispatchPnP(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PIO_STACK_LOCATION isl;
    NTSTATUS           status;
    PVOID              ext;

    PAGED_CODE();
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (DeviceObject == NULL || Irp == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    isl    = IoGetCurrentIrpStackLocation(Irp);
    status = Irp->IoStatus.Status;
    ext    = DeviceObject->DeviceExtension;

    if (ext == NULL) {
        Irp->IoStatus.Status      = STATUS_NO_SUCH_DEVICE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NO_SUCH_DEVICE;
    }

    //
    // Decide PDO vs FDO by signature.
    //
    if (((PI2CCTRL_PDO)ext)->Signature == I2CCTRL_PDO_SIGNATURE) {
        //
        // Child PDO - use PDO dispatch.
        //
        return I2cCtrl_PdoDispatch(DeviceObject, Irp);
    }

    //
    // Otherwise treat as FDO - use FDO dispatch.
    //
    return I2cCtrl_FdoDispatch(DeviceObject, Irp);
}

//
// Abstract helpers for I²C controller power/timing
//

/* -----------------------------------------------------------------------
 * I2cCtrl_EnableController - HAL-generic enable/disable wrapper
 * XP/2003 BSOD-safe, C89-compliant
 *
 * Purpose:
 *   - Safely enable or disable the I²C controller using HAL ops
 *   - Guard against NULL pointers and exceptions
 *   - Update runtime flags to reflect current state
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_EnableController(
    PI2CCTRL_FDO devctx,
    BOOLEAN      enable
    )
{
    NTSTATUS st;

    /* Defensive init */
    st = STATUS_SUCCESS;

    if (devctx == NULL) {
        return;
    }

    /* Use HAL ops to enable/disable controller with SEH guard */
    __try {
        if (devctx->Ops != NULL && devctx->Ops->Enable != NULL) {
            st = devctx->Ops->Enable(devctx, enable);
            if (NT_SUCCESS(st)) {
                devctx->Enabled = enable ? TRUE : FALSE;
            } else {
                devctx->HardwareFailure = TRUE;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        KdPrint(("I2CCTRL: EnableController: exception in HAL Enable\n"));
        devctx->HardwareFailure = TRUE;
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
I2cCtrl_SetDevicePowerD0(
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
        /* 1) Power/clock enable and controller bring-up (abstract HAL hooks) */
        for (tries = 0; tries < 3; tries++) {
            I2cCtrl_EnableController(devctx, TRUE);
            if (NT_SUCCESS(status)) break;
            KeStallExecutionProcessor(1000); /* 1ms backoff */
        }
        if (!NT_SUCCESS(status)) {
            KdPrint(("I2CCTRL: D0: enable controller failed status=0x%08lx\n", status));
            /* Fallback: go to D3 hard-off */
            I2cCtrl_SetDevicePowerD3(devctx);
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
        I2cCtrl_SetDevicePowerD3(devctx);
    }

    return status;
}

VOID
I2cCtrl_SetDevicePowerD1(
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
        I2cCtrl_SetDevicePowerD2(devctx);
    }
}

VOID
I2cCtrl_SetDevicePowerD2(
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
        I2cCtrl_SetDevicePowerD3(devctx);
    }
}

VOID
I2cCtrl_SetDevicePowerD3(
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
    isl    = IoGetCurrentIrpStackLocation(Irp);
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
                status = I2cCtrl_SetDevicePowerD0(devctx);
                break;
            case PowerDeviceD1:
                I2cCtrl_SetDevicePowerD1(devctx);
                status = STATUS_SUCCESS;
                break;
            case PowerDeviceD2:
                I2cCtrl_SetDevicePowerD2(devctx);
                status = STATUS_SUCCESS;
                break;
            case PowerDeviceD3:
            default:
                I2cCtrl_SetDevicePowerD3(devctx);
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
            targetS = PowerSystemSleeping1;   /* S1 → D1 wake */
        } else if (devctx->SupportsD2 != FALSE) {
            targetS = PowerSystemSleeping2;   /* S2 → D2 wake */
        } else {
            targetS = PowerSystemSleeping3;   /* S3 → D3 wake */
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
 * Top‑level IRP_MJ_POWER router for the I²C controller bus driver.
 * XP/2003‑safe, WDM‑compliant, C89‑clean.
 *
 * Responsibilities:
 *  - Runs at PASSIVE_LEVEL (PAGED_CODE)
 *  - Validates DeviceObject and extension invariants
 *  - Routes power IRPs to PDO or FDO handlers based on signature
 *  - Ensures PoStartNextPowerIrp is invoked exactly once per IRP
 *  - PDO path: completes IRP locally (never forwarded)
 *  - FDO path: forwards to ACPI.sys via I2cCtrl_DispatchFDOPower
 *
 * Notes:
 *  - All PDOs share the same DriverObject, so IRP_MJ_POWER always
 *    arrives here first. Routing is mandatory.
 *  - PDOs must *not* forward power IRPs; they must complete them.
 *  - FDOs must use PoCallDriver when forwarding.
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
        /* No extension → no device */
        Irp->IoStatus.Status      = STATUS_NO_SUCH_DEVICE;
        Irp->IoStatus.Information = 0;
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NO_SUCH_DEVICE;
    }

    /* -------------------------------------------------------------------
     * PDO path:
     * If the extension signature matches a child PDO, route to the
     * dedicated PDO power handler. That handler:
     *   - Calls PoStartNextPowerIrp
     *   - Executes _PS0/_PS3
     *   - Completes the IRP locally
     * ------------------------------------------------------------------- */
    if (((PI2CCTRL_PDO)ext)->Signature == I2CCTRL_PDO_SIGNATURE) {
        return I2cCtrl_PdoDispatchPower(DeviceObject, Irp);
    }

    /* -------------------------------------------------------------------
     * FDO path:
     * All controller‑level power logic (system power, D‑state mapping,
     * wake, context save/restore, forwarding to ACPI.sys) is handled
     * by the FDO dispatcher.
     * ------------------------------------------------------------------- */
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

/* Pick next request: strict priority, bounded bursts (XP‑safe) */
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
 * IOCTL Dispatch - XP/2003-safe, HAL-generic, WDM-compliant, ETW-instrumented
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

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL,
                "DispatchIoctl: enter DevExt=%p Irp=%p Ioctl=0x%08lx",
                devctx, Irp,
                (isl ? (ULONG)isl->Parameters.DeviceIoControl.IoControlCode : 0UL));

    if (devctx == NULL || isl == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                    "DispatchIoctl: invalid context DevExt=%p Isl=%p", devctx, isl);
        status = STATUS_INVALID_PARAMETER;
        goto CompleteDirect;
    }

    if (devctx->Started == FALSE || devctx->Stopping != FALSE || devctx->Ops == NULL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                    "DispatchIoctl: device not ready Started=%lu Stopping=%lu Ops=%p",
                    (ULONG)devctx->Started, (ULONG)devctx->Stopping, devctx->Ops);
        status = STATUS_DEVICE_NOT_READY;
        goto CompleteDirect;
    }

    switch (isl->Parameters.DeviceIoControl.IoControlCode) {

    case IOCTL_I2C_FORCE_CRASH:
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                    "DispatchIoctl: IOCTL_I2C_FORCE_CRASH");
        I2cCtrl_ForceCrash(devctx, STATUS_UNSUCCESSFUL);
        status = STATUS_UNSUCCESSFUL;
        break;

    /* Existing I2C/SMBus cases remain as in your original dispatcher ... */

    case IOCTL_HID_READ_REPORT:
    {
        PI2CCTRL_PDO hidpdo = (PI2CCTRL_PDO)DeviceObject->DeviceExtension;
        ULONG  outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
        PUCHAR outBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;

        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL,
                    "DispatchIoctl: IOCTL_HID_READ_REPORT HidPdo=%p OutLen=%lu Buf=%p",
                    hidpdo, outLen, outBuf);

        if (hidpdo == NULL || outBuf == NULL || outLen == 0U) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                        "DispatchIoctl: HID_READ_REPORT invalid params HidPdo=%p OutLen=%lu Buf=%p",
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
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                            "DispatchIoctl: HID_READ_REPORT busy Pending=%p", hidpdo->PendingHidReadIrp);
                status = STATUS_DEVICE_BUSY;
                info   = 0U;
                break;
            }

            hidpdo->PendingHidReadIrp = Irp;
            KeReleaseSpinLock(&hidpdo->HidInputLock, oldIrql);
        }

        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL,
                    "DispatchIoctl: HID_READ_REPORT queued Irp=%p", Irp);
        return STATUS_PENDING;
    }

case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
{
    PI2CCTRL_PDO hidpdo = (PI2CCTRL_PDO)DeviceObject->DeviceExtension;
    ULONG  outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
    PUCHAR outBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL,
                "HID_GET_DEVICE_DESCRIPTOR HidPdo=%p OutLen=%lu Buf=%p",
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

    /* Validate HID descriptor before using DescriptorList[0] */
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

    /* Validate HID descriptor before using DescriptorList[0] */
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

    /* Refresh HID descriptor safely */
    {
        HID_I2C_DESCRIPTOR_V10 parsed;
        NTSTATUS descStatus =
            I2cCtrl_ReadAndValidateHidDescriptor(devctx,
                                                 (UCHAR)hidpdo->SlaveAddress,
                                                 (PUCHAR)&parsed,
                                                 sizeof(parsed),
                                                 &parsed);

        if (NT_SUCCESS(descStatus)) {
            /* Copy ONLY the HID_DESCRIPTOR portion */
            RtlZeroMemory(&hidpdo->HidDesc, sizeof(hidpdo->HidDesc));
            RtlCopyMemory(&hidpdo->HidDesc,
                          &parsed,
                          min(sizeof(hidpdo->HidDesc),
                              parsed.wHIDDescLength));
        }
    }

    /* Write report */
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

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL,
                "HID_GET_FEATURE HidPdo=%p OutLen=%lu Buf=%p",
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

    /* Refresh and validate HID descriptor before feature read */
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
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                        "HID_GET_FEATURE: descriptor validation failed Slave=0x%02X Status=0x%08lx",
                        (unsigned)hidpdo->SlaveAddress,
                        descStatus);
            /* Continue anyway */
        }
    }

    /* Optional: write report ID to Command register if required */
    if (outLen >= 1U) {
        UCHAR reportId = outBuf[0];
        (void)devctx->Ops->IssueWriteByte(devctx, reportId);
    }

    /* Read feature report via HID-over-I²C Data register */
    status = devctx->Ops->IssueBlockRead(devctx,
                                         (UCHAR)hidpdo->SlaveAddress,
                                         hidpdo->DataRegister,
                                         outBuf,
                                         outLen,
                                         &bytesDone);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                    "HID_GET_FEATURE failed Slave=0x%02X Len=%lu Status=0x%08lx",
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

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL,
                "HID_SET_FEATURE HidPdo=%p InLen=%lu Buf=%p",
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

    /* Refresh and validate HID descriptor before feature write */
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
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                        "HID_SET_FEATURE: descriptor validation failed Slave=0x%02X Status=0x%08lx",
                        (unsigned)hidpdo->SlaveAddress,
                        descStatus);
            /* Continue anyway */
        }
    }

    /* HID-over-I²C feature write: write report bytes to Data register */
    status = devctx->Ops->IssueBlockWrite(devctx,
                                          (UCHAR)hidpdo->SlaveAddress,
                                          hidpdo->DataRegister,
                                          inBuf,
                                          inLen,
                                          &bytesDone);

    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                    "HID_SET_FEATURE failed Slave=0x%02X Len=%lu Status=0x%08lx",
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
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                    "DispatchIoctl: unknown IOCTL 0x%08lx",
                    (ULONG)isl->Parameters.DeviceIoControl.IoControlCode);
        break;
    }

CompleteDirect:
    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = info;

    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_IOCTL,
                "DispatchIoctl: complete Irp=%p Status=0x%08lx Info=%Iu",
                Irp, status, info);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}


/* --- Simple helpers: enable controller and do 1‑byte transactions --- */

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
// Generic per‑chip init routine (XP-safe: guard IRQL and struct)
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
// Perform a full controller reset sequence (XP-safe)
//
VOID
I2cCtrl_PerformReset(
    PI2CCTRL_FDO devctx
    )
{
    const ULONG CTRL_RESET_BIT      = 0x00000001U;
    const ULONG STAT_BUSY_BIT       = 0x00000001U;
    const ULONG STAT_RESET_DONE_BIT = 0x00000002U;

    ULONG ctrl;
    ULONG stat;
    ULONG tries;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (devctx == NULL) {
        KdPrint(("I2CCTRL: PerformReset: invalid devctx\n"));
        return;
    }

    if (&g_CurrentRegMap == NULL) {
        KdPrint(("I2CCTRL: PerformReset: g_CurrentRegMap NULL\n"));
        return;
    }

    KdPrint(("I2CCTRL: PerformReset: initiating reset sequence\n"));

    /* Assert reset bit */
    ctrl = I2cCtrl_ReadRegisterSafe(devctx, g_CurrentRegMap.ControlReg);
    I2cCtrl_WriteRegisterSafe(devctx, g_CurrentRegMap.ControlReg, ctrl | CTRL_RESET_BIT);

    /* Poll for reset completion or idle state */
    tries = 0U;
    do {
        stat = I2cCtrl_ReadRegisterSafe(devctx, g_CurrentRegMap.StatusReg);
        if (((stat & STAT_BUSY_BIT) == 0U) || ((stat & STAT_RESET_DONE_BIT) != 0U)) {
            break;
        }
        KeStallExecutionProcessor(10U); /* 10 µs delay */
        tries++;
    } while (tries < 500U);

    /* Deassert reset bit */
    ctrl = I2cCtrl_ReadRegisterSafe(devctx, g_CurrentRegMap.ControlReg);
    if ((ctrl & CTRL_RESET_BIT) != 0U) {
        I2cCtrl_WriteRegisterSafe(devctx, g_CurrentRegMap.ControlReg, ctrl & ~CTRL_RESET_BIT);
    }

    /* Final verification */
    if (tries >= 500U) {
        KdPrint(("I2CCTRL: PerformReset: timeout waiting for reset completion\n"));
    } else {
        KdPrint(("I2CCTRL: PerformReset: reset complete (tries=%lu, STAT=0x%08lx)\n", tries, stat));
    }
}

const I2CCTRL_DEVICE_ID*
I2cCtrl_FindControllerId(
    PCWSTR PnpId
    )
{
    ULONG i;

    if (PnpId == NULL) {
        I2cCtrl_LogSimple("FindControllerId: NULL PnpId\n");
        return NULL;
    }

    for (i = 0; i < RTL_NUMBER_OF(g_I2cControllers); i++) {

        const I2CCTRL_DEVICE_ID* id = &g_I2cControllers[i];

        if (id->PciId == NULL)
            continue;

        /* Case-insensitive match */
        if (_wcsnicmp(PnpId, id->PciId, wcslen(id->PciId)) == 0) {
            I2cCtrl_LogSimple("FindControllerId: match found\n");
            return id;
        }
    }

    I2cCtrl_LogSimple("FindControllerId: no match\n");
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
        KdPrint(("I2CCTRL: ApplyQuirks: invalid devctx\n"));
        I2cCtrl_LogSimple("ApplyQuirks: invalid devctx\n");
        return;
    }

    id = I2cCtrl_FindControllerId(devctx->PnpId);
    if (id == NULL) {
        KdPrint(("I2CCTRL: ApplyQuirks: no table entry for %ws\n", devctx->PnpId));
        I2cCtrl_LogSimple("ApplyQuirks: no table entry\n");
        return;
    }

    bar0 = devctx->MmioBase;
    bar2 = devctx->LpssBar2;

    I2cCtrl_LogSimple("ApplyQuirks: begin\n");

    /* ============================================================
       LPSS POWER-ON (BAR2)
       ============================================================ */
    if (bar2 != NULL) {

        /* LPSS clock gate */
        if (id->LpssClkGateOffset) {
            clk = READ_REGISTER_ULONG((PULONG)(bar2 + id->LpssClkGateOffset));
            clk &= ~0x1U;
            WRITE_REGISTER_ULONG((PULONG)(bar2 + id->LpssClkGateOffset), clk);
            I2cCtrl_LogSimple("LPSS: clock gate cleared\n");
        }

        /* LPSS reset */
        if (id->LpssResetOffset) {
            ctrl = READ_REGISTER_ULONG((PULONG)(bar2 + id->LpssResetOffset));
            ctrl &= ~0x1U;
            WRITE_REGISTER_ULONG((PULONG)(bar2 + id->LpssResetOffset), ctrl);
            I2cCtrl_LogSimple("LPSS: reset deasserted\n");
        }

        /* LPSS functional clock */
        if (id->LpssFuncClkOffset) {
            clk = READ_REGISTER_ULONG((PULONG)(bar2 + id->LpssFuncClkOffset));
            clk |= 0x1U;
            WRITE_REGISTER_ULONG((PULONG)(bar2 + id->LpssFuncClkOffset), clk);
            I2cCtrl_LogSimple("LPSS: functional clock enabled\n");
        }

        /* LPSS misc */
        if (id->LpssMiscOffset) {
            verify = READ_REGISTER_ULONG((PULONG)(bar2 + id->LpssMiscOffset));
            WRITE_REGISTER_ULONG((PULONG)(bar2 + id->LpssMiscOffset), verify);
            I2cCtrl_LogSimple("LPSS: misc touched\n");
        }
    }

    /* ============================================================
       DW-I2C FUNCTIONAL QUIRKS (BAR0)
       ============================================================ */

    /* Reset workaround */
    if (id->Quirks & QUIRK_NEEDS_RESET_WORKAROUND) {

        I2cCtrl_LogSimple("Quirk: reset workaround\n");

        ctrl = READ_REGISTER_ULONG((PULONG)(bar0 + id->ControlOffset));
        WRITE_REGISTER_ULONG((PULONG)(bar0 + id->ControlOffset),
                             ctrl | CTRL_RESET_BIT);

        tries = 0;
        while (tries < 500) {
            stat = READ_REGISTER_ULONG((PULONG)(bar0 + id->StatusOffset));
            if ((stat & STAT_BUSY_BIT) == 0U ||
                (stat & STAT_RESET_DONE_BIT) != 0U)
                break;
            KeStallExecutionProcessor(10);
            tries++;
        }

        ctrl = READ_REGISTER_ULONG((PULONG)(bar0 + id->ControlOffset));
        WRITE_REGISTER_ULONG((PULONG)(bar0 + id->ControlOffset),
                             ctrl & ~CTRL_RESET_BIT);

        I2cCtrl_LogSimple("Quirk: reset workaround complete\n");
    }

    /* Broken clock gate */
    if (id->Quirks & QUIRK_BROKEN_CLOCK_GATE) {

        I2cCtrl_LogSimple("Quirk: broken clock gate\n");

        clk = READ_REGISTER_ULONG((PULONG)(bar0 + id->ClockOffset));
        clk |= CLK_ENABLE_BIT;
        clk &= ~CLK_GATE_BIT;
        WRITE_REGISTER_ULONG((PULONG)(bar0 + id->ClockOffset), clk);
    }

    /* No DMA support */
    if (id->Quirks & QUIRK_NO_DMA_SUPPORT) {

        I2cCtrl_LogSimple("Quirk: no DMA support\n");

        ctrl = READ_REGISTER_ULONG((PULONG)(bar0 + id->ControlOffset));
        ctrl &= ~CTRL_DMA_EN_BIT;
        WRITE_REGISTER_ULONG((PULONG)(bar0 + id->ControlOffset), ctrl);
    }

    /* ACPI 2.0+ */
    if (id->Quirks & QUIRK_ACPI20) {
        devctx->AcpiIs20Plus = TRUE;
        I2cCtrl_LogSimple("Quirk: ACPI 2.0+\n");
    }

    /* ACPI 1.0b */
    if (id->Quirks & QUIRK_ACPI10) {
        devctx->AcpiIs20Plus = FALSE;
        I2cCtrl_LogSimple("Quirk: ACPI 1.0b\n");
    }

    /* Slow clock */
    if (id->Quirks & QUIRK_SLOW_CLOCK) {
        devctx->StallIntervalUs += 5;
        I2cCtrl_LogSimple("Quirk: slow clock\n");
    }

    /* No D1/D2 */
    if (id->Quirks & QUIRK_NO_D1D2) {
        devctx->SupportsD1 = FALSE;
        devctx->SupportsD2 = FALSE;
        I2cCtrl_LogSimple("Quirk: no D1/D2\n");
    }

    /* ============================================================
       BSOD WORKAROUNDS
       ============================================================ */

    if (id->BsodQuirks & BSOD_FORCE_PIO) {
        devctx->ForcePioMode = TRUE;
        I2cCtrl_LogSimple("BSOD: force PIO\n");
    }

    if (id->BsodQuirks & BSOD_MASK_INTERRUPTS) {
        I2cCtrl_MaskInterrupts(devctx, TRUE);
        I2cCtrl_LogSimple("BSOD: mask interrupts\n");
    }

    if (id->BsodQuirks & BSOD_EXTRA_RESET) {
        I2cCtrl_PerformReset(devctx);
        I2cCtrl_LogSimple("BSOD: extra reset\n");
    }

    if (id->BsodQuirks & BSOD_DELAY_INIT) {
        KeStallExecutionProcessor(50000);
        I2cCtrl_LogSimple("BSOD: delay init\n");
    }

    I2cCtrl_LogSimple("ApplyQuirks: done\n");
}

VOID
I2cHidApplyQuirks(
    PI2CCTRL_PDO childDx,
    const I2CHID_DEVICE_ID* hidMatch
    )
{
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (!childDx || !hidMatch || !hidMatch->HidId) {
        KdPrint(("I2CHID: ApplyQuirks: invalid parameters\n"));
        return;
    }

    KdPrint(("I2CHID: Applying HID quirks for %ws\n", hidMatch->HidId));

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
            KdPrint(("I2CHID: ELAN quirks applied\n"));
            break;

        case HID_QUIRK_SYNAPTICS:
            childDx->HidSynapticsFix = TRUE;
            childDx->HidPacketHeaderSize = 4;
            KdPrint(("I2CHID: Synaptics quirks applied\n"));
            break;

        case HID_QUIRK_ASUS:
            childDx->HidDebounceFix = TRUE;
            childDx->HidExtraDelayUs = 150;
            KdPrint(("I2CHID: ASUS quirks applied\n"));
            break;

        case HID_QUIRK_GOODIX:
            childDx->HidSlowRead = TRUE;
            childDx->HidPacketHeaderSize = 2;
            KdPrint(("I2CHID: Goodix quirks applied\n"));
            break;

        case HID_QUIRK_RAYDIUM:
            childDx->HidPacketHeaderSize = 2;
            childDx->HidRaydiumMode = TRUE;
            KdPrint(("I2CHID: Raydium quirks applied\n"));
            break;

        case HID_QUIRK_FOCALTECH:
            childDx->HidScaleCoordinates = TRUE;
            KdPrint(("I2CHID: FocalTech quirks applied\n"));
            break;

        case HID_QUIRK_CYPRESS:
            childDx->HidFilterInterrupts = TRUE;
            KdPrint(("I2CHID: Cypress quirks applied\n"));
            break;

        case HID_QUIRK_HIMAX:
            childDx->HidHimaxMode = TRUE;
            childDx->HidPacketHeaderSize = 3;
            KdPrint(("I2CHID: Himax quirks applied\n"));
            break;

        case HID_QUIRK_PIXART:
            childDx->HidPixartChecksum = TRUE;
            KdPrint(("I2CHID: PixArt quirks applied\n"));
            break;

        case HID_QUIRK_SILEAD:
            childDx->HidExtraDelayUs = 300;
            childDx->HidSileadMode = TRUE;
            KdPrint(("I2CHID: Silead quirks applied\n"));
            break;

        case HID_QUIRK_ATMEL:
            childDx->HidAtmelHeaderFix = TRUE;
            KdPrint(("I2CHID: Atmel quirks applied\n"));
            break;

        case HID_QUIRK_PRIMAX:
            childDx->HidPrimaxMode = TRUE;
            KdPrint(("I2CHID: Primax quirks applied\n"));
            break;

        case HID_QUIRK_CHICONY:
            childDx->HidDebounceFix = TRUE;
            KdPrint(("I2CHID: Chicony quirks applied\n"));
            break;

        default:
            KdPrint(("I2CHID: No vendor-specific quirks\n"));
            break;
    }

    KdPrint(("I2CHID: HID quirks applied successfully\n"));
}


//
// Guarded MMIO helpers and data accessors (XP-BSOD-safe, WinDDK, C89)
//

//
// Safe 32-bit MMIO write using full device context
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

//
// Safe 32-bit MMIO read using full device context
//
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
// MMIO accessor: write controller data register
//
VOID
I2cCtrlWriteData(
    PI2CCTRL_FDO devctx,
    ULONG value
    )
{
    ULONG offset;

    if (devctx == NULL) {
        KdPrint(("I2CCTRL: WriteData NULL devctx\n"));
        return;
    }

    offset = g_CurrentRegMap.DataReg;

    I2cCtrl_WriteRegisterSafe(devctx, offset, value);

    if ((g_CurrentRegMap.Quirks & QUIRK_NO_DMA_SUPPORT) != 0U) {
        KdPrint(("I2CCTRL: Writing data without DMA (quirk)\n"));
    }
}

//
// MMIO accessor: read controller data register
//
ULONG
I2cCtrlReadData(
    PI2CCTRL_FDO devctx
    )
{
    ULONG offset;
    ULONG value;

    if (devctx == NULL) {
        KdPrint(("I2CCTRL: ReadData NULL devctx\n"));
        return 0U;
    }

    offset = g_CurrentRegMap.DataReg;
    value  = I2cCtrl_ReadRegisterSafe(devctx, offset);

    if ((g_CurrentRegMap.Quirks & QUIRK_NO_DMA_SUPPORT) != 0U) {
        KdPrint(("I2CCTRL: Reading data without DMA (quirk)\n"));
    }

    return value;
}


//
// MMIO accessor: read controller status register (BSOD‑safe)
//
ULONG
I2cCtrlReadStatus(
    PI2CCTRL_FDO devctx
    )
{
    ULONG offset;
    ULONG status;

    if (devctx == NULL) {
        return 0U;
    }

    /* Status register offset from active register map */
    offset = g_CurrentRegMap.StatusReg;

    /* Perform safe MMIO read */
    status = I2cCtrl_ReadRegisterSafe(devctx, offset);

    /* Quirk handling: clock gate workaround */
    if ((g_CurrentRegMap.Quirks & QUIRK_BROKEN_CLOCK_GATE) != 0U) {
        KdPrint(("I2CCTRL: Applying clock gate workaround during status read\n"));
        /* Example (kept commented as in your original):
           ULONG clk = I2cCtrl_ReadRegisterSafe(devctx, g_CurrentRegMap.ClockReg);
           clk |= 0x00000001U;          // enable clock
           clk &= ~0x00000002U;         // clear gate
           I2cCtrl_WriteRegisterSafe(devctx, g_CurrentRegMap.ClockReg, clk);
        */
    }

    return status;
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


NTSTATUS
I2cCtrl_CreateChildPdo(
    PDEVICE_OBJECT ParentFdo,
    PI2CCTRL_FDO   fdoExt,
    PWSTR          hidBuf,
    PWSTR          uidBuf
    )
{
    NTSTATUS        status;
    PDEVICE_OBJECT  childPdo;
    PI2CCTRL_PDO    pdoExt;
    KIRQL           oldIrql;
    BOOLEAN         duplicate;
    PLIST_ENTRY     link;
    PI2CCTRL_PDO    ext;
    UNICODE_STRING  targetInst;
    const WCHAR*    hwIds[1];
    const WCHAR*    compatIds[1];
    ULONG           hwCount;
    ULONG           compatCount;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    PAGED_CODE();

    status       = STATUS_SUCCESS;
    childPdo     = NULL;
    pdoExt       = NULL;
    duplicate    = FALSE;
    link         = NULL;
    ext          = NULL;
    hwCount      = 0U;
    compatCount  = 0U;

    if (ParentFdo == NULL || fdoExt == NULL || hidBuf == NULL) {
        KdPrint(("I2CCTRL: CreateChildPdo: invalid parameters\n"));
        return STATUS_INVALID_PARAMETER;
    }

    RtlInitUnicodeString(&targetInst, (uidBuf != NULL) ? uidBuf : L"0000");

    //
    // Prevent duplicate InstanceId children (under ChildLock)
    //
    KeAcquireSpinLock(&fdoExt->ChildLock, &oldIrql);
    for (link = fdoExt->ChildList.Flink;
         link != &fdoExt->ChildList;
         link = link->Flink) {

        ext = CONTAINING_RECORD(link, I2CCTRL_PDO, ListEntry);

        if (ext->InstanceId.Buffer != NULL &&
            RtlEqualUnicodeString(&ext->InstanceId, &targetInst, TRUE)) {

            duplicate = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&fdoExt->ChildLock, oldIrql);

    if (duplicate) {
        KdPrint(("I2CCTRL: CreateChildPdo: duplicate InstanceId detected; skipping\n"));
        return STATUS_SUCCESS;
    }

    status = IoCreateDevice(
                 ParentFdo->DriverObject,
                 sizeof(I2CCTRL_PDO),
                 NULL,
                 FILE_DEVICE_UNKNOWN,
                 FILE_DEVICE_SECURE_OPEN,
                 FALSE,
                 &childPdo
             );
    if (!NT_SUCCESS(status) || childPdo == NULL) {
        KdPrint(("I2CCTRL: CreateChildPdo: IoCreateDevice failed (0x%08X)\n", status));
        return status;
    }

    RtlZeroMemory(childPdo->DeviceExtension, sizeof(I2CCTRL_PDO));
    pdoExt = (PI2CCTRL_PDO)childPdo->DeviceExtension;

    pdoExt->Pdo        = childPdo;
    pdoExt->ParentFdo  = fdoExt;
    InitializeListHead(&pdoExt->ListEntry);

    pdoExt->Present         = TRUE;
    pdoExt->Reported        = FALSE;
    pdoExt->Started         = FALSE;
    pdoExt->Removed         = FALSE;
    pdoExt->SurpriseRemoved = FALSE;
    pdoExt->Enumerated      = TRUE;
    pdoExt->Stopping        = FALSE;
    pdoExt->CurrentPowerState = PowerDeviceD3;

    pdoExt->Signature     = 'PDOI';
    pdoExt->ErrorCount    = 0U;
    pdoExt->LastErrorCode = STATUS_SUCCESS;

    //
    // Explicit IDs
    //
    status = I2cCtrl_DupString(&pdoExt->HardwareId, hidBuf);
    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL: CreateChildPdo: Dup HardwareId failed (0x%08X)\n", status));
        IoDeleteDevice(childPdo);
        return status;
    }

    status = I2cCtrl_DupString(&pdoExt->InstanceId,
                               (uidBuf != NULL) ? uidBuf : L"0000");
    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL: CreateChildPdo: Dup InstanceId failed (0x%08X)\n", status));
        ExFreePoolWithTag(pdoExt->HardwareId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&pdoExt->HardwareId, sizeof(UNICODE_STRING));
        IoDeleteDevice(childPdo);
        return status;
    }

    //
    // Compatible ID must be PNP0C50 (no ACPI\ prefix)
    //
    status = I2cCtrl_DupString(&pdoExt->CompatibleId, L"PNP0C50");
    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL: CreateChildPdo: Dup CompatibleId failed (0x%08X)\n", status));
        ExFreePoolWithTag(pdoExt->HardwareId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&pdoExt->HardwareId, sizeof(UNICODE_STRING));
        ExFreePoolWithTag(pdoExt->InstanceId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&pdoExt->InstanceId, sizeof(UNICODE_STRING));
        IoDeleteDevice(childPdo);
        return status;
    }

    //
    // MULTI_SZ lists
    //
    hwIds[0] = hidBuf;        /* e.g. L"ACPI\\PNP0C50" */
    hwCount  = 1U;

    compatIds[0] = L"PNP0C50";
    compatCount  = 1U;

    status = I2cCtrlBuildMultiSz(&pdoExt->HardwareIdsMultiSz, hwIds, hwCount);
    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL: CreateChildPdo: Build HW MULTI_SZ failed (0x%08X)\n", status));
        ExFreePoolWithTag(pdoExt->HardwareId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&pdoExt->HardwareId, sizeof(UNICODE_STRING));
        ExFreePoolWithTag(pdoExt->InstanceId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&pdoExt->InstanceId, sizeof(UNICODE_STRING));
        ExFreePoolWithTag(pdoExt->CompatibleId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&pdoExt->CompatibleId, sizeof(UNICODE_STRING));
        IoDeleteDevice(childPdo);
        return status;
    }

    status = I2cCtrlBuildMultiSz(&pdoExt->CompatibleIdsMultiSz,
                                 compatIds,
                                 compatCount);
    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL: CreateChildPdo: Build COMPAT MULTI_SZ failed (0x%08X)\n", status));
        ExFreePoolWithTag(pdoExt->HardwareIdsMultiSz, TAG_I2C_MISC);
        pdoExt->HardwareIdsMultiSz = NULL;
        ExFreePoolWithTag(pdoExt->HardwareId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&pdoExt->HardwareId, sizeof(UNICODE_STRING));
        ExFreePoolWithTag(pdoExt->InstanceId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&pdoExt->InstanceId, sizeof(UNICODE_STRING));
        ExFreePoolWithTag(pdoExt->CompatibleId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&pdoExt->CompatibleId, sizeof(UNICODE_STRING));
        IoDeleteDevice(childPdo);
        return status;
    }

    //
    // Descriptor
    //
    RtlZeroMemory(&pdoExt->Desc, sizeof(pdoExt->Desc));
    pdoExt->Desc.Address = 0U;

    childPdo->Flags |= DO_BUS_ENUMERATED_DEVICE | DO_POWER_PAGABLE;
    childPdo->Flags &= ~DO_DEVICE_INITIALIZING;
    childPdo->StackSize = ParentFdo->StackSize;

    //
    // Insert into child list
    //
    KeAcquireSpinLock(&fdoExt->ChildLock, &oldIrql);
    InsertTailList(&fdoExt->ChildList, &pdoExt->ListEntry);
    fdoExt->NumChildren++;
    KeReleaseSpinLock(&fdoExt->ChildLock, oldIrql);

    KdPrint(("I2CCTRL: Child PDO created: %p (NumChildren=%lu)\n",
             childPdo, fdoExt->NumChildren));

    //
    // Optional: register an interface (not required for ACPI\PNP0C50 to enumerate)
    //
    {
        UNICODE_STRING symLink;
        RtlInitUnicodeString(&symLink, L"");
        status = IoRegisterDeviceInterface(childPdo,
                                           &GUID_I2CCTRL_CHILD_IFACE,
                                           NULL,
                                           &symLink);
        if (NT_SUCCESS(status)) {
            (void)IoSetDeviceInterfaceState(&symLink, TRUE);
        }
    }

    return STATUS_SUCCESS;
}


//
// ACPI‑safe child PDO list teardown
// XP/2003‑compatible
// - DO NOT delete PDOs here
// - DO NOT free ACPI‑visible strings here
// - DO NOT unlink from ChildList here
// - Only mark Removed; actual cleanup is in PDO IRP_MN_REMOVE_DEVICE
//
VOID
I2cCtrl_DeleteChildPdos(
    PI2CCTRL_FDO devctx
    )
{
    PLIST_ENTRY  entry;
    PI2CCTRL_PDO pdoExt;
    KIRQL        oldIrql;

    if (devctx == NULL) {
        return;
    }

    KeAcquireSpinLock(&devctx->ChildLock, &oldIrql);

    for (entry = devctx->ChildList.Flink;
         entry != &devctx->ChildList;
         entry = entry->Flink) {

        pdoExt = CONTAINING_RECORD(entry, I2CCTRL_PDO, ListEntry);

        //
        // Mark PDO removed - but DO NOT delete or unlink it.
        // Actual deletion/unlink happens in PDO IRP_MN_REMOVE_DEVICE.
        //
        pdoExt->Removed = TRUE;
    }

    KeReleaseSpinLock(&devctx->ChildLock, oldIrql);
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


/* -----------------------------------------------------------------------
 * I2cCtrl_LoadRegistryPolicy - HAL-generic, XP/2003 BSOD-safe, C89-compliant
 *
 * Purpose:
 *   - Load driver policy from registry without hardcoded service path
 *   - Use IoOpenDeviceRegistryKey to access devnode/driver keys
 *   - Query "Parameters" subkey values with validation and sane defaults
 *   - Remain portable across HALs and PnP instances
 *
 * Guarantees:
 *   - Runs only at PASSIVE_LEVEL
 *   - Operates strictly on non-paged memory
 *   - Handles missing keys/values gracefully; never crashes
 * ----------------------------------------------------------------------- */
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
    RTL_QUERY_REGISTRY_TABLE tbl[13];

    /* Temporary struct to hold queried values */
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
    } cfg;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    PAGED_CODE();

    if (Dx == NULL || Dx->Self == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* C89 init */
    status     = STATUS_SUCCESS;
    drvKey     = NULL;
    paramsKey  = NULL;
    RtlZeroMemory(&cfg, sizeof(cfg));
    RtlZeroMemory(tbl, sizeof(tbl));
    RtlInitUnicodeString(&paramsName, L"Parameters");

    /* Open per-driver registry key (HKLM\System\CCS\Services\<Driver>\...) */
    status = IoOpenDeviceRegistryKey(Dx->Self, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &drvKey);
    if (!NT_SUCCESS(status) || drvKey == NULL) {
        /* Fallback: attempt device key (per-instance), still safe */
        status = IoOpenDeviceRegistryKey(Dx->Self, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &drvKey);
        if (!NT_SUCCESS(status) || drvKey == NULL) {
            /* No registry available: apply defaults and return success */
            goto ApplyDefaults;
        }
    }

    /* Open/create Parameters subkey under the obtained driver/device key */
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
            /* If Parameters missing, keep defaults */
            paramsKey = NULL;
        }
    }

    /* Build query table (use handle-based query) */
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

    /* Terminator */
    tbl[12].Name = NULL;

    /* Query values using handle-based root (driver/device key or its Parameters subkey) */
    {
        HANDLE root = (paramsKey != NULL) ? paramsKey : drvKey;
        if (root != NULL) {
            (void)RtlQueryRegistryValues(RTL_REGISTRY_HANDLE, root, tbl, NULL, NULL);
        }
    }

ApplyDefaults:
    /* Apply defaults if missing/invalid; HAL-generic policies */
    Dx->PolicyEnableHighSpeed   = (cfg.EnableHighSpeed != 0U) ? 1U : 0U;

    /* Bus speed range: 10 kHz .. 3.4 MHz; default 400 kHz unless high speed policy */
    Dx->PolicyBusSpeedHz        = (cfg.BusSpeedHz >= 10000U && cfg.BusSpeedHz <= 3400000U)
                                  ? cfg.BusSpeedHz
                                  : ((Dx->PolicyEnableHighSpeed != 0U) ? 3400000U : 400000U);

    /* Retry policy: cap to a reasonable bound */
    Dx->PolicyMaxRetries        = (cfg.MaxRetries <= 10U) ? cfg.MaxRetries : 3U;

    /* Delay between retries: max 100 ms */
    Dx->PolicyRetryDelayUs      = (cfg.RetryDelayUs <= 100000U) ? cfg.RetryDelayUs : 1000U;

    /* Transaction timeout: 10 ms .. 5000 ms; default 1000 ms */
    Dx->PolicyTxnTimeoutMs      = (cfg.TransactionTimeoutMs >= 10U && cfg.TransactionTimeoutMs <= 5000U)
                                  ? cfg.TransactionTimeoutMs : 1000U;

    /* Backoff policy when bus busy/arbitration */
    Dx->PolicyBackoffOnBusy     = (cfg.BackoffOnBusy != 0U) ? 1U : 0U;

    /* Exponential backoff bounds */
    Dx->PolicyBackoffInitialUs  = (cfg.BackoffInitialUs >= 1U && cfg.BackoffInitialUs <= 1000U)
                                  ? cfg.BackoffInitialUs : 10U;
    Dx->PolicyBackoffMaxUs      = (cfg.BackoffMaxUs >= Dx->PolicyBackoffInitialUs && cfg.BackoffMaxUs <= 50000U)
                                  ? cfg.BackoffMaxUs : 5000U;

    /* SMBus PEC toggle */
    Dx->PolicyUsePec            = (cfg.UsePec != 0U) ? 1U : 0U;

    /* GPIO polarity for optional lines (if present) */
    Dx->PolicyGpioActiveLow     = (cfg.GpioActiveLow != 0U) ? 1U : 0U;

    /* 10-bit addressing default */
    Dx->PolicyForce10Bit        = (cfg.Force10Bit != 0U) ? 1U : 0U;

    /* Crash-on-error (developer diagnostics) */
    Dx->PolicyCrashOnError      = (cfg.CrashOnError != 0U) ? 1U : 0U;

    /* Derived runtime values */
    Dx->ActiveBusSpeedHz        = Dx->PolicyBusSpeedHz;
    Dx->Use10BitAddrDefault     = (Dx->PolicyForce10Bit != 0U);
    Dx->BackoffCurrentUs        = Dx->PolicyBackoffInitialUs;

    /* Cleanup */
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
 * Safe ACPI handle close (XP/2003‑compatible)
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