/* -----------------------------------------------------------------------
   uartctrl_pnp.c – PnP dispatch for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl.h"
#include "uartctrl_ext.h"
#include "uartctrl_hw.h"

/* Completion routine used to wait for lower driver on START_DEVICE */
static NTSTATUS
UARTCTRL_StartCompletionRoutine(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* -----------------------------------------------------------------------
   PnP Dispatch
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_DispatchPnP(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PUARTCTRL_DEVEXT ext;
    PIO_STACK_LOCATION isl;
    NTSTATUS status;

    ext = (PUARTCTRL_DEVEXT)DevObj->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    status = IoAcquireRemoveLock(&ext->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    switch (isl->MinorFunction) {

    case IRP_MN_START_DEVICE:
    {
        KEVENT ev;
        PCM_RESOURCE_LIST raw;
        PCM_RESOURCE_LIST xlat;

        KeInitializeEvent(&ev, NotificationEvent, FALSE);

        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp,
                               UARTCTRL_StartCompletionRoutine,
                               &ev,
                               TRUE, TRUE, TRUE);

        status = IoCallDriver(ext->LowerDevice, Irp);
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
            status = Irp->IoStatus.Status;
        }

        if (!NT_SUCCESS(status)) {
            Irp->IoStatus.Status = status;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            IoReleaseRemoveLock(&ext->RemoveLock, Irp);
            return status;
        }

        raw  = isl->Parameters.StartDevice.AllocatedResources;
        xlat = isl->Parameters.StartDevice.AllocatedResourcesTranslated;

        status = UARTCTRL_StartDevice(ext, raw, xlat);

        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;
    }

    case IRP_MN_STOP_DEVICE:
        UARTCTRL_StopDevice(ext);
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);
        ext->Started = FALSE;
        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;

    case IRP_MN_SURPRISE_REMOVAL:
        ext->Removed = TRUE;
        UARTCTRL_StopDevice(ext);
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);
        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;

    case IRP_MN_REMOVE_DEVICE:
    {
        KIRQL irql;

        ext->Removed = TRUE;
        ext->Started = FALSE;
        UARTCTRL_StopDevice(ext);

        /* Cancel any queued read IRPs */
        KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
        while (!IsListEmpty(&ext->ReadQueue)) {
            PLIST_ENTRY le;
            PIRP p;

            le = RemoveHeadList(&ext->ReadQueue);
            p  = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);

            p->IoStatus.Status = STATUS_CANCELLED;
            p->IoStatus.Information = 0;
            IoCompleteRequest(p, IO_NO_INCREMENT);
        }
        KeReleaseSpinLock(&ext->ReadQueueLock, irql);

        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);

        if (ext->Symlink.Buffer) {
            IoDeleteSymbolicLink(&ext->Symlink);
            RtlZeroMemory(&ext->Symlink, sizeof(ext->Symlink));
        }

        IoDetachDevice(ext->LowerDevice);
        IoReleaseRemoveLockAndWait(&ext->RemoveLock, Irp);
        IoDeleteDevice(DevObj);
        return status;
    }

    default:
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);
        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;
    }
}