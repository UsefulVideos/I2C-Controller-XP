/* gpioctrl.c
 * GPIO Controller Driver (gpioctrl.sys)
 * WinDDK 7.1.0 – XP/2003 build environment – C89 compliant
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
    PGPIOCTRL_FDO_EXT ext;
    PDEVICE_OBJECT lowerDevice;

    status = IoCreateDevice(
        DriverObject,
        sizeof(GPIOCTRL_FDO_EXT),
        NULL,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &fdo);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Attach to the PDO’s stack – REQUIRED for PnP behavior.
    //
    lowerDevice = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (lowerDevice == NULL) {
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }

    Gpioctrl_GlobalDeviceObject = fdo;

    ext = (PGPIOCTRL_FDO_EXT)fdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));

    ext->Self        = fdo;
    ext->LowerDevice = lowerDevice;
    ext->Signature   = GPIOCTRL_FDO_SIGNATURE;

    ext->SupportsPull        = 1;
    ext->SupportsInterrupts  = 1;
    ext->SupportsDebounce    = 1;
    ext->PinCount            = 32;
    ext->DebounceDefaultMs   = 10;
    ext->CrashOnError        = 0;

    KeInitializeSpinLock(&ext->RegLock);
    KeInitializeSpinLock(&ext->IsrLogLock);
    KeInitializeDpc(&ext->IsrDpc, GpioCtrl_Dpc, ext);
    ext->PendingIntMask = 0;
    ext->IsrLogHead     = 0;
    ext->IsrLogTail     = 0;

    fdo->Flags |= DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   PnP dispatch (standalone FDO – handle and complete)
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_DispatchPnp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PGPIOCTRL_FDO_EXT ext;
    PIO_STACK_LOCATION isl;
    NTSTATUS status;

    ext = (PGPIOCTRL_FDO_EXT)DeviceObject->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    switch (isl->MinorFunction) {

    case IRP_MN_START_DEVICE:
        //
        // StartDevice MUST complete the IRP itself.
        //
        return g_GpioCtrlGlobal.StartDevice(DeviceObject, Irp);

    case IRP_MN_STOP_DEVICE:
        //
        // StopDevice MUST complete the IRP itself.
        //
        return g_GpioCtrlGlobal.StopDevice(DeviceObject, Irp);

    case IRP_MN_REMOVE_DEVICE:
        //
        // RemoveDevice MUST complete the IRP itself.
        //
        return GpioCtrl_RemoveDevice(DeviceObject, Irp);

    default:
        //
        // For all other PnP IRPs, succeed and complete here.
        //
        status = STATUS_SUCCESS;
        break;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}


/* ---------------------------------------------------------------------------
   Power dispatch (standalone FDO – no lower device)
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_DispatchPower(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PGPIOCTRL_FDO_EXT ext;
    PIO_STACK_LOCATION isl;
    NTSTATUS status = STATUS_SUCCESS;

    ext = (PGPIOCTRL_FDO_EXT)DeviceObject->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    UNREFERENCED_PARAMETER(isl);

    PoStartNextPowerIrp(Irp);

    if (ext->LowerDevice != NULL) {
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(ext->LowerDevice, Irp);
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
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
    PGPIOCTRL_FDO_EXT ext;
    ULONG code;

    isl  = IoGetCurrentIrpStackLocation(Irp);
    ext  = (PGPIOCTRL_FDO_EXT)DeviceObject->DeviceExtension;
    code = isl->Parameters.DeviceIoControl.IoControlCode;

    /* Default values */
    status = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;

    /* Device not started yet */
    if (!ext->Started) {
        status = STATUS_DEVICE_NOT_READY;
    } else {
        switch (code) {
        case IOCTL_GPIO_READ_PIN:
            status = GpioCtrl_IoctlReadPin(ext, Irp);
            break;

        case IOCTL_GPIO_WRITE_PIN:
            status = GpioCtrl_IoctlWritePin(ext, Irp);
            break;

        case IOCTL_GPIO_CONFIGURE_PIN:
            status = GpioCtrl_IoctlConfigurePin(ext, Irp);
            break;

        case IOCTL_GPIO_QUERY_CAPS:
            status = GpioCtrl_IoctlQueryCaps(ext, Irp);
            break;

        case IOCTL_GPIO_FORCE_CRASH:
            /* This call never returns; no code after it executes */
            KeBugCheckEx(MANUALLY_INITIATED_CRASH,
                         0x474F5043UL, /* "GPOC" */
                         ext->Signature,
                         0,
                         0);
            /* no break, no return needed */
            break;

        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }
    }

    /* Complete IRP once, unless bugcheck occurred */
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS
GpioCtrl_StartDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    NTSTATUS status;
    PGPIOCTRL_FDO_EXT ext;
    PCM_RESOURCE_LIST resTranslated;
    PCM_RESOURCE_LIST resRaw;
    PIO_STACK_LOCATION isl;
    ULONG i, j;
    BOOLEAN hasPmcWindow;
    const GPIOCTRL_DEVICE_ID* ctrlProfile;
    WCHAR hwidBuf[256];
    UNICODE_STRING hwid;
    ULONG quirks;
    ULONG bsodFlags;

    ext = (PGPIOCTRL_FDO_EXT)DeviceObject->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    hasPmcWindow = FALSE;
    ctrlProfile  = NULL;
    quirks       = QUIRK_NONE;
    bsodFlags    = BSOD_NONE;

    RtlZeroMemory(hwidBuf, sizeof(hwidBuf));

    GpioCtrl_Log("START: Entered for device %p\n", DeviceObject);

    //
    // Forward START_DEVICE to lower driver
    //
    IoCopyCurrentIrpStackLocationToNext(Irp);
    status = IoCallDriver(ext->LowerDevice, Irp);

    if (!NT_SUCCESS(status)) {
        GpioCtrl_Log("START: Lower driver failed, status=0x%08X\n", status);
        return status;
    }

    GpioCtrl_Log("START: Lower driver completed successfully\n");

    //
    // Query Hardware ID and match against g_GpioControllers[]
    //
    RtlInitEmptyUnicodeString(&hwid, hwidBuf, sizeof(hwidBuf));

    status = IoGetDeviceProperty(
                 ext->Pdo,                      /* physical device object */
                 DevicePropertyHardwareID,
                 sizeof(hwidBuf),
                 hwidBuf,
                 &i);                           /* i reused as length */

    if (NT_SUCCESS(status)) {
        const WCHAR* p;

        /* hwidBuf is MULTI_SZ */
        p = hwidBuf;

        while (*p != UNICODE_NULL) {
            UNICODE_STRING oneId;

            RtlInitUnicodeString(&oneId, p);

            for (i = 0; i < g_GpioControllersCount; i++) {
                /* simple substring match: does this HWID contain our pattern? */
                if (wcsstr(p, g_GpioControllers[i].PciId) != NULL) {
                    ctrlProfile = &g_GpioControllers[i];
                    break;
                }
            }

            if (ctrlProfile != NULL) {
                break;
            }

            /* advance to next string in MULTI_SZ */
            p += wcslen(p) + 1;
        }
    } else {
        GpioCtrl_Log("START: IoGetDeviceProperty(HardwareID) failed, status=0x%08X\n", status);
    }

    if (ctrlProfile != NULL) {
        ext->ControllerProfile = ctrlProfile;
        quirks    = ctrlProfile->Quirks;
        bsodFlags = ctrlProfile->BsodQuirks;

        GpioCtrl_Log("START: Matched controller profile: %ws (quirks=0x%X, bsod=0x%X)\n",
                     ctrlProfile->PciId,
                     ctrlProfile->Quirks,
                     ctrlProfile->BsodQuirks);
    } else {
        ext->ControllerProfile = NULL;
        GpioCtrl_Log("START: No specific controller profile matched – using defaults\n");
    }

    //
    // BSOD_DELAY_INIT: conservative delay before touching hardware
    //
    if (bsodFlags & BSOD_DELAY_INIT) {
        GpioCtrl_Log("START: BSOD_DELAY_INIT – delaying initial hardware access\n");
        KeStallExecutionProcessor(20000); /* ~20 ms */
    }

    resTranslated = isl->Parameters.StartDevice.AllocatedResourcesTranslated;
    resRaw        = isl->Parameters.StartDevice.AllocatedResources;
    UNREFERENCED_PARAMETER(resRaw);

    /* Parse translated resources */
    if (resTranslated != NULL) {
        GpioCtrl_Log("START: Parsing %u resource lists\n", resTranslated->Count);

        for (i = 0; i < resTranslated->Count; i++) {
            PCM_FULL_RESOURCE_DESCRIPTOR full;
            PCM_PARTIAL_RESOURCE_LIST partial;
            PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;

            full    = &resTranslated->List[i];
            partial = &full->PartialResourceList;

            for (j = 0; j < partial->Count; j++) {
                desc = &partial->PartialDescriptors[j];

                switch (desc->Type) {

                case CmResourceTypeMemory:

                    /* GPIO MMIO (4 KB window) */
                    if (desc->u.Memory.Length == 0x1000) {
                        /* PMC MMIO (LPSS power controller) lives in 0xFE000000 range */
                        if ((desc->u.Memory.Start.LowPart & 0xFFF00000) == 0xFE000000) {
                            ext->PmcBasePa = desc->u.Memory.Start;
                            ext->PmcLength = desc->u.Memory.Length;
                            hasPmcWindow   = TRUE;
                            GpioCtrl_Log("START: PMC MMIO: PA=%08X Len=%u\n",
                                         ext->PmcBasePa.LowPart,
                                         ext->PmcLength);
                        } else {
                            ext->MmioBasePa = desc->u.Memory.Start;
                            ext->MmioLength = desc->u.Memory.Length;
                            GpioCtrl_Log("START: GPIO MMIO: PA=%08X Len=%u\n",
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

                    GpioCtrl_Log("START: IRQ: Vector=%u IRQL=%u Mode=%s\n",
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

    /* Map GPIO MMIO */
    if (ext->MmioLength != 0) {
        ext->MmioBase = (PUCHAR)MmMapIoSpace(ext->MmioBasePa,
                                             ext->MmioLength,
                                             MmNonCached);

        if (ext->MmioBase == NULL) {
            GpioCtrl_Log("START: MmMapIoSpace FAILED for GPIO\n");
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        GpioCtrl_Log("START: GPIO MMIO mapped at %p\n", ext->MmioBase);
    }

    /* Map PMC MMIO (LPSS power domain controller) */
    if (ext->PmcLength != 0) {
        ext->PmcBase = (PUCHAR)MmMapIoSpace(ext->PmcBasePa,
                                            ext->PmcLength,
                                            MmNonCached);

        if (ext->PmcBase == NULL) {
            GpioCtrl_Log("START: MmMapIoSpace FAILED for PMC\n");
        } else {
            GpioCtrl_Log("START: PMC MMIO mapped at %p\n", ext->PmcBase);
        }
    }

    if (!hasPmcWindow && ext->MmioBase != NULL) {
        /* No PMC window: treat as Cannon Lake / modern PCH GPIO and skip LPSS power/clock */
        ext->ControllerType = GpioctrlControllerCannonLake;
        GpioCtrl_Log("START: No PMC window detected – assuming Cannon Lake‑style GPIO, skipping LPSS power/clock\n");
    } else if (hasPmcWindow) {
        ext->ControllerType = GpioctrlControllerLpss;
    } else {
        ext->ControllerType = GpioctrlControllerUnknown;
    }

    /* BSOD_MASK_INTERRUPTS: mask IRQs before any heavy register programming */
    if ((bsodFlags & BSOD_MASK_INTERRUPTS) && ext->MmioBase != NULL) {
        KIRQL oldIrql;
        GpioCtrl_Log("START: BSOD_MASK_INTERRUPTS – pre‑masking GPIO interrupts\n");
        KeAcquireSpinLock(&ext->RegLock, &oldIrql);
        GpioRegWrite(ext, REG_INT_EN_OFFSET, 0);
        KeReleaseSpinLock(&ext->RegLock, oldIrql);
    }

    /* Enable LPSS power domain if PMC is mapped (LPSS only) */
    if (ext->PmcBase != NULL) {
        ULONG val;
        ULONG timeout;

        GpioCtrl_Log("START: Enabling LPSS power domain\n");

        /* Clear PG bit */
        val = *(volatile ULONG*)(ext->PmcBase + 0x44);
        val &= ~0x1;
        *(volatile ULONG*)(ext->PmcBase + 0x44) = val;

        /* Wait for PG_STATUS to clear */
        timeout = 1000;
        while (timeout--) {
            val = *(volatile ULONG*)(ext->PmcBase + 0x48);
            if ((val & 0x1) == 0) {
                GpioCtrl_Log("START: LPSS power domain ON\n");
                break;
            }
            KeStallExecutionProcessor(10);
        }

        if (timeout == 0) {
            GpioCtrl_Log("START: ERROR – LPSS power domain did not turn on\n");
        }

        /* Clear ACK */
        *(volatile ULONG*)(ext->PmcBase + 0x4C) = 0x1;
    }

    /* LPSS CLOCK + RESET ENABLE SEQUENCE (only when PMC/LPSS is present) */
    if (ext->MmioBase != NULL && hasPmcWindow) {
        ULONG val;
        ULONG timeout;

        GpioCtrl_Log("START: Enabling LPSS clock + reset\n");

        /* 1) Enable clock (offset 0x800, bit0 = CLK_EN) */
        val = *(volatile ULONG*)(ext->MmioBase + 0x800);
        val |= 0x1;
        *(volatile ULONG*)(ext->MmioBase + 0x800) = val;

        /* QUIRK_BROKEN_CLOCK_GATE: skip clock‑gate polling if unreliable */
        if (!(quirks & QUIRK_BROKEN_CLOCK_GATE)) {
            /* Wait for clock status (bit1 = CLK_ON_STATUS) */
            timeout = 1000;
            while (timeout--) {
                val = *(volatile ULONG*)(ext->MmioBase + 0x800);
                if (val & 0x2) {
                    GpioCtrl_Log("START: LPSS clock ON\n");
                    break;
                }
                KeStallExecutionProcessor(10);
            }

            if (timeout == 0) {
                GpioCtrl_Log("START: WARNING – LPSS clock status did not assert\n");
            }
        } else {
            GpioCtrl_Log("START: QUIRK_BROKEN_CLOCK_GATE – skipping clock status polling\n");
        }

        /* 2) Deassert reset (offset 0x804, bit0 = RESET_DEASSERT) */
        val = *(volatile ULONG*)(ext->MmioBase + 0x804);
        val |= 0x1;
        *(volatile ULONG*)(ext->MmioBase + 0x804) = val;

        /* Wait for reset done (bit1 = RESET_DONE) */
        timeout = 1000;
        while (timeout--) {
            val = *(volatile ULONG*)(ext->MmioBase + 0x804);
            if (val & 0x2) {
                GpioCtrl_Log("START: LPSS reset deasserted\n");
                break;
            }
            KeStallExecutionProcessor(10);
        }

        if (timeout == 0) {
            GpioCtrl_Log("START: WARNING – LPSS reset status did not assert\n");
        }

        /* QUIRK_NEEDS_RESET_WORKAROUND: extra reset pulse */
        if (quirks & QUIRK_NEEDS_RESET_WORKAROUND) {
            GpioCtrl_Log("START: QUIRK_NEEDS_RESET_WORKAROUND – issuing extra reset pulse\n");

            /* Briefly clear RESET_DEASSERT then set again */
            val = *(volatile ULONG*)(ext->MmioBase + 0x804);
            val &= ~0x1;
            *(volatile ULONG*)(ext->MmioBase + 0x804) = val;
            KeStallExecutionProcessor(10);

            val |= 0x1;
            *(volatile ULONG*)(ext->MmioBase + 0x804) = val;
        }

        GpioCtrl_Log("START: LPSS clock/reset sequence completed\n");
    }

    /* QUIRK_NO_DMA_SUPPORT / BSOD_FORCE_PIO:
       GPIO driver itself does not use DMA, but log for diagnostics so that
       upper‑layer (e.g. I2C) drivers can honor PIO‑only policy. */
    if (quirks & QUIRK_NO_DMA_SUPPORT) {
        GpioCtrl_Log("START: QUIRK_NO_DMA_SUPPORT – controller is PIO‑only (no DMA)\n");
    }

    if (bsodFlags & BSOD_FORCE_PIO) {
        GpioCtrl_Log("START: BSOD_FORCE_PIO – system policy prefers PIO over DMA\n");
    }

    /* Load registry policy */
    GpioCtrl_Log("START: Loading registry policy\n");
    GpioCtrl_LoadRegistryPolicy(ext, NULL);

    /* Initialize hardware defaults */
    if (ext->MmioBase != NULL) {
        KIRQL oldIrql;
        ULONG mask;

        GpioCtrl_Log("START: Initializing hardware defaults\n");

        KeAcquireSpinLock(&ext->RegLock, &oldIrql);

        GpioRegWrite(ext, REG_INT_EN_OFFSET, 0);
        mask = GpioRegRead(ext, REG_INT_STAT_OFFSET);

        if (mask != 0) {
            GpioRegWrite(ext, REG_INT_STAT_OFFSET, mask);
            GpioCtrl_Log("START: Cleared pending interrupts (mask=%08X)\n", mask);
        }

        KeReleaseSpinLock(&ext->RegLock, oldIrql);
    }

    /* Connect interrupt */
    if (ext->Vector != 0) {
        GpioCtrl_Log("START: Connecting interrupt\n");

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
            GpioCtrl_Log("START: IoConnectInterrupt FAILED (0x%08X)\n", status);
            ext->InterruptObject    = NULL;
            ext->InterruptConnected = FALSE;
        } else {
            GpioCtrl_Log("START: Interrupt connected successfully\n");
            ext->InterruptConnected = TRUE;
        }
    }

    ext->Started = TRUE;
    GpioCtrl_Log("START: Completed successfully\n");

    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
   STOP device: disconnect interrupt, unmap MMIO/PMC safely (LPSS or CNL)
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_StopDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PGPIOCTRL_FDO_EXT ext;

    UNREFERENCED_PARAMETER(Irp);
    ext = (PGPIOCTRL_FDO_EXT)DeviceObject->DeviceExtension;

    GpioCtrl_Log("STOP: Entered for device %p\n", DeviceObject);

    /* Disconnect interrupt */
    if (ext->InterruptConnected && ext->InterruptObject != NULL) {
        GpioCtrl_Log("STOP: Disconnecting interrupt (Vector=%u)\n", ext->Vector);

        IoDisconnectInterrupt(ext->InterruptObject);

        ext->InterruptObject    = NULL;
        ext->InterruptConnected = FALSE;

        GpioCtrl_Log("STOP: Interrupt disconnected\n");
    }

    /* Unmap GPIO MMIO */
    if (ext->MmioBase != NULL) {
        GpioCtrl_Log("STOP: Unmapping GPIO MMIO at %p (Len=%u)\n",
                     ext->MmioBase,
                     ext->MmioLength);

        MmUnmapIoSpace(ext->MmioBase, ext->MmioLength);

        ext->MmioBase   = NULL;
        ext->MmioLength = 0;

        GpioCtrl_Log("STOP: GPIO MMIO unmapped\n");
    }

    /* Unmap PMC MMIO (LPSS only — Cannon Lake has no PMC window) */
    if (ext->PmcBase != NULL) {
        GpioCtrl_Log("STOP: Unmapping PMC MMIO at %p (Len=%u)\n",
                     ext->PmcBase,
                     ext->PmcLength);

        MmUnmapIoSpace(ext->PmcBase, ext->PmcLength);

        ext->PmcBase   = NULL;
        ext->PmcLength = 0;

        GpioCtrl_Log("STOP: PMC MMIO unmapped\n");
    }

    ext->Started = FALSE;
    GpioCtrl_Log("STOP: Completed successfully\n");

    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   REMOVE device: ensure stopped, delete (no detach)
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_RemoveDevice(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PGPIOCTRL_FDO_EXT ext;

    UNREFERENCED_PARAMETER(Irp);
    ext = (PGPIOCTRL_FDO_EXT)DeviceObject->DeviceExtension;

    GpioCtrl_Log("REMOVE: Entered for device %p\n", DeviceObject);

    ext->Removed = TRUE;

    if (ext->Started) {
        GpioCtrl_Log("REMOVE: Device still started, calling StopDevice\n");
        (VOID)GpioCtrl_StopDevice(DeviceObject, Irp);
    }

    GpioCtrl_FlushIsrLog(ext);

    GpioCtrl_Log("REMOVE: Deleting device object %p\n", DeviceObject);
    IoDeleteDevice(DeviceObject);

    GpioCtrl_Log("REMOVE: Completed successfully\n");
    return STATUS_SUCCESS;
}



/* ---------------------------------------------------------------------------
   MMIO helpers
   --------------------------------------------------------------------------- */
ULONG
GpioRegRead(
    IN PGPIOCTRL_FDO_EXT Ext,
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
    IN PGPIOCTRL_FDO_EXT Ext,
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
    PGPIOCTRL_FDO_EXT Ext
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
        GpioCtrl_Log("LPSS: ERROR – power domain did not turn on\n");
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
    PGPIOCTRL_FDO_EXT Ext
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
       NOTE: Offsets are platform‑specific.
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
        GpioCtrl_Log("LPSS: WARNING – clock status did not assert\n");
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
        GpioCtrl_Log("LPSS: WARNING – reset status did not assert\n");
    }

    GpioCtrl_Log("LPSS: Clock/Reset sequence completed\n");
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   Flush ISR circular buffer to gpioctrl.log (PASSIVE_LEVEL only)
   --------------------------------------------------------------------------- */
VOID
GpioCtrl_FlushIsrLog(
    PGPIOCTRL_FDO_EXT Ext
    )
{
    KIRQL oldIrql;
    ULONG i;

    /* Only safe at PASSIVE_LEVEL */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;

    KeAcquireSpinLock(&Ext->IsrLogLock, &oldIrql);

    for (i = Ext->IsrLogTail; i < Ext->IsrLogHead; i++) {
        ULONG idx = i % GPIO_LOG_SIZE;
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
