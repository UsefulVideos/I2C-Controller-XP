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

/* ---------------------------------------------------------------------------
   DriverEntry
   --------------------------------------------------------------------------- */
NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT  DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
{
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload               = GpioCtrl_Unload;
    DriverObject->DriverExtension->AddDevice = GpioCtrl_AddDevice;

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = GpioCtrl_DispatchCreateClose;
    }

    DriverObject->MajorFunction[IRP_MJ_PNP]            = GpioCtrl_DispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]          = GpioCtrl_DispatchPower;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = GpioCtrl_DispatchIoctl;

    return STATUS_SUCCESS;
}

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

/* ---------------------------------------------------------------------------
   AddDevice (single-FDO bus/controller driver)
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_AddDevice(
    IN PDRIVER_OBJECT  DriverObject,
    IN PDEVICE_OBJECT  PhysicalDeviceObject
    )
{
    NTSTATUS status;
    PDEVICE_OBJECT fdo;
    PGPIOCTRL_FDO_EXT ext;

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

    ext = (PGPIOCTRL_FDO_EXT)fdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));

    ext->Self                = fdo;
    ext->LowerDevice         = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    ext->Signature           = GPIOCTRL_FDO_SIGNATURE;

    /* Default capabilities/policy; refine in StartDevice */
    ext->SupportsPull        = 1;
    ext->SupportsInterrupts  = 1;
    ext->SupportsDebounce    = 1;
    ext->PinCount            = 32;
    ext->DebounceDefaultMs   = 10;
    ext->CrashOnError        = 0;

    KeInitializeSpinLock(&ext->RegLock);
    KeInitializeDpc(&ext->IsrDpc, GpioCtrl_Dpc, ext);
    ext->PendingIntMask = 0;

    fdo->Flags |= DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   PnP dispatch
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_DispatchPnp(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    NTSTATUS status;
    PIO_STACK_LOCATION isl;
    PGPIOCTRL_FDO_EXT ext;

    isl = IoGetCurrentIrpStackLocation(Irp);
    ext = (PGPIOCTRL_FDO_EXT)DeviceObject->DeviceExtension;
    status = STATUS_SUCCESS;

    switch (isl->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = GpioCtrl_StartDevice(DeviceObject, Irp);
        break;

    case IRP_MN_STOP_DEVICE:
        status = GpioCtrl_StopDevice(DeviceObject, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        status = GpioCtrl_RemoveDevice(DeviceObject, Irp);
        break;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);
        return status;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

/* ---------------------------------------------------------------------------
   Power dispatch (pass-through)
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_DispatchPower(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PGPIOCTRL_FDO_EXT ext;
    ext = (PGPIOCTRL_FDO_EXT)DeviceObject->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(ext->LowerDevice, Irp);
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

/* ---------------------------------------------------------------------------
   START device: pass down, parse resources, map MMIO, connect interrupt, load policy
   --------------------------------------------------------------------------- */
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

    ext = (PGPIOCTRL_FDO_EXT)DeviceObject->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    /* Pass START down first */
    IoCopyCurrentIrpStackLocationToNext(Irp);
    status = IoCallDriver(ext->LowerDevice, Irp);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    resTranslated = isl->Parameters.StartDevice.AllocatedResourcesTranslated;
    resRaw        = isl->Parameters.StartDevice.AllocatedResources;
    UNREFERENCED_PARAMETER(resRaw);

    /* Parse translated resources */
    if (resTranslated != NULL) {
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
                    ext->MmioBasePa = desc->u.Memory.Start;
                    ext->MmioLength = desc->u.Memory.Length;
                    break;

                case CmResourceTypeInterrupt:
                    ext->Vector        = desc->u.Interrupt.Vector;
                    ext->Irql          = (KIRQL)desc->u.Interrupt.Level;
                    ext->SynchIrql     = (KIRQL)desc->u.Interrupt.Level;
                    ext->Affinity      = desc->u.Interrupt.Affinity;
                    ext->InterruptMode = (desc->Flags & CM_RESOURCE_INTERRUPT_LATCHED)
                                         ? Latched : LevelSensitive;
                    break;

                default:
                    break;
                }
            }
        }
    }

    /* Map MMIO */
    if (ext->MmioLength != 0) {
        ext->MmioBase = (PUCHAR)MmMapIoSpace(ext->MmioBasePa, ext->MmioLength, MmNonCached);
        if (ext->MmioBase == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /* Load registry policy (optional) */
    GpioCtrl_LoadRegistryPolicy(ext, NULL);

    /* Initialize hardware defaults: disable interrupts, clear pending */
    if (ext->MmioBase != NULL) {
        KIRQL oldIrql;
        ULONG mask;

        KeAcquireSpinLock(&ext->RegLock, &oldIrql);
        GpioRegWrite(ext, REG_INT_EN_OFFSET, 0);
        mask = GpioRegRead(ext, REG_INT_STAT_OFFSET);
        if (mask != 0) {
            GpioRegWrite(ext, REG_INT_STAT_OFFSET, mask); /* W1C all pending */
        }
        KeReleaseSpinLock(&ext->RegLock, oldIrql);
    }

    /* Connect interrupt if available */
    if (ext->Vector != 0) {
        status = IoConnectInterrupt(
            &ext->InterruptObject,
            GpioCtrl_Isr,
            ext,
            &ext->RegLock,      /* synchronization lock */
            ext->Vector,
            ext->Irql,
            ext->SynchIrql,
            ext->InterruptMode,
            TRUE,               /* ShareVector */
            ext->Affinity,
            FALSE               /* FloatingSave */
        );
        if (!NT_SUCCESS(status)) {
            ext->InterruptObject    = NULL;
            ext->InterruptConnected = FALSE;
        } else {
            ext->InterruptConnected = TRUE;
        }
    }

    ext->Started = TRUE;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   STOP device: disconnect interrupt, unmap MMIO
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

    if (ext->InterruptConnected && ext->InterruptObject != NULL) {
        IoDisconnectInterrupt(ext->InterruptObject);
        ext->InterruptObject    = NULL;
        ext->InterruptConnected = FALSE;
    }

    if (ext->MmioBase != NULL) {
        MmUnmapIoSpace(ext->MmioBase, ext->MmioLength);
        ext->MmioBase   = NULL;
        ext->MmioLength = 0;
    }

    ext->Started = FALSE;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   REMOVE device: ensure stopped, detach, delete
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
    ext->Removed = TRUE;

    if (ext->Started) {
        (VOID)GpioCtrl_StopDevice(DeviceObject, Irp);
    }

    if (ext->LowerDevice != NULL) {
        IoDetachDevice(ext->LowerDevice);
        ext->LowerDevice = NULL;
    }

    IoDeleteDevice(DeviceObject);
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
