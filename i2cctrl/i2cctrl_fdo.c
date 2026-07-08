/* i2cctrl_fdo.c */
#include <ntddk.h>
#include "i2cctrl_ext.h"
#include "i2cctrl_hw.h"
#include "i2cctrl_bsod.h"
#include "i2cctrl_detect.h"

NTSTATUS
I2cCtrl_QueryDeviceRelations(
    PI2CCTRL_FDO fdoExt,
    PIRP         Irp
    )
{
    PIO_STACK_LOCATION isl;
    DEVICE_RELATIONS  *relations;
    PLIST_ENTRY        le;
    ULONG              count, i;
    ULONG              size;
    KIRQL              oldIrql;

    PAGED_CODE();

    isl = IoGetCurrentIrpStackLocation(Irp);
    ASSERT(isl->Parameters.QueryDeviceRelations.Type == BusRelations);

    count = 0;
    i     = 0;

    /* Count children */
    KeAcquireSpinLock(&fdoExt->ChildLock, &oldIrql);
    for (le = fdoExt->ChildList.Flink;
         le != &fdoExt->ChildList;
         le = le->Flink)
    {
        PI2CCTRL_PDO child = CONTAINING_RECORD(le, I2CCTRL_PDO, ListEntry);
        if (child->Present && !child->Removed)
            count++;
    }
    KeReleaseSpinLock(&fdoExt->ChildLock, oldIrql);

    /* Correct allocation */
    size = sizeof(DEVICE_RELATIONS);
    if (count > 1)
        size += (count - 1) * sizeof(PDEVICE_OBJECT);

    relations = ExAllocatePoolWithTag(PagedPool, size, I2CCTRL_TAG_EXT);
    if (!relations) {
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    /* Fill */
    KeAcquireSpinLock(&fdoExt->ChildLock, &oldIrql);
    for (le = fdoExt->ChildList.Flink;
         le != &fdoExt->ChildList && i < count;
         le = le->Flink)
    {
        PI2CCTRL_PDO child = CONTAINING_RECORD(le, I2CCTRL_PDO, ListEntry);
        if (!child->Present || child->Removed)
            continue;

        relations->Objects[i] = child->Pdo;
        ObReferenceObject(child->Pdo);
        i++;
    }
    KeReleaseSpinLock(&fdoExt->ChildLock, oldIrql);

    relations->Count = i;

    Irp->IoStatus.Information = (ULONG_PTR)relations;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}


NTSTATUS
I2cCtrl_FdoDispatch(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PIO_STACK_LOCATION irpSp;
    PI2CCTRL_FDO       fdoExt;
    NTSTATUS           status;

    PAGED_CODE();
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    if (DeviceObject == NULL || Irp == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* All logging removed */

    irpSp  = IoGetCurrentIrpStackLocation(Irp);
    fdoExt = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;

    /* Must be our FDO */
    if (fdoExt == NULL || fdoExt->Self != DeviceObject) {

        Irp->IoStatus.Status      = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /* If already removed, fail cleanly */
    if (fdoExt->Removed) {

        Irp->IoStatus.Status      = STATUS_NO_SUCH_DEVICE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NO_SUCH_DEVICE;
    }

    /* Acquire remove lock for this IRP */
    status = IoAcquireRemoveLock(&fdoExt->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {

        Irp->IoStatus.Status      = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    if (irpSp == NULL) {

        IoReleaseRemoveLock(&fdoExt->RemoveLock, Irp);
        Irp->IoStatus.Status      = STATUS_INVALID_PARAMETER;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    switch (irpSp->MajorFunction) {

    case IRP_MJ_PNP:

        switch (irpSp->MinorFunction) {

        case IRP_MN_START_DEVICE:

            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(
                Irp,
                I2cCtrl_StartCompletion,
                fdoExt,
                TRUE, TRUE, TRUE
            );
            return IoCallDriver(fdoExt->LowerDevice, Irp);

        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            ULONG type = irpSp->Parameters.QueryDeviceRelations.Type;

            if (type == BusRelations)
            {
                if (fdoExt->ReadyForChildren)
                {
                    (void)I2cCtrl_CreateI2cDevice(fdoExt->Self, fdoExt);
                }

                status = I2cCtrl_QueryDeviceRelations(fdoExt, Irp);

                IoReleaseRemoveLock(&fdoExt->RemoveLock, Irp);
                return status;
            }

            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(
                Irp,
                I2CCTRL_ReleaseLockCompletion,
                &fdoExt->RemoveLock,
                TRUE, TRUE, TRUE
            );
            return IoCallDriver(fdoExt->LowerDevice, Irp);
        }

        case IRP_MN_QUERY_ID:
        {
            ULONG idType = irpSp->Parameters.QueryId.IdType;

            if (idType == BusQueryDeviceID)
            {
                static const WCHAR busId[] = L"I2CCTRL";
                SIZE_T len = sizeof(busId);
                PWSTR buf;

                buf = (PWSTR)ExAllocatePoolWithTag(PagedPool, len, 'odfI');
                if (!buf) {
                    status = STATUS_INSUFFICIENT_RESOURCES;
                } else {
                    RtlCopyMemory(buf, busId, len);
                    Irp->IoStatus.Information = (ULONG_PTR)buf;
                    status = STATUS_SUCCESS;
                }

                Irp->IoStatus.Status = status;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return status;
            }

            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(
                Irp,
                I2CCTRL_ReleaseLockCompletion,
                &fdoExt->RemoveLock,
                TRUE, TRUE, TRUE
            );
            return IoCallDriver(fdoExt->LowerDevice, Irp);
        }

        case IRP_MN_STOP_DEVICE:

            fdoExt->Stopping = TRUE;
            fdoExt->Started  = FALSE;

            KeRemoveQueueDpc(&fdoExt->IsrDpc);
            KeRemoveQueueDpc(&fdoExt->QueueDpc);
            KeRemoveQueueDpc(&fdoExt->TimeoutDpc);

            if (fdoExt->InterruptObject) {
                IoDisconnectInterrupt(fdoExt->InterruptObject);
                fdoExt->InterruptObject = NULL;
            }

            (void)I2cCtrl_StopDevice(fdoExt);

            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(
                Irp,
                I2CCTRL_ReleaseLockCompletion,
                &fdoExt->RemoveLock,
                TRUE, TRUE, TRUE
            );
            return IoCallDriver(fdoExt->LowerDevice, Irp);

        case IRP_MN_SURPRISE_REMOVAL:

            fdoExt->SurpriseRemoved = TRUE;
            fdoExt->Stopping        = TRUE;
            fdoExt->Removed         = TRUE;
            fdoExt->Started         = FALSE;

            KeRemoveQueueDpc(&fdoExt->IsrDpc);
            KeRemoveQueueDpc(&fdoExt->QueueDpc);
            KeRemoveQueueDpc(&fdoExt->TimeoutDpc);

            if (fdoExt->InterruptObject) {
                IoDisconnectInterrupt(fdoExt->InterruptObject);
                fdoExt->InterruptObject = NULL;
            }

            (void)I2cCtrl_StopDevice(fdoExt);

            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(
                Irp,
                I2CCTRL_ReleaseLockCompletion,
                &fdoExt->RemoveLock,
                TRUE, TRUE, TRUE
            );
            return IoCallDriver(fdoExt->LowerDevice, Irp);

        case IRP_MN_REMOVE_DEVICE:
        {
            PI2CCTRL_FDO   ext;
            PDEVICE_OBJECT lower;
            NTSTATUS       downStatus;

            ext   = fdoExt;
            lower = ext->LowerDevice;

            ext->Removed  = TRUE;
            ext->Stopping = TRUE;
            ext->Started  = FALSE;

            (void)I2cCtrl_RemoveDevice(ext, Irp);

            if (lower != NULL) {

                IoSkipCurrentIrpStackLocation(Irp);
                downStatus = IoCallDriver(lower, Irp);
            } else {

                Irp->IoStatus.Status      = STATUS_SUCCESS;
                Irp->IoStatus.Information = 0;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                downStatus = STATUS_SUCCESS;
            }

            IoReleaseRemoveLockAndWait(&ext->RemoveLock, Irp);

            if (lower != NULL) {
                IoDetachDevice(lower);
                ext->LowerDevice = NULL;
            }

            ASSERT(IsListEmpty(&ext->ChildList));
            ASSERT(ext->NumChildren == 0);

            IoDeleteDevice(DeviceObject);

            return downStatus;
        }

        default:

            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(
                Irp,
                I2CCTRL_ReleaseLockCompletion,
                &fdoExt->RemoveLock,
                TRUE, TRUE, TRUE
            );
            return IoCallDriver(fdoExt->LowerDevice, Irp);
        }

        /* NOT REACHED */
    }

    /* Non-PnP majors */

    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(
        Irp,
        I2CCTRL_ReleaseLockCompletion,
        &fdoExt->RemoveLock,
        TRUE, TRUE, TRUE
    );
    return IoCallDriver(fdoExt->LowerDevice, Irp);
}


/*
 * Initialize FDO extension fields when the device is created.
 * Called from AddDevice or IRP_MN_START_DEVICE.
 */
NTSTATUS
I2cCtrl_InitFdoExtension(
    PI2CCTRL_FDO fdoExt,
    PDEVICE_OBJECT DeviceObject
    )
{
    if (!fdoExt || !DeviceObject) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(fdoExt, sizeof(I2CCTRL_FDO));

    fdoExt->Self = DeviceObject;
    InitializeListHead(&fdoExt->ChildList);
    I2CCTRL_INIT_LOCK(&fdoExt->ChildLock);
    fdoExt->NumChildren = 0;
    fdoExt->LowerDevice = NULL;
    fdoExt->PhysicalDevice = NULL;

    return STATUS_SUCCESS;
}

/*
 * Stop FDO: release resources but keep PDOs alive until removed.
 * Called from IRP_MN_REMOVE_DEVICE or IRP_MN_SURPRISE_REMOVAL.
 */
VOID
I2cCtrl_StopFdo(
    PI2CCTRL_FDO fdoExt
    )
{
    if (!fdoExt) {
        return;
    }

    // Example: cancel timers, release hardware state
    // (actual hardware cleanup is in I2cCtrl_StopDevice)

    // Clear child list count (PDOs will be deleted separately)
    fdoExt->NumChildren = 0;
}

/*
 * Generic dispatch for non-PnP IRPs at the FDO.
 * Passes through to lower driver if attached.
 */
NTSTATUS
I2cCtrl_FdoGenericDispatch(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    PI2CCTRL_FDO fdoExt = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;

    if (fdoExt && fdoExt->LowerDevice) {
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(fdoExt->LowerDevice, Irp);
    }

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
 * FDO Power Dispatch (ACPI 1.0b/2.0+ compatible, XP-BSOD-safe, WDM/WinDDK-safe, C89-compliant)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_FdoDispatchPower(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PI2CCTRL_FDO       devctx;
    PIO_STACK_LOCATION isl;
    NTSTATUS           status;
    KIRQL              oldIrql;
    POWER_STATE        ps;
    DEVICE_POWER_STATE newDevState;
    DEVICE_POWER_STATE oldDevState;
    SYSTEM_POWER_STATE sysState;
    BOOLEAN            busy;
    BOOLEAN            allowD1;
    BOOLEAN            allowD2;

    /* C89: declare and init locals up-front */
    devctx         = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    isl            = IoGetCurrentIrpStackLocation(Irp);
    status         = STATUS_SUCCESS;
    oldIrql        = PASSIVE_LEVEL;
    ps.DeviceState = PowerDeviceUnspecified;
    newDevState    = PowerDeviceUnspecified;
    oldDevState    = PowerDeviceUnspecified;
    sysState       = PowerSystemUnspecified;
    busy           = FALSE;
    allowD1        = FALSE;
    allowD2        = FALSE;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    /* Defensive validation */
    if (devctx == NULL || isl == NULL) {
        KdPrint(("I2CCTRL: FdoDispatchPower: invalid devctx/stack\n"));
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }
	
	/* IRQL-safe logging: only at PASSIVE_LEVEL */
if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
    I2cCtrl_Log("FDO Power Dispatch: Minor=%lu for FDO %p\n",
                (ULONG)isl->MinorFunction,
                DeviceObject);
}
	
    /* ACPI capability gating: only use D1/D2 if ACPI >= 2.0 AND device supports them */
    allowD1 = (devctx->AcpiIs20Plus && devctx->SupportsD1) ? TRUE : FALSE;
    allowD2 = (devctx->AcpiIs20Plus && devctx->SupportsD2) ? TRUE : FALSE;

    /* XP/WDM rule: call PoStartNextPowerIrp for every received power IRP */
    PoStartNextPowerIrp(Irp);

    switch (isl->MinorFunction) {

    case IRP_MN_SET_POWER:

        if (isl->Parameters.Power.Type == SystemPowerState) {

            /* Map system state to target device state */
            sysState = isl->Parameters.Power.State.SystemState;

            switch (sysState) {
            case PowerSystemWorking:
                newDevState = PowerDeviceD0;
                break;

            case PowerSystemSleeping1:
                newDevState = allowD1 ? PowerDeviceD1 : PowerDeviceD3;
                break;

            case PowerSystemSleeping2:
                newDevState = allowD2 ? PowerDeviceD2 : PowerDeviceD3;
                break;

            case PowerSystemSleeping3:
            case PowerSystemHibernate:
            case PowerSystemShutdown:
            default:
                newDevState = PowerDeviceD3;
                break;
            }

            KdPrint(("I2CCTRL: SET_POWER(System): S%lu -> target D%lu\n",
                     (ULONG)sysState, (ULONG)newDevState));

            /* Forward system power IRP with completion that applies device transition */
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp,
                                   I2cCtrl_SystemPowerCompletion,
                                   devctx,
                                   TRUE,
                                   TRUE,
                                   TRUE);

            devctx->SystemPowerState = sysState;
            return PoCallDriver(devctx->LowerDevice, Irp);
        }

        if (isl->Parameters.Power.Type == DevicePowerState) {

            oldDevState = devctx->CurrentDevicePowerState;
            newDevState = isl->Parameters.Power.State.DeviceState;

            /* Coerce unsupported D1/D2 to D3 */
            if (newDevState == PowerDeviceD1 && !allowD1) {
                newDevState = PowerDeviceD3;
            }
            if (newDevState == PowerDeviceD2 && !allowD2) {
                newDevState = PowerDeviceD3;
            }

            KdPrint(("I2CCTRL: SET_POWER(Device): D%lu -> D%lu\n",
                     (ULONG)oldDevState, (ULONG)newDevState));

            /* Quiesce hardware if powering down out of D0 while busy */
            if (newDevState != oldDevState && newDevState != PowerDeviceD0) {
                KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
                busy = (devctx->PendingIrp != NULL) ? TRUE : FALSE;
                KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

                if (busy) {
                    I2cCtrl_QuiesceHardware(devctx);
                }
            }

            /* Save extended context before leaving D0 */
            if (oldDevState == PowerDeviceD0 && newDevState != PowerDeviceD0) {
                __try {
                    I2cCtrl_SaveFifoState(devctx);
                    I2cCtrl_SaveQueueState(devctx);
                    I2cCtrl_SaveArbCounters(devctx);

                    devctx->SavedBusAddress   = devctx->TargetAddress;
                    devctx->SavedBusSpeed     = devctx->CurrentBusSpeed;
                    devctx->SavedTimingHighNs = I2cCtrl_QueryTimingHigh(devctx);
                    devctx->SavedTimingLowNs  = I2cCtrl_QueryTimingLow(devctx);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    KdPrint(("I2CCTRL: SET_POWER(Device): exception saving context\n"));
                }
            }

            /* Wake policy */
            if (devctx->WakeCapable) {
                if (newDevState != PowerDeviceD0 && !devctx->WakeArmed) {
                    I2cCtrl_ArmWake(devctx);
                    devctx->WakeArmed = TRUE;
                } else if (newDevState == PowerDeviceD0 && devctx->WakeArmed) {
                    I2cCtrl_DisarmWake(devctx);
                    devctx->WakeArmed = FALSE;
                }
            }

            /* Transition to requested device power state */
            if (newDevState != oldDevState) {
                __try {
                    switch (newDevState) {
                    case PowerDeviceD0:
                        status = I2cCtrl_SetControllerPowerD0(devctx);
                        if (NT_SUCCESS(status)) {
                            I2cCtrl_RestoreFifoState(devctx);
                            I2cCtrl_RestoreQueueState(devctx);
                            I2cCtrl_RestoreArbCounters(devctx);

                            I2cCtrl_ApplyBusTiming(devctx,
                                                   devctx->SavedTimingHighNs,
                                                   devctx->SavedTimingLowNs,
                                                   devctx->SavedBusSpeed);

                            I2cCtrl_MaskInterrupts(devctx, FALSE);
                        }
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
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    KdPrint(("I2CCTRL: SET_POWER(Device): exception during transition\n"));
                    status = STATUS_ACCESS_VIOLATION;
                }

                if (NT_SUCCESS(status)) {
                    ps.DeviceState = newDevState;
                    PoSetPowerState(devctx->Self, DevicePowerState, ps);
                    devctx->CurrentDevicePowerState = newDevState;
                    Irp->IoStatus.Status = STATUS_SUCCESS;
                } else {
                    Irp->IoStatus.Status = status;
                }
            }

            IoSkipCurrentIrpStackLocation(Irp);
            return PoCallDriver(devctx->LowerDevice, Irp);
        }

        /* Unknown power type: pass through */
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(devctx->LowerDevice, Irp);

    case IRP_MN_QUERY_POWER:
        /* For device power queries: deny if we would power down while busy */
        if (isl->Parameters.Power.Type == DevicePowerState) {
            newDevState = isl->Parameters.Power.State.DeviceState;

            if (newDevState != PowerDeviceD0) {
                KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
                busy = (devctx->PendingIrp != NULL) ? TRUE : FALSE;
                KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

                if (busy) {
                    Irp->IoStatus.Status = STATUS_DEVICE_BUSY;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);
                    return STATUS_DEVICE_BUSY;
                }
            }
        }

        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(devctx->LowerDevice, Irp);

    case IRP_MN_WAIT_WAKE:

        if (!devctx->WakeCapable) {
            Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_NOT_SUPPORTED;
        }

        KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
        if (devctx->PendingIrp != NULL) {
            KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);
            Irp->IoStatus.Status = STATUS_DEVICE_BUSY;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_DEVICE_BUSY;
        }

        devctx->PendingIrp = Irp;
        IoMarkIrpPending(Irp);
        IoSetCancelRoutine(Irp, I2cCtrl_CancelWakeIrp);
        KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

        I2cCtrl_EnableWakeSignal(devctx);

        IoSkipCurrentIrpStackLocation(Irp);
        (void)PoCallDriver(devctx->LowerDevice, Irp);
        return STATUS_PENDING;

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(devctx->LowerDevice, Irp);
    }
}
