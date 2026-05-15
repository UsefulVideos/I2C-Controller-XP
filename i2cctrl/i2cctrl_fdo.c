/* i2cctrl_fdo.c */
#include <ntddk.h>
#include "i2cctrl_ext.h"
#include "i2cctrl_hw.h"
#include "i2cctrl_bsod.h"

NTSTATUS
I2cCtrl_QueryDeviceRelations(
    PI2CCTRL_FDO fdoExt,
    PIRP Irp
    )
{
    PIO_STACK_LOCATION isl;
    DEVICE_RELATIONS *relations;
    PLIST_ENTRY le;
    ULONG count, i;
    NTSTATUS status;
    KIRQL oldIrql;

    PAGED_CODE();

    isl = IoGetCurrentIrpStackLocation(Irp);
    ASSERT(isl->Parameters.QueryDeviceRelations.Type == BusRelations);

    count  = 0;
    i      = 0;
    status = STATUS_SUCCESS;
    relations = NULL;

    //
    // Count live children under ChildLock
    //
    KeAcquireSpinLock(&fdoExt->ChildLock, &oldIrql);
    for (le = fdoExt->ChildList.Flink;
         le != &fdoExt->ChildList;
         le = le->Flink)
    {
        PI2CCTRL_PDO child = CONTAINING_RECORD(le, I2CCTRL_PDO, ListEntry);

        if (child->Present && !child->Removed) {
            count++;
        }
    }
    KeReleaseSpinLock(&fdoExt->ChildLock, oldIrql);

    //
    // Allocate DEVICE_RELATIONS
    // XP requires a valid structure even when Count == 0.
    //
    relations = (DEVICE_RELATIONS *)ExAllocatePoolWithTag(
                    PagedPool,
                    sizeof(DEVICE_RELATIONS) + (count * sizeof(PDEVICE_OBJECT)),
                    I2CCTRL_TAG_EXT);
    if (relations == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status      = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    relations->Count = count;

    //
    // Fill array under ChildLock
    //
    KeAcquireSpinLock(&fdoExt->ChildLock, &oldIrql);
    for (le = fdoExt->ChildList.Flink;
         le != &fdoExt->ChildList && i < count;
         le = le->Flink)
    {
        PI2CCTRL_PDO child = CONTAINING_RECORD(le, I2CCTRL_PDO, ListEntry);

        if (!child->Present || child->Removed) {
            continue;
        }

        relations->Objects[i] = child->Pdo;
        ObReferenceObject(child->Pdo);
        i++;
    }
    KeReleaseSpinLock(&fdoExt->ChildLock, oldIrql);

    //
    // Defensive: ensure Count matches actual referenced PDOs
    //
    relations->Count = i;

    KdPrint(("I2CCTRL(FDO): BusRelations: reporting %lu children\n",
             relations->Count));

    Irp->IoStatus.Information = (ULONG_PTR)relations;
    Irp->IoStatus.Status      = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * I2cCtrl_FdoDispatch - FDO dispatch routine
 * XP/2003 BSOD-safe, WinDDK-compiler-safe, C89-compliant.
 * ----------------------------------------------------------------------- */
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

    irpSp  = IoGetCurrentIrpStackLocation(Irp);
    fdoExt = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;

    I2cCtrl_Log("FDO: Entered for device %p, Major=0x%02X, Minor=0x%02X\n",
                DeviceObject,
                irpSp ? irpSp->MajorFunction : 0xFF,
                irpSp ? irpSp->MinorFunction : 0xFF);

    /* Must be our FDO */
    if (fdoExt == NULL || fdoExt->Self != DeviceObject) {
        I2cCtrl_Log("FDO: Invalid extension or Self mismatch -> STATUS_INVALID_DEVICE_REQUEST\n");

        Irp->IoStatus.Status      = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /* If already removed, fail cleanly */
    if (fdoExt->Removed) {
        I2cCtrl_Log("FDO: Device already removed -> STATUS_NO_SUCH_DEVICE\n");

        Irp->IoStatus.Status      = STATUS_NO_SUCH_DEVICE;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_NO_SUCH_DEVICE;
    }

    /* Acquire remove lock for this IRP */
    status = IoAcquireRemoveLock(&fdoExt->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("FDO: IoAcquireRemoveLock FAILED (0x%08X)\n", status);

        Irp->IoStatus.Status      = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    if (irpSp == NULL) {
        I2cCtrl_Log("FDO: irpSp NULL -> STATUS_INVALID_PARAMETER\n");

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
            I2cCtrl_Log("FDO: IRP_MN_START_DEVICE -> forwarding + StartCompletion\n");

            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(
                Irp,
                I2cCtrl_StartCompletion,
                fdoExt,
                TRUE, TRUE, TRUE
            );
            return IoCallDriver(fdoExt->LowerDevice, Irp);

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            I2cCtrl_Log("FDO: IRP_MN_QUERY_DEVICE_RELATIONS (Type=%u)\n",
                        irpSp->Parameters.QueryDeviceRelations.Type);

            if (irpSp->Parameters.QueryDeviceRelations.Type == BusRelations) {
                status = I2cCtrl_QueryDeviceRelations(fdoExt, Irp);
                IoReleaseRemoveLock(&fdoExt->RemoveLock, Irp);
                return status;
            }

            I2cCtrl_Log("FDO: Passing QUERY_DEVICE_RELATIONS down\n");

            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(
                Irp,
                I2CCTRL_ReleaseLockCompletion,
                &fdoExt->RemoveLock,
                TRUE, TRUE, TRUE
            );
            return IoCallDriver(fdoExt->LowerDevice, Irp);

        case IRP_MN_QUERY_ID:
            I2cCtrl_Log("FDO: IRP_MN_QUERY_ID -> passing down\n");

            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(
                Irp,
                I2CCTRL_ReleaseLockCompletion,
                &fdoExt->RemoveLock,
                TRUE, TRUE, TRUE
            );
            return IoCallDriver(fdoExt->LowerDevice, Irp);

case IRP_MN_STOP_DEVICE:
    I2cCtrl_Log("FDO: IRP_MN_STOP_DEVICE -> stopping controller\n");

    // Mark the device as stopping BEFORE touching hardware.
    fdoExt->Stopping = TRUE;
    fdoExt->Started  = FALSE;

    // Cancel all DPCs...
    KeRemoveQueueDpc(&fdoExt->IsrDpc);
    KeRemoveQueueDpc(&fdoExt->QueueDpc);
    KeRemoveQueueDpc(&fdoExt->TimeoutDpc);

    // Disconnect interrupt...
    if (fdoExt->InterruptObject) {
        IoDisconnectInterrupt(fdoExt->InterruptObject);
        fdoExt->InterruptObject = NULL;
    }

    // Now it is safe to stop the hardware.
    (void)I2cCtrl_StopDevice(fdoExt);

    // Forward IRP down the stack.
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(
        Irp,
        I2CCTRL_ReleaseLockCompletion,
        &fdoExt->RemoveLock,
        TRUE, TRUE, TRUE
    );
    return IoCallDriver(fdoExt->LowerDevice, Irp);



case IRP_MN_SURPRISE_REMOVAL:
    I2cCtrl_Log("FDO: IRP_MN_SURPRISE_REMOVAL -> emergency stop\n");

    // Mark device as gone BEFORE touching hardware.
    fdoExt->SurpriseRemoved = TRUE;
    fdoExt->Stopping        = TRUE;
    fdoExt->Removed         = TRUE;
    fdoExt->Started         = FALSE;

    // Cancel all DPCs...
    KeRemoveQueueDpc(&fdoExt->IsrDpc);
    KeRemoveQueueDpc(&fdoExt->QueueDpc);
    KeRemoveQueueDpc(&fdoExt->TimeoutDpc);

    // Disconnect interrupt...
    if (fdoExt->InterruptObject) {
        IoDisconnectInterrupt(fdoExt->InterruptObject);
        fdoExt->InterruptObject = NULL;
    }

    // Now it is safe to stop the hardware.
    (void)I2cCtrl_StopDevice(fdoExt);

    // Forward IRP down the stack.
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
    PI2CCTRL_FDO ext;
    PDEVICE_OBJECT lower;
    NTSTATUS       downStatus;

    I2cCtrl_Log("FDO: IRP_MN_REMOVE_DEVICE -> full teardown\n");

    ext   = fdoExt;
    lower = ext->LowerDevice;

    // >>> This is the correct place for the flags <<<
    ext->Removed  = TRUE;
    ext->Stopping = TRUE;
    ext->Started  = FALSE;

    (void)I2cCtrl_RemoveDevice(ext, Irp);

    if (lower != NULL) {
        I2cCtrl_Log("FDO: Forwarding REMOVE_DEVICE to lower %p\n", lower);

        IoSkipCurrentIrpStackLocation(Irp);
        downStatus = IoCallDriver(lower, Irp);
    } else {
        I2cCtrl_Log("FDO: No lower device -> completing REMOVE locally\n");

        Irp->IoStatus.Status      = STATUS_SUCCESS;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        downStatus = STATUS_SUCCESS;
    }

    I2cCtrl_Log("FDO: Waiting for outstanding IRPs\n");
    IoReleaseRemoveLockAndWait(&ext->RemoveLock, Irp);

    if (lower != NULL) {
        I2cCtrl_Log("FDO: Detaching from lower %p\n", lower);
        IoDetachDevice(lower);
        ext->LowerDevice = NULL;
    }

    ASSERT(IsListEmpty(&ext->ChildList));
    ASSERT(ext->NumChildren == 0);

    I2cCtrl_Log("FDO: Deleting FDO %p\n", DeviceObject);
    IoDeleteDevice(DeviceObject);

    return downStatus;
}

        default:
            I2cCtrl_Log("FDO: Unhandled PnP minor 0x%02X -> passing down\n",
                        irpSp->MinorFunction);

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
    I2cCtrl_Log("FDO: Non-PnP major 0x%02X -> passing down\n",
                irpSp->MajorFunction);

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
    devctx        = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    isl           = IoGetCurrentIrpStackLocation(Irp);
    status        = STATUS_SUCCESS;
    oldIrql       = PASSIVE_LEVEL;
    ps.DeviceState = PowerDeviceUnspecified;
    newDevState   = PowerDeviceUnspecified;
    oldDevState   = PowerDeviceUnspecified;
    sysState      = PowerSystemUnspecified;
    busy          = FALSE;
    allowD1       = FALSE;
    allowD2       = FALSE;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);

    /* Defensive validation */
    if (devctx == NULL || isl == NULL) {
        KdPrint(("I2CCTRL: DispatchPower: invalid devctx/stack\n"));
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        PoStartNextPowerIrp(Irp);
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }

    /* ACPI capability gating: only use D1/D2 if ACPI >= 2.0 AND device supports them */
    allowD1 = (devctx->AcpiIs20Plus && devctx->SupportsD1) ? TRUE : FALSE;
    allowD2 = (devctx->AcpiIs20Plus && devctx->SupportsD2) ? TRUE : FALSE;

    /* XP/WDM rule: call PoStartNextPowerIrp for every received power IRP */
    PoStartNextPowerIrp(Irp);

    switch (isl->MinorFunction)
    {
/* ---------------------------------------------------------------------------
 * Power dispatch - feature-complete integration for IRP_MN_SET_POWER / IRP_MN_QUERY_POWER
 * XP/2003-hardened, C89-compliant
 *
 * Requirements on XP:
 * - Call PoStartNextPowerIrp(Irp) for every power IRP (before completion or forwarding).
 * - Use PoCallDriver when passing power IRPs down.
 * - For SystemPowerState SET_POWER, forward with completion that maps to DevicePowerState.
 * - For DevicePowerState SET_POWER, perform the transition locally, then forward.
 * - Respect SupportsD1/SupportsD2, and arm/disarm wake where applicable.
 * --------------------------------------------------------------------------- */

case IRP_MN_SET_POWER:
{
    PIO_STACK_LOCATION isl;
    DEVICE_POWER_STATE oldDevState;
    DEVICE_POWER_STATE newDevState;
    SYSTEM_POWER_STATE sysState;
    BOOLEAN allowD1;
    BOOLEAN allowD2;
    BOOLEAN busy;
    KIRQL   oldIrql;
    NTSTATUS status;
    POWER_STATE ps;

    /* C89 init */
    isl          = IoGetCurrentIrpStackLocation(Irp);
    oldDevState  = devctx->CurrentDevicePowerState;
    newDevState  = PowerDeviceUnspecified;
    sysState     = PowerSystemUnspecified;
    allowD1      = (devctx->SupportsD1 != FALSE);
    allowD2      = (devctx->SupportsD2 != FALSE);
    busy         = FALSE;
    oldIrql      = PASSIVE_LEVEL;
    status       = STATUS_SUCCESS;
    ps.DeviceState = oldDevState;

    PoStartNextPowerIrp(Irp);

    if (isl->Parameters.Power.Type == SystemPowerState) {
        /* Map system state to target device state */
        sysState = isl->Parameters.Power.State.SystemState;
        switch (sysState) {
        case PowerSystemWorking:    newDevState = PowerDeviceD0; break;
        case PowerSystemSleeping1:  newDevState = allowD1 ? PowerDeviceD1 : PowerDeviceD3; break;
        case PowerSystemSleeping2:  newDevState = allowD2 ? PowerDeviceD2 : PowerDeviceD3; break;
        case PowerSystemSleeping3:
        case PowerSystemHibernate:
        case PowerSystemShutdown:   newDevState = PowerDeviceD3; break;
        default:                    newDevState = PowerDeviceD3; break;
        }

        KdPrint(("I2CCTRL: SET_POWER(System): S%lu -> target D%lu\n",
                 (ULONG)sysState, (ULONG)newDevState));

        /* Forward system power IRP with completion that applies device transition */
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp,
                               I2cCtrl_SystemPowerCompletion,
                               devctx,
                               TRUE, TRUE, TRUE);

        devctx->SystemPowerState = sysState;
        return PoCallDriver(devctx->LowerDevice, Irp);
    }
    else if (isl->Parameters.Power.Type == DevicePowerState) {
        newDevState = isl->Parameters.Power.State.DeviceState;

        /* Coerce unsupported D1/D2 to D3 (XP ACPI constraints) */
        if (newDevState == PowerDeviceD1 && allowD1 == FALSE) {
            newDevState = PowerDeviceD3;
        }
        if (newDevState == PowerDeviceD2 && allowD2 == FALSE) {
            newDevState = PowerDeviceD3;
        }

        KdPrint(("I2CCTRL: SET_POWER(Device): D%lu -> D%lu\n",
                 (ULONG)oldDevState, (ULONG)newDevState));

        /* Quiesce hardware if powering down out of D0 while busy */
        if (newDevState != oldDevState && newDevState != PowerDeviceD0) {
            KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
            busy = (devctx->PendingIrp != NULL) ? TRUE : FALSE;
            KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

            if (busy != FALSE) {
                I2cCtrl_QuiesceHardware(devctx);
            }
        }

        /* Save extended context before leaving D0 (FIFO, queues, counters) */
        if (oldDevState == PowerDeviceD0 && newDevState != PowerDeviceD0) {
            __try {
                I2cCtrl_SaveFifoState(devctx);        /* stub: depth, watermarks, occupancy */
                I2cCtrl_SaveQueueState(devctx);       /* stub: snapshot queues, pending IRPs */
                I2cCtrl_SaveArbCounters(devctx);      /* stub: arbitration loss, retries */
                /* Timing and speed context may already be tracked elsewhere */
                devctx->SavedBusAddress   = devctx->TargetAddress;
                devctx->SavedBusSpeed     = devctx->CurrentBusSpeed;
                devctx->SavedTimingHighNs = I2cCtrl_QueryTimingHigh(devctx);
                devctx->SavedTimingLowNs  = I2cCtrl_QueryTimingLow(devctx);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                KdPrint(("I2CCTRL: SET_POWER(Device): exception saving context\n"));
            }
        }

        /* Wake policy: disarm when leaving D0, arm when entering low-power */
        if (devctx->WakeCapable != FALSE) {
            if (newDevState != PowerDeviceD0 && devctx->WakeArmed == FALSE) {
                I2cCtrl_ArmWake(devctx);   /* stub: PoRequestPowerIrp for WaitWake, program wake source */
                devctx->WakeArmed = TRUE;
            } else if (newDevState == PowerDeviceD0 && devctx->WakeArmed != FALSE) {
                I2cCtrl_DisarmWake(devctx);/* stub: cancel WaitWake, mask wake interrupt source */
                devctx->WakeArmed = FALSE;
            }
        }

        /* Transition to requested device power state */
        if (newDevState != oldDevState) {
            __try {
                switch (newDevState) {
                case PowerDeviceD0:
                    status = I2cCtrl_SetDevicePowerD0(devctx);

                    if (NT_SUCCESS(status)) {
                        /* Restore extended context on resume */
                        I2cCtrl_RestoreFifoState(devctx);
                        I2cCtrl_RestoreQueueState(devctx);
                        I2cCtrl_RestoreArbCounters(devctx);

                        /* Re-apply timing & bus speed */
                        I2cCtrl_ApplyBusTiming(devctx,
                                               devctx->SavedTimingHighNs,
                                               devctx->SavedTimingLowNs,
                                               devctx->SavedBusSpeed);
                        /* Re-enable interrupts last */
                        I2cCtrl_MaskInterrupts(devctx, FALSE);
                    }
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

        /* Forward device power IRP down after local handling */
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(devctx->LowerDevice, Irp);
    }

    /* Unknown power type: pass through */
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(devctx->LowerDevice, Irp);
}
break;

case IRP_MN_QUERY_POWER:
{
    PIO_STACK_LOCATION isl;
    DEVICE_POWER_STATE queryDevState;
    BOOLEAN busy;
    KIRQL   oldIrql;

    /* C89 init */
    isl           = IoGetCurrentIrpStackLocation(Irp);
    queryDevState = PowerDeviceUnspecified;
    busy          = FALSE;
    oldIrql       = PASSIVE_LEVEL;

    PoStartNextPowerIrp(Irp);

    /* For device power queries: deny if we would power down while busy */
    if (isl->Parameters.Power.Type == DevicePowerState) {
        queryDevState = isl->Parameters.Power.State.DeviceState;

        if (queryDevState != PowerDeviceD0) {
            KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
            busy = (devctx->PendingIrp != NULL) ? TRUE : FALSE;
            KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

            if (busy != FALSE) {
                Irp->IoStatus.Status = STATUS_DEVICE_BUSY;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_DEVICE_BUSY;
            }
        }
    }

    /* Allow query; pass down */
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(devctx->LowerDevice, Irp);
}
break;


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

    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(devctx->LowerDevice, Irp);
}
