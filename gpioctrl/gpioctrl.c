/* gpioctrl.c
 * GPIO Controller Driver (gpioctrl.sys)
 * WinDDK 7.1.0 - XP/2003 build environment - C89 compliant
 *
 * Matches gpioctrl_ext.h:
 * - DriverEntry, Unload, AddDevice
 * - Dispatch routines: PnP, Power, DeviceControl, Create/Close
 * - PnP helpers: StartDevice, StopDevice, RemoveDevice
 * - Registry policy loader
 * - MMIO helpers: GpioRegRead/GpioRegWrite
 * - ISR/DPC
 * - IOCTL handlers: ReadPin, WritePin, ConfigurePin, QueryCaps
 */

#include <ntddk.h>
#include "gpioctrl_ext.h"
#include <stdarg.h>   // <-- required for va_start, va_end
#include <stdio.h>    // <-- required for _vsnprintf

PDEVICE_OBJECT Gpioctrl_GlobalDeviceObject = NULL;
GPIOCTRL_GLOBAL g_GpioCtrlGlobal;

/* -----------------------------------------------------------------------
 * DriverEntry - GPIO Controller driver entry point
 *
 * XP/2003 BSOD-safe, C89-compliant.
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

    /* Initialize any global GPIO state here if you have it */
    RtlZeroMemory(&g_GpioCtrlGlobal, sizeof(g_GpioCtrlGlobal));

    /* Default all IRP major functions to a safe handler */
    for (i = 0U; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = GpioCtrl_DispatchCreateClose;
    }

    /* Assign supported dispatch routines */
    DriverObject->MajorFunction[IRP_MJ_PNP]            = GpioCtrl_DispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]          = GpioCtrl_DispatchPower;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = GpioCtrl_DispatchIoctl;

    /* Set unload routine early */
    DriverObject->DriverUnload = GpioCtrl_Unload;

    /* AddDevice must be available */
    if (DriverObject->DriverExtension == NULL) {
        return STATUS_UNSUCCESSFUL;
    }

    /* Assign AddDevice (PnP entry point) */
    DriverObject->DriverExtension->AddDevice = GpioCtrl_AddDevice;

    /* Register lifecycle helpers in our own global struct */
    g_GpioCtrlGlobal.StartDevice = GpioCtrl_StartDevice;
    g_GpioCtrlGlobal.StopDevice  = GpioCtrl_StopDevice;

    return STATUS_SUCCESS;
}

//
// Table of supported GPIO controllers (explicit ACPI/PCI devices)
// Match against full HWID substrings only
//
const GPIOCTRL_DEVICE_ID g_GpioControllers[] = {

    /* ACPI-based GPIO controllers */
    { L"ACPI\\INT3450",
      0x00,0x04,0x08,0x0C,       /* BAR0 GPIO registers */
      0,0,0,0,                   /* No LPSS BAR2 */
      QUIRK_ACPI20, BSOD_NONE },

    { L"ACPI\\INT3451",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_FORCE_PIO },

    { L"ACPI\\AMD0030",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE },

    /* NEW: ACPI Skylake/Kaby Lake GPIO controller */
    { L"ACPI\\INT34BB",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI20, BSOD_NONE },


    /* PCI-based Intel GPIO controllers WITH LPSS BAR2 */
    { L"PCI\\VEN_8086&DEV_9D35",
      0x00,0x04,0x08,0x0C,       /* BAR0 GPIO MMIO */
      0x200,0x204,0x208,0x20C,   /* LPSS BAR2 */
      QUIRK_NEEDS_RESET_WORKAROUND, BSOD_EXTRA_RESET },

    { L"PCI\\VEN_8086&DEV_9D36",
      0x10,0x14,0x18,0x1C,
      0x200,0x204,0x208,0x20C,
      QUIRK_BROKEN_CLOCK_GATE, BSOD_MASK_INTERRUPTS },

    { L"PCI\\VEN_8086&DEV_9D37",
      0x20,0x24,0x28,0x2C,
      0x200,0x204,0x208,0x20C,
      QUIRK_NO_DMA_SUPPORT, BSOD_FORCE_PIO },


    /* Legacy PCI controllers requiring ACPI 1.0b fallback */
    { L"PCI\\VEN_8086&DEV_A123",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI10, BSOD_DELAY_INIT },

    { L"PCI\\VEN_8086&DEV_A124",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_ACPI10, BSOD_DELAY_INIT },


    /* Generic Intel GPIO controllers (NO LPSS unless proven otherwise) */
    { L"PCI\\VEN_8086&DEV_A2F0",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_A2F1",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_A2F2",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE },

    { L"PCI\\VEN_8086&DEV_A2F3",
      0x00,0x04,0x08,0x0C,
      0,0,0,0,
      QUIRK_NONE, BSOD_NONE }
};

const ULONG g_GpioControllersCount =
    sizeof(g_GpioControllers) / sizeof(g_GpioControllers[0]);


/* ---------------------------------------------------------------------------
   Unload
   --------------------------------------------------------------------------- */
VOID
GpioCtrl_Unload(
    IN PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
GpioCtrl_AddDevice(
    IN PDRIVER_OBJECT  DriverObject,
    IN PDEVICE_OBJECT  PhysicalDeviceObject
    )
{
    NTSTATUS status;
    PDEVICE_OBJECT fdo;
    PGPIOCTRL_FDO ext;
    PDEVICE_OBJECT lowerDevice;

    GpioCtrl_Log("AddDevice: Entered for PDO %p\n", PhysicalDeviceObject);

    /* Create FDO */
    status = IoCreateDevice(
                 DriverObject,
                 sizeof(GPIOCTRL_FDO),
                 NULL,
                 FILE_DEVICE_UNKNOWN,
                 FILE_DEVICE_SECURE_OPEN,
                 FALSE,
                 &fdo);

    if (!NT_SUCCESS(status)) {
        GpioCtrl_Log("AddDevice: IoCreateDevice FAILED (0x%08X)\n", status);
        return status;
    }

    GpioCtrl_Log("AddDevice: Created FDO %p\n", fdo);

    /* Zero extension BEFORE attaching */
    ext = (PGPIOCTRL_FDO)fdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));

    ext->Self = fdo;
    ext->Pdo  = PhysicalDeviceObject;

    /* Attach to device stack */
    lowerDevice = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (lowerDevice == NULL) {
        GpioCtrl_Log("AddDevice: IoAttachDeviceToDeviceStack FAILED\n");
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    ext->LowerDevice = lowerDevice;

    GpioCtrl_Log("AddDevice: Attached FDO %p to lower device %p\n",
                 fdo, lowerDevice);

    /* Initialize static capabilities (defaults) */
    ext->Signature          = GPIOCTRL_FDO_SIGNATURE;
    ext->SupportsPull       = 1;
    ext->SupportsInterrupts = 1;
    ext->SupportsDebounce   = 1;
    ext->PinCount           = 32;
    ext->DebounceDefaultMs  = 10;
    ext->CrashOnError       = 0;

    /* Synchronization primitives */
    KeInitializeSpinLock(&ext->RegLock);
    KeInitializeSpinLock(&ext->IsrLogLock);

    /* ISR/DPC */
    KeInitializeDpc(&ext->IsrDpc, GpioCtrl_Dpc, ext);
    ext->PendingIntMask = 0;

    /* ISR log buffer */
    ext->IsrLogHead = 0;
    ext->IsrLogTail = 0;

    /* Wait-for-interrupt support */
    KeInitializeEvent(&ext->WaitEvent, NotificationEvent, FALSE);
    KeInitializeSpinLock(&ext->WaitLock);
    ext->LastFiredPin = (ULONG)-1;

    /* Mark device ready */
    fdo->Flags |= DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    GpioCtrl_Log("AddDevice: Completed successfully for FDO %p\n", fdo);

    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   PnP dispatch (standalone FDO - handle and complete)
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_DispatchPnp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PGPIOCTRL_FDO ext;
    PIO_STACK_LOCATION isl;
    NTSTATUS status;

    ext = (PGPIOCTRL_FDO)DeviceObject->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    GpioCtrl_Log("PnP: Entered for device %p, MinorFunction=0x%02X\n",
                 DeviceObject, isl->MinorFunction);

    switch (isl->MinorFunction) {

    case IRP_MN_START_DEVICE:
        GpioCtrl_Log("PnP: IRP_MN_START_DEVICE -> calling StartDevice\n");
        return g_GpioCtrlGlobal.StartDevice(DeviceObject, Irp);

    case IRP_MN_STOP_DEVICE:
        GpioCtrl_Log("PnP: IRP_MN_STOP_DEVICE -> calling StopDevice\n");
        return g_GpioCtrlGlobal.StopDevice(DeviceObject, Irp);

    case IRP_MN_REMOVE_DEVICE:
        GpioCtrl_Log("PnP: IRP_MN_REMOVE_DEVICE -> calling RemoveDevice\n");
        return GpioCtrl_RemoveDevice(DeviceObject, Irp);

    default:
        GpioCtrl_Log("PnP: Unhandled MinorFunction=0x%02X -> completing SUCCESS\n",
                     isl->MinorFunction);

        status = STATUS_SUCCESS;
        break;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    GpioCtrl_Log("PnP: Completed default IRP for device %p, status=0x%08X\n",
                 DeviceObject, status);

    return status;
}


/* ---------------------------------------------------------------------------
   Power dispatch (standalone FDO - no lower device)
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_DispatchPower(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PGPIOCTRL_FDO ext;
    PIO_STACK_LOCATION isl;
    NTSTATUS status = STATUS_SUCCESS;

    ext = (PGPIOCTRL_FDO)DeviceObject->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    GpioCtrl_Log("Power: Entered for device %p, MinorFunction=0x%02X\n",
                 DeviceObject, isl->MinorFunction);

    //
    // Required for XP/2003 power IRP sequencing
    //
    PoStartNextPowerIrp(Irp);

    //
    // If we have a lower device, forward the IRP
    //
    if (ext->LowerDevice != NULL) {

        GpioCtrl_Log("Power: Forwarding to lower device %p\n",
                     ext->LowerDevice);

        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(ext->LowerDevice, Irp);
    }

    //
    // Standalone FDO: complete here
    //
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    GpioCtrl_Log("Power: Completed locally for device %p, status=0x%08X\n",
                 DeviceObject, status);

    return status;
}

/* ---------------------------------------------------------------------------
   Create/Close
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_DispatchCreateClose(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   Device Control
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_DispatchIoctl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    NTSTATUS status;
    PIO_STACK_LOCATION isl;
    PGPIOCTRL_FDO ext;
    ULONG code;

    isl  = IoGetCurrentIrpStackLocation(Irp);
    ext  = (PGPIOCTRL_FDO)DeviceObject->DeviceExtension;
    code = isl->Parameters.DeviceIoControl.IoControlCode;

    GpioCtrl_Log("Ioctl: Entered for device %p, IoctlCode=0x%08X\n",
                 DeviceObject, code);

    /* Default values */
    status = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;

    /* Device not started yet */
    if (!ext->Started) {

        GpioCtrl_Log("Ioctl: Device %p not started -> STATUS_DEVICE_NOT_READY\n",
                     DeviceObject);

        status = STATUS_DEVICE_NOT_READY;

    } else {

        switch (code) {

        case IOCTL_GPIOCTRL_READ_PIN:
            GpioCtrl_Log("Ioctl: IOCTL_GPIOCTRL_READ_PIN\n");
            status = GpioCtrl_IoctlReadPin(ext, Irp);
            break;

        case IOCTL_GPIOCTRL_WRITE_PIN:
            GpioCtrl_Log("Ioctl: IOCTL_GPIOCTRL_WRITE_PIN\n");
            status = GpioCtrl_IoctlWritePin(ext, Irp);
            break;

        case IOCTL_GPIOCTRL_CONFIGURE_PIN:
            GpioCtrl_Log("Ioctl: IOCTL_GPIOCTRL_CONFIGURE_PIN\n");
            status = GpioCtrl_IoctlConfigurePin(ext, Irp);
            break;

        case IOCTL_GPIOCTRL_QUERY_CAPS:
            GpioCtrl_Log("Ioctl: IOCTL_GPIOCTRL_QUERY_CAPS\n");
            status = GpioCtrl_IoctlQueryCaps(ext, Irp);
            break;

        case IOCTL_GPIOCTRL_FORCE_CRASH:
            GpioCtrl_Log("Ioctl: IOCTL_GPIOCTRL_FORCE_CRASH -> triggering bugcheck\n");

            KeBugCheckEx(
                MANUALLY_INITIATED_CRASH,
                0x474F5043UL, /* "GPOC" */
                ext->Signature,
                0,
                0
            );
            /* No return — system will bugcheck */
            break;

        default:
            GpioCtrl_Log("Ioctl: Unknown IoctlCode=0x%08X -> STATUS_INVALID_DEVICE_REQUEST\n",
                         code);
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }
    }

    /* Complete IRP once, unless bugcheck occurred */
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    GpioCtrl_Log("Ioctl: Completed for device %p, status=0x%08X\n",
                 DeviceObject, status);

    return status;
}

/* ---------------------------------------------------------------------------
   StartDevice: parse resources, map MMIO, enable LPSS, connect IRQ
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_StartDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    NTSTATUS status;
    PGPIOCTRL_FDO ext;
    PCM_RESOURCE_LIST resTranslated;
    PIO_STACK_LOCATION isl;
    ULONG i, j;
    BOOLEAN hasPmcWindow;
    const GPIOCTRL_DEVICE_ID* ctrlProfile;
    WCHAR hwidBuf[256];
    ULONG quirks;
    ULONG bsodFlags;

    ext = (PGPIOCTRL_FDO)DeviceObject->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    hasPmcWindow = FALSE;
    ctrlProfile  = NULL;
    quirks       = QUIRK_NONE;
    bsodFlags    = BSOD_NONE;

    RtlZeroMemory(hwidBuf, sizeof(hwidBuf));

    GpioCtrl_Log("StartDevice: Entered for device %p\n", DeviceObject);

    /* -----------------------------------------------------------------------
       0) Forward START_DEVICE to lower driver
       ----------------------------------------------------------------------- */
    IoCopyCurrentIrpStackLocationToNext(Irp);
    status = IoCallDriver(ext->LowerDevice, Irp);

    if (!NT_SUCCESS(status)) {
        GpioCtrl_Log("StartDevice: Lower driver failed, status=0x%08X\n", status);
        return status;
    }

    GpioCtrl_Log("StartDevice: Lower driver completed successfully\n");

    /* -----------------------------------------------------------------------
       1) Match Hardware ID against controller profiles
       ----------------------------------------------------------------------- */
    {
        UNICODE_STRING oneId;
        const WCHAR* p;
        ULONG len;

        status = IoGetDeviceProperty(
                     ext->Pdo,
                     DevicePropertyHardwareID,
                     sizeof(hwidBuf),
                     hwidBuf,
                     &len);

        if (NT_SUCCESS(status)) {
            p = hwidBuf;

            while (*p != UNICODE_NULL) {
                RtlInitUnicodeString(&oneId, p);

                for (i = 0; i < g_GpioControllersCount; i++) {
                    if (wcsstr(p, g_GpioControllers[i].PciId) != NULL) {
                        ctrlProfile = &g_GpioControllers[i];
                        break;
                    }
                }

                if (ctrlProfile != NULL)
                    break;

                p += wcslen(p) + 1;
            }
        } else {
            GpioCtrl_Log("StartDevice: IoGetDeviceProperty(HardwareID) failed, status=0x%08X\n", status);
        }
    }

    if (ctrlProfile != NULL) {
        ext->ControllerProfile = ctrlProfile;
        quirks    = ctrlProfile->Quirks;
        bsodFlags = ctrlProfile->BsodQuirks;

        GpioCtrl_Log("StartDevice: Matched controller profile: %ws (quirks=0x%X, bsod=0x%X)\n",
                     ctrlProfile->PciId,
                     ctrlProfile->Quirks,
                     ctrlProfile->BsodQuirks);
    } else {
        ext->ControllerProfile = NULL;
        GpioCtrl_Log("StartDevice: No specific controller profile matched - using defaults\n");
    }

    /* -----------------------------------------------------------------------
       2) BSOD_DELAY_INIT
       ----------------------------------------------------------------------- */
    if (bsodFlags & BSOD_DELAY_INIT) {
        GpioCtrl_Log("StartDevice: BSOD_DELAY_INIT - delaying initial hardware access\n");
        KeStallExecutionProcessor(20000);
    }

    /* -----------------------------------------------------------------------
       3) Parse translated resources
       ----------------------------------------------------------------------- */
    resTranslated = isl->Parameters.StartDevice.AllocatedResourcesTranslated;

    if (resTranslated != NULL) {
        GpioCtrl_Log("StartDevice: Parsing %u resource lists\n", resTranslated->Count);

        for (i = 0; i < resTranslated->Count; i++) {
            PCM_FULL_RESOURCE_DESCRIPTOR full = &resTranslated->List[i];
            PCM_PARTIAL_RESOURCE_LIST partial = &full->PartialResourceList;

            for (j = 0; j < partial->Count; j++) {
                PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = &partial->PartialDescriptors[j];

                switch (desc->Type) {

                case CmResourceTypeMemory:

                    if (desc->u.Memory.Length == 0x1000) {

                        if ((desc->u.Memory.Start.LowPart & 0xFFF00000) == 0xFE000000) {
                            ext->PmcBasePa = desc->u.Memory.Start;
                            ext->PmcLength = desc->u.Memory.Length;
                            hasPmcWindow   = TRUE;

                            GpioCtrl_Log("StartDevice: PMC MMIO: PA=%08X Len=%u\n",
                                         ext->PmcBasePa.LowPart,
                                         ext->PmcLength);
                        } else {
                            ext->MmioBasePa = desc->u.Memory.Start;
                            ext->MmioLength = desc->u.Memory.Length;

                            GpioCtrl_Log("StartDevice: GPIO MMIO: PA=%08X Len=%u\n",
                                         ext->MmioBasePa.LowPart,
                                         ext->MmioLength);
                        }
                    }
                    break;

                case CmResourceTypeInterrupt:

                    ext->Vector        = desc->u.Interrupt.Vector;
                    ext->Irql          = (KIRQL)desc->u.Interrupt.Level;
                    ext->SynchIrql     = (KIRQL)desc->u.Interrupt.Level;
                    ext->Affinity      = desc->u.Interrupt.Affinity;
                    ext->InterruptMode = (desc->Flags & CM_RESOURCE_INTERRUPT_LATCHED)
                                         ? Latched : LevelSensitive;

                    GpioCtrl_Log("StartDevice: IRQ: Vector=%u IRQL=%u Mode=%s\n",
                                 ext->Vector,
                                 ext->Irql,
                                 (ext->InterruptMode == Latched ? "Latched" : "Level"));
                    break;

                default:
                    break;
                }
            }
        }
    }

    /* -----------------------------------------------------------------------
       4) Map GPIO MMIO
       ----------------------------------------------------------------------- */
    if (ext->MmioLength != 0) {
        ext->MmioBase = (PUCHAR)MmMapIoSpace(ext->MmioBasePa,
                                             ext->MmioLength,
                                             MmNonCached);

        if (ext->MmioBase == NULL) {
            GpioCtrl_Log("StartDevice: MmMapIoSpace FAILED for GPIO\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        GpioCtrl_Log("StartDevice: GPIO MMIO mapped at %p\n", ext->MmioBase);
    }

    /* -----------------------------------------------------------------------
       5) Map PMC MMIO (LPSS)
       ----------------------------------------------------------------------- */
    if (ext->PmcLength != 0) {
        ext->PmcBase = (PUCHAR)MmMapIoSpace(ext->PmcBasePa,
                                            ext->PmcLength,
                                            MmNonCached);

        if (ext->PmcBase == NULL) {
            GpioCtrl_Log("StartDevice: MmMapIoSpace FAILED for PMC\n");
        } else {
            GpioCtrl_Log("StartDevice: PMC MMIO mapped at %p\n", ext->PmcBase);
        }
    }

    /* Determine controller type */
    if (!hasPmcWindow && ext->MmioBase != NULL) {
        ext->ControllerType = GpioctrlControllerCannonLake;
        GpioCtrl_Log("StartDevice: No PMC window detected - assuming Cannon Lake-style GPIO\n");
    } else if (hasPmcWindow) {
        ext->ControllerType = GpioctrlControllerLpss;
    } else {
        ext->ControllerType = GpioctrlControllerUnknown;
    }

    /* -----------------------------------------------------------------------
       6) BSOD_MASK_INTERRUPTS before programming registers
       ----------------------------------------------------------------------- */
    if ((bsodFlags & BSOD_MASK_INTERRUPTS) && ext->MmioBase != NULL) {
        KIRQL oldIrql;
        GpioCtrl_Log("StartDevice: BSOD_MASK_INTERRUPTS - pre-masking GPIO interrupts\n");
        KeAcquireSpinLock(&ext->RegLock, &oldIrql);
        GpioRegWrite(ext, REG_INT_EN_OFFSET, 0);
        KeReleaseSpinLock(&ext->RegLock, oldIrql);
    }

    /* -----------------------------------------------------------------------
       7) LPSS power domain enable (if PMC exists)
       ----------------------------------------------------------------------- */
    if (ext->PmcBase != NULL) {
        ULONG val, timeout;

        GpioCtrl_Log("StartDevice: Enabling LPSS power domain\n");

        val = *(volatile ULONG*)(ext->PmcBase + 0x44);
        val &= ~0x1;
        *(volatile ULONG*)(ext->PmcBase + 0x44) = val;

        timeout = 1000;
        while (timeout--) {
            val = *(volatile ULONG*)(ext->PmcBase + 0x48);
            if ((val & 0x1) == 0) {
                GpioCtrl_Log("StartDevice: LPSS power domain ON\n");
                break;
            }
            KeStallExecutionProcessor(10);
        }

        if (timeout == 0)
            GpioCtrl_Log("StartDevice: ERROR - LPSS power domain did not turn on\n");

        *(volatile ULONG*)(ext->PmcBase + 0x4C) = 0x1;
    }

    /* -----------------------------------------------------------------------
       8) LPSS clock + reset sequence
       ----------------------------------------------------------------------- */
    if (ext->MmioBase != NULL && hasPmcWindow) {
        ULONG val, timeout;

        GpioCtrl_Log("StartDevice: Enabling LPSS clock + reset\n");

        /* Clock enable */
        val = *(volatile ULONG*)(ext->MmioBase + 0x800);
        val |= 0x1;
        *(volatile ULONG*)(ext->MmioBase + 0x800) = val;

        if (!(quirks & QUIRK_BROKEN_CLOCK_GATE)) {
            timeout = 1000;
            while (timeout--) {
                val = *(volatile ULONG*)(ext->MmioBase + 0x800);
                if (val & 0x2) {
                    GpioCtrl_Log("StartDevice: LPSS clock ON\n");
                    break;
                }
                KeStallExecutionProcessor(10);
            }

            if (timeout == 0)
                GpioCtrl_Log("StartDevice: WARNING - LPSS clock status did not assert\n");
        } else {
            GpioCtrl_Log("StartDevice: QUIRK_BROKEN_CLOCK_GATE - skipping clock status polling\n");
        }

        /* Reset deassert */
        val = *(volatile ULONG*)(ext->MmioBase + 0x804);
        val |= 0x1;
        *(volatile ULONG*)(ext->MmioBase + 0x804) = val;

        timeout = 1000;
        while (timeout--) {
            val = *(volatile ULONG*)(ext->MmioBase + 0x804);
            if (val & 0x2) {
                GpioCtrl_Log("StartDevice: LPSS reset deasserted\n");
                break;
            }
            KeStallExecutionProcessor(10);
        }

        if (timeout == 0)
            GpioCtrl_Log("StartDevice: WARNING - LPSS reset status did not assert\n");

        if (quirks & QUIRK_NEEDS_RESET_WORKAROUND) {
            GpioCtrl_Log("StartDevice: QUIRK_NEEDS_RESET_WORKAROUND - issuing extra reset pulse\n");

            val = *(volatile ULONG*)(ext->MmioBase + 0x804);
            val &= ~0x1;
            *(volatile ULONG*)(ext->MmioBase + 0x804) = val;
            KeStallExecutionProcessor(10);

            val |= 0x1;
            *(volatile ULONG*)(ext->MmioBase + 0x804) = val;
        }

        GpioCtrl_Log("StartDevice: LPSS clock/reset sequence completed\n");
    }

    /* -----------------------------------------------------------------------
       9) Registry policy
       ----------------------------------------------------------------------- */
    GpioCtrl_Log("StartDevice: Loading registry policy\n");
    GpioCtrl_LoadRegistryPolicy(ext, NULL);

    /* -----------------------------------------------------------------------
       10) Initialize hardware defaults
       ----------------------------------------------------------------------- */
    if (ext->MmioBase != NULL) {
        KIRQL oldIrql;
        ULONG mask;

        GpioCtrl_Log("StartDevice: Initializing hardware defaults\n");

        KeAcquireSpinLock(&ext->RegLock, &oldIrql);

        GpioRegWrite(ext, REG_INT_EN_OFFSET, 0);
        mask = GpioRegRead(ext, REG_INT_STAT_OFFSET);

        if (mask != 0) {
            GpioRegWrite(ext, REG_INT_STAT_OFFSET, mask);
            GpioCtrl_Log("StartDevice: Cleared pending interrupts (mask=%08X)\n", mask);
        }

        KeReleaseSpinLock(&ext->RegLock, oldIrql);
    }

/* -----------------------------------------------------------------------
   10.5) Enable GPIO interrupts for all pins (or a specific pin)
   ----------------------------------------------------------------------- */
if (ext->MmioBase != NULL) {
    KIRQL oldIrql;

    GpioCtrl_Log("StartDevice: Enabling GPIO interrupts\n");

    KeAcquireSpinLock(&ext->RegLock, &oldIrql);

    /* Example: enable all pins */
    GpioRegWrite(ext, REG_INT_EN_OFFSET, 0xFFFFFFFF);

    /* Clear any pending interrupts */
    GpioRegWrite(ext, REG_INT_STAT_OFFSET, 0xFFFFFFFF);

    KeReleaseSpinLock(&ext->RegLock, oldIrql);
}


    /* -----------------------------------------------------------------------
       11) Connect interrupt
       ----------------------------------------------------------------------- */
    if (ext->Vector != 0) {
        GpioCtrl_Log("StartDevice: Connecting interrupt\n");

        status = IoConnectInterrupt(
                     &ext->InterruptObject,
                     GpioCtrl_Isr,
                     ext,
                     &ext->RegLock,
                     ext->Vector,
                     ext->Irql,
                     ext->SynchIrql,
                     ext->InterruptMode,
                     TRUE,
                     ext->Affinity,
                     FALSE);

        if (!NT_SUCCESS(status)) {
            GpioCtrl_Log("StartDevice: IoConnectInterrupt FAILED (0x%08X)\n", status);
            ext->InterruptObject    = NULL;
            ext->InterruptConnected = FALSE;
        } else {
            GpioCtrl_Log("StartDevice: Interrupt connected successfully\n");
            ext->InterruptConnected = TRUE;
        }
    }

    ext->Started = TRUE;
    GpioCtrl_Log("StartDevice: Completed successfully\n");

    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
   StopDevice: disconnect interrupt, cancel DPC, power down LPSS, unmap MMIO/PMC
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_StopDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp
    )
{
    PGPIOCTRL_FDO ext;
    const GPIOCTRL_DEVICE_ID* id;
    ULONG quirks;
    ULONG bsod;

    UNREFERENCED_PARAMETER(Irp);

    ext    = (PGPIOCTRL_FDO)DeviceObject->DeviceExtension;
    id     = ext->ControllerProfile;   /* Set in StartDevice */
    quirks = ext->QuirkFlags;
    bsod   = ext->BsodPolicy;

    GpioCtrl_Log("StopDevice: Entered for device %p\n", DeviceObject);

    /* -----------------------------------------------------------------------
       0) Cancel ISR DPC (this prevents BugCheck 0xCE)
       ----------------------------------------------------------------------- */
    GpioCtrl_Log("StopDevice: Removing queued ISR DPC (if any)\n");
    KeRemoveQueueDpc(&ext->IsrDpc);

    /* -----------------------------------------------------------------------
       1) BSOD_MASK_INTERRUPTS - mask IRQs before shutdown
       ----------------------------------------------------------------------- */
    if ((bsod & BSOD_MASK_INTERRUPTS) && ext->MmioBase != NULL) {

        KIRQL oldIrql;

        GpioCtrl_Log("StopDevice: BSOD_MASK_INTERRUPTS - masking interrupts\n");

        KeAcquireSpinLock(&ext->RegLock, &oldIrql);
        GpioRegWrite(ext, REG_INT_EN_OFFSET, 0);
        KeReleaseSpinLock(&ext->RegLock, oldIrql);
    }

    /* -----------------------------------------------------------------------
       2) Disconnect interrupt
       ----------------------------------------------------------------------- */
    if (ext->InterruptConnected && ext->InterruptObject != NULL) {

        GpioCtrl_Log("StopDevice: Disconnecting interrupt (Vector=%u)\n", ext->Vector);

        IoDisconnectInterrupt(ext->InterruptObject);

        ext->InterruptObject    = NULL;
        ext->InterruptConnected = FALSE;

        GpioCtrl_Log("StopDevice: Interrupt disconnected\n");
    }

    /* -----------------------------------------------------------------------
       3) LPSS shutdown sequence (only if PMC exists AND controller has LPSS offsets)
       ----------------------------------------------------------------------- */
    if (ext->PmcBase != NULL &&
        id != NULL &&
        (id->LpssClkGateOffset ||
         id->LpssResetOffset   ||
         id->LpssFuncClkOffset ||
         id->LpssMiscOffset))
    {
        ULONG val;

        GpioCtrl_Log("StopDevice: LPSS shutdown sequence\n");

        /* Power-gate LPSS domain (PMC + 0x44) */
        val = *(volatile ULONG*)(ext->PmcBase + 0x44);
        val |= 0x1;
        *(volatile ULONG*)(ext->PmcBase + 0x44) = val;

        /* QUIRK_NEEDS_RESET_WORKAROUND */
        if ((quirks & QUIRK_NEEDS_RESET_WORKAROUND) &&
            ext->MmioBase != NULL &&
            id->LpssResetOffset != 0)
        {
            ULONG r;

            GpioCtrl_Log("StopDevice: QUIRK_NEEDS_RESET_WORKAROUND - issuing reset pulse\n");

            r = READ_REGISTER_ULONG((PULONG)(ext->MmioBase + id->LpssResetOffset));
            r &= ~0x1;
            WRITE_REGISTER_ULONG((PULONG)(ext->MmioBase + id->LpssResetOffset), r);

            KeStallExecutionProcessor(10);

            r |= 0x1;
            WRITE_REGISTER_ULONG((PULONG)(ext->MmioBase + id->LpssResetOffset), r);
        }

        GpioCtrl_Log("StopDevice: LPSS power domain gated\n");
    }

    /* -----------------------------------------------------------------------
       4) Unmap GPIO MMIO (BAR0)
       ----------------------------------------------------------------------- */
    if (ext->MmioBase != NULL) {

        GpioCtrl_Log("StopDevice: Unmapping GPIO MMIO at %p (Len=%u)\n",
                     ext->MmioBase, ext->MmioLength);

        MmUnmapIoSpace(ext->MmioBase, ext->MmioLength);

        ext->MmioBase   = NULL;
        ext->MmioLength = 0;

        GpioCtrl_Log("StopDevice: GPIO MMIO unmapped\n");
    }

    /* -----------------------------------------------------------------------
       5) Unmap PMC MMIO (LPSS only)
       ----------------------------------------------------------------------- */
    if (ext->PmcBase != NULL) {

        GpioCtrl_Log("StopDevice: Unmapping PMC MMIO at %p (Len=%u)\n",
                     ext->PmcBase, ext->PmcLength);

        MmUnmapIoSpace(ext->PmcBase, ext->PmcLength);

        ext->PmcBase   = NULL;
        ext->PmcLength = 0;

        GpioCtrl_Log("StopDevice: PMC MMIO unmapped\n");
    }

    ext->Started = FALSE;

    GpioCtrl_Log("StopDevice: Completed successfully\n");

    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
   RemoveDevice: full teardown after IRP_MN_REMOVE_DEVICE
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_RemoveDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PGPIOCTRL_FDO ext;
    ULONG bsodFlags;

    UNREFERENCED_PARAMETER(Irp);

    ext = (PGPIOCTRL_FDO)DeviceObject->DeviceExtension;
    bsodFlags = (ext->ControllerProfile ? ext->ControllerProfile->BsodQuirks : 0);

    GpioCtrl_Log("RemoveDevice: Entered for device %p\n", DeviceObject);

    ext->Removed = TRUE;

    /* -----------------------------------------------------------------------
       1) BSOD_MASK_INTERRUPTS — mask IRQs before teardown
       ----------------------------------------------------------------------- */
    if ((bsodFlags & BSOD_MASK_INTERRUPTS) && ext->MmioBase != NULL) {

        KIRQL oldIrql;

        GpioCtrl_Log("RemoveDevice: BSOD_MASK_INTERRUPTS - masking interrupts before removal\n");

        KeAcquireSpinLock(&ext->RegLock, &oldIrql);
        GpioRegWrite(ext, REG_INT_EN_OFFSET, 0);
        KeReleaseSpinLock(&ext->RegLock, oldIrql);
    }

    /* -----------------------------------------------------------------------
       2) If still started, stop it cleanly
       ----------------------------------------------------------------------- */
    if (ext->Started) {
        GpioCtrl_Log("RemoveDevice: Device still started, calling StopDevice\n");
        (VOID)GpioCtrl_StopDevice(DeviceObject, Irp);
    }

    /* -----------------------------------------------------------------------
       3) Flush ISR/DPC log buffer
       ----------------------------------------------------------------------- */
    GpioCtrl_FlushIsrLog(ext);

    /* -----------------------------------------------------------------------
       4) Detach from lower device
       ----------------------------------------------------------------------- */
    if (ext->LowerDevice != NULL) {
        GpioCtrl_Log("RemoveDevice: Detaching from lower device %p\n", ext->LowerDevice);
        IoDetachDevice(ext->LowerDevice);
        ext->LowerDevice = NULL;
    }

    /* -----------------------------------------------------------------------
       5) Delete device object
       ----------------------------------------------------------------------- */
    GpioCtrl_Log("RemoveDevice: Deleting device object %p\n", DeviceObject);
    IoDeleteDevice(DeviceObject);

    GpioCtrl_Log("RemoveDevice: Completed successfully\n");
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   MMIO helpers
   --------------------------------------------------------------------------- */
ULONG
GpioRegRead(
    IN PGPIOCTRL_FDO Ext,
    IN ULONG Offset
    )
{
    volatile ULONG* reg;
    ULONG value;

    reg = (volatile ULONG*)(Ext->MmioBase + Offset);
    value = *reg;
    return value;
}

VOID
GpioRegWrite(
    IN PGPIOCTRL_FDO Ext,
    IN ULONG Offset,
    IN ULONG Value
    )
{
    volatile ULONG* reg;
    reg = (volatile ULONG*)(Ext->MmioBase + Offset);
    *reg = Value;
}

/* ---------------------------------------------------------------------------
   Enable LPSS power domain (Intel Serial IO)
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_EnableLpssPower(
    PGPIOCTRL_FDO Ext
    )
{
    ULONG val;
    ULONG timeout;

    if (Ext->PmcBase == NULL) {
        GpioCtrl_Log("LPSS: PMC base not mapped\n");
        return STATUS_NOT_SUPPORTED;
    }

    GpioCtrl_Log("LPSS: Enabling LPSS power domain\n");

    /* Step 1: Clear power-gate bit */
    val = *(volatile ULONG*)(Ext->PmcBase + 0x44);
    val &= ~0x1;   /* Clear PG bit */
    *(volatile ULONG*)(Ext->PmcBase + 0x44) = val;

    /* Step 2: Wait for PG_STATUS to clear */
    timeout = 1000;
    while (timeout--) {
        val = *(volatile ULONG*)(Ext->PmcBase + 0x48);
        if ((val & 0x1) == 0) {
            GpioCtrl_Log("LPSS: Power domain ON\n");
            break;
        }
        KeStallExecutionProcessor(10);
    }

    if (timeout == 0) {
        GpioCtrl_Log("LPSS: ERROR - power domain did not turn on\n");
        return STATUS_IO_TIMEOUT;
    }

    /* Step 3: Clear ACK bit */
    *(volatile ULONG*)(Ext->PmcBase + 0x4C) = 0x1;

    GpioCtrl_Log("LPSS: Power domain enabled successfully\n");
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   Enable LPSS I2C clock and deassert reset (template)
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_EnableLpssClockAndReset(
    PGPIOCTRL_FDO Ext
    )
{
    ULONG val;
    ULONG timeout;

    if (Ext->MmioBase == NULL) {
        GpioCtrl_Log("LPSS: MmioBase is NULL, cannot enable clock\n");
        return STATUS_DEVICE_NOT_READY;
    }

    GpioCtrl_Log("LPSS: Enabling LPSS I2C clock and deasserting reset\n");

    /* 
       NOTE: Offsets are platform-specific.
       These are typical LPSS-style templates:

       0x800: CLKCTL   (bit 0 = clock enable)
       0x804: RSTCTL   (bit 0 = reset deassert)
    */

    /* 1) Enable clock */
    val = *(volatile ULONG*)(Ext->MmioBase + 0x800);
    val |= 0x1;   /* set CLK_EN */
    *(volatile ULONG*)(Ext->MmioBase + 0x800) = val;

    /* Optional: wait for clock status bit */
    timeout = 1000;
    while (timeout--) {
        val = *(volatile ULONG*)(Ext->MmioBase + 0x800);
        if (val & 0x2) {   /* example: bit1 = CLK_ON_STATUS */
            GpioCtrl_Log("LPSS: Clock is ON\n");
            break;
        }
        KeStallExecutionProcessor(10);
    }

    if (timeout == 0) {
        GpioCtrl_Log("LPSS: WARNING - clock status did not assert\n");
    }

    /* 2) Deassert reset */
    val = *(volatile ULONG*)(Ext->MmioBase + 0x804);
    val |= 0x1;   /* set RESET_DEASSERT */
    *(volatile ULONG*)(Ext->MmioBase + 0x804) = val;

    /* Optional: wait for reset done */
    timeout = 1000;
    while (timeout--) {
        val = *(volatile ULONG*)(Ext->MmioBase + 0x804);
        if (val & 0x2) {   /* example: bit1 = RESET_DONE */
            GpioCtrl_Log("LPSS: Reset deasserted\n");
            break;
        }
        KeStallExecutionProcessor(10);
    }

    if (timeout == 0) {
        GpioCtrl_Log("LPSS: WARNING - reset status did not assert\n");
    }

    GpioCtrl_Log("LPSS: Clock/Reset sequence completed\n");
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   Flush ISR circular buffer to gpioctrl.log (PASSIVE_LEVEL only)
   --------------------------------------------------------------------------- */
VOID
GpioCtrl_FlushIsrLog(
    PGPIOCTRL_FDO Ext
    )
{
    KIRQL oldIrql;
    ULONG i;

    /* Only safe at PASSIVE_LEVEL */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;

    KeAcquireSpinLock(&Ext->IsrLogLock, &oldIrql);

    for (i = Ext->IsrLogTail; i < Ext->IsrLogHead; i++) {
        ULONG idx = i % GPIOCTRL_LOG_SIZE;
        GpioCtrl_Log("%s\n", Ext->IsrLog[idx]);
    }

    Ext->IsrLogTail = Ext->IsrLogHead;

    KeReleaseSpinLock(&Ext->IsrLogLock, oldIrql);
}

// ----------------------------------------------------
// Place GpioCtrl_Log() HERE — at the bottom of the file
// ----------------------------------------------------

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, GpioCtrl_Log)
#endif

VOID
GpioCtrl_Log(
    IN PCCHAR Format,
    ...
    )
{
    CHAR                buffer[512];
    va_list             args;
    UNICODE_STRING      path;
    OBJECT_ATTRIBUTES   oa;
    IO_STATUS_BLOCK     iosb;
    HANDLE              hFile;
    NTSTATUS            status;
    SIZE_T              len;

    PAGED_CODE();

    if (Format == NULL) {
        return;
    }

    /* Format the string with ALL arguments */
    va_start(args, Format);
    _vsnprintf(buffer, sizeof(buffer) - 1, Format, args);
    va_end(args);

    buffer[sizeof(buffer) - 1] = '\0';

    len = strlen(buffer);
    if (len == 0) {
        return;
    }

    /* Prepare log file path */
    RtlInitUnicodeString(&path, L"\\SystemRoot\\System32\\gpioctrl.log");

    InitializeObjectAttributes(
        &oa,
        &path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL
    );

    /* Open or create the log file */
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

    /* Write the formatted text */
    ZwWriteFile(
        hFile,
        NULL,
        NULL,
        NULL,
        &iosb,
        (PVOID)buffer,
        (ULONG)len,
        NULL,
        NULL
    );

    ZwClose(hFile);
}

NTSTATUS
GpioCtrl_IoctlWaitForInterrupt(
    IN PGPIOCTRL_FDO Ext,
    IN PIRP Irp
    )
{
    PIO_STACK_LOCATION isl;
    PGPIOCTRL_WAIT_REQUEST in;
    PGPIOCTRL_WAIT_RESPONSE out;
    ULONG inLen, outLen;
    NTSTATUS status;
    KIRQL oldIrql;
    ULONG pin;

    isl    = IoGetCurrentIrpStackLocation(Irp);
    inLen  = isl->Parameters.DeviceIoControl.InputBufferLength;
    outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;

    if (inLen < sizeof(GPIOCTRL_WAIT_REQUEST) ||
        outLen < sizeof(GPIOCTRL_WAIT_RESPONSE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    in  = (PGPIOCTRL_WAIT_REQUEST)Irp->AssociatedIrp.SystemBuffer;
    out = (PGPIOCTRL_WAIT_RESPONSE)Irp->AssociatedIrp.SystemBuffer;

    /* For now we ignore in->Pin (future: per‑pin filtering) */
    UNREFERENCED_PARAMETER(in);

    /* Reset event before waiting */
    KeClearEvent(&Ext->WaitEvent);

    /* Block until ISR/DPC signals */
    status = KeWaitForSingleObject(
                 &Ext->WaitEvent,
                 Executive,
                 KernelMode,
                 FALSE,
                 NULL);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Retrieve the pin that fired */
    KeAcquireSpinLock(&Ext->WaitLock, &oldIrql);
    pin = Ext->LastFiredPin;
    Ext->LastFiredPin = (ULONG)-1;
    KeReleaseSpinLock(&Ext->WaitLock, oldIrql);

    out->Pin   = pin;
    out->Fired = 1;

    Irp->IoStatus.Information = sizeof(GPIOCTRL_WAIT_RESPONSE);
    return STATUS_SUCCESS;
}
