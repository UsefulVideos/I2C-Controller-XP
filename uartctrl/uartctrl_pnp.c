/* -----------------------------------------------------------------------
   uartctrl_pnp.c – PnP dispatch for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl.h"
#include "uartctrl_ext.h"
#include "uartctrl_hw.h"

/* -----------------------------------------------------------------------
 * UARTCTRL_StartCompletionRoutine – signal event after lower driver START
 * ----------------------------------------------------------------------- */
static NTSTATUS
UARTCTRL_StartCompletionRoutine(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp,
    PVOID          Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    /* ISR/DPC‑safe logging only */
    UartCtrl_LogIsr("PnP: START_DEVICE completion routine invoked\n");

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);

    /* Tell the I/O manager we are taking over IRP completion */
    return STATUS_MORE_PROCESSING_REQUIRED;
}


/* -----------------------------------------------------------------------
 * UARTCTRL_DispatchPnP – handle PnP IRPs for UART controller
 * ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_DispatchPnP(
    PDEVICE_OBJECT DevObj,
    PIRP           Irp
    )
{
    PUARTCTRL_FDO      ext;
    PIO_STACK_LOCATION isl;
    NTSTATUS           status;

    ext = (PUARTCTRL_FDO)DevObj->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    UartCtrl_Log("PnP: minor=0x%02X\n", isl->MinorFunction);

    /* Acquire remove lock */
    status = IoAcquireRemoveLock(&ext->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        UartCtrl_Log("PnP: remove lock FAILED (0x%08lx)\n", status);
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    switch (isl->MinorFunction) {

    /* -------------------------------------------------------------
     * START_DEVICE
     * ------------------------------------------------------------- */
    case IRP_MN_START_DEVICE:
    {
        KEVENT ev;
        PCM_RESOURCE_LIST raw;
        PCM_RESOURCE_LIST xlat;

        UartCtrl_Log("PnP: IRP_MN_START_DEVICE\n");

        KeInitializeEvent(&ev, NotificationEvent, FALSE);

        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(
            Irp,
            UARTCTRL_StartCompletionRoutine,
            &ev,
            TRUE, TRUE, TRUE);

        status = IoCallDriver(ext->LowerDevice, Irp);

        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
            status = Irp->IoStatus.Status;
        }

        if (!NT_SUCCESS(status)) {
            UartCtrl_Log("PnP: lower driver START failed (0x%08lx)\n", status);
            Irp->IoStatus.Status = status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            IoReleaseRemoveLock(&ext->RemoveLock, Irp);
            return status;
        }

        raw  = isl->Parameters.StartDevice.AllocatedResources;
        xlat = isl->Parameters.StartDevice.AllocatedResourcesTranslated;

        status = UARTCTRL_StartDevice(ext, raw, xlat);

        UartCtrl_Log("PnP: StartDevice returned 0x%08lx\n", status);

        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;
    }

    /* -------------------------------------------------------------
     * STOP_DEVICE
     * ------------------------------------------------------------- */
    case IRP_MN_STOP_DEVICE:
        UartCtrl_Log("PnP: IRP_MN_STOP_DEVICE\n");

        UARTCTRL_StopDevice(ext);
        ext->Started = FALSE;

        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);

        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;

    /* -------------------------------------------------------------
     * SURPRISE_REMOVAL
     * ------------------------------------------------------------- */
    case IRP_MN_SURPRISE_REMOVAL:
        UartCtrl_Log("PnP: IRP_MN_SURPRISE_REMOVAL\n");

        ext->Removed = TRUE;
        UARTCTRL_StopDevice(ext);

        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);

        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;

    /* -------------------------------------------------------------
     * REMOVE_DEVICE
     * ------------------------------------------------------------- */
    case IRP_MN_REMOVE_DEVICE:
    {
        KIRQL irql;

        UartCtrl_Log("PnP: IRP_MN_REMOVE_DEVICE\n");

        ext->Removed = TRUE;
        ext->Started = FALSE;

        UARTCTRL_StopDevice(ext);

        /* Flush queued read IRPs */
        KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
        while (!IsListEmpty(&ext->ReadQueue)) {

            PLIST_ENTRY le = RemoveHeadList(&ext->ReadQueue);
            PIRP p = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);

            UartCtrl_Log("PnP: completing queued read IRP %p\n", p);

            p->IoStatus.Status      = STATUS_CANCELLED;
            p->IoStatus.Information = 0;
            IoCompleteRequest(p, IO_NO_INCREMENT);
        }
        KeReleaseSpinLock(&ext->ReadQueueLock, irql);

        /* Pass REMOVE down */
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);

        /* Delete symbolic link */
        if (ext->Symlink.Buffer != NULL) {
            UartCtrl_Log("PnP: deleting symlink '%wZ'\n", &ext->Symlink);
            IoDeleteSymbolicLink(&ext->Symlink);
            RtlZeroMemory(&ext->Symlink, sizeof(ext->Symlink));
        }

        /* Remove from global list */
        KeAcquireSpinLockAtDpcLevel(&g_UartCtrlGlobal.GlobalLock);
        RemoveEntryList(&ext->ListEntry);
        KeReleaseSpinLockFromDpcLevel(&g_UartCtrlGlobal.GlobalLock);

        /* Detach */
        IoDetachDevice(ext->LowerDevice);

        /* Final remove lock release */
        IoReleaseRemoveLockAndWait(&ext->RemoveLock, Irp);

        /* Delete FDO */
        UartCtrl_Log("PnP: deleting FDO %p\n", DevObj);
        IoDeleteDevice(DevObj);

        return status;
    }

    /* -------------------------------------------------------------
     * Default: pass through
     * ------------------------------------------------------------- */
    default:
        UartCtrl_Log("PnP: passing minor 0x%02X down\n",
                     isl->MinorFunction);

        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);

        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;
    }
}
