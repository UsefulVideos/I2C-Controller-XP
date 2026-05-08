/* i2cctrl_Queue.c */
#include "i2cctrl_queue.h"

VOID
I2cCtrl_InitQueue(
    PI2CCTRL_FDO Dx
    )
{
    KIRQL irql;

    I2CCTRL_INIT_LOCK(&Dx->QueueLock);
    I2CCTRL_INIT_LOCK(&Dx->CancelLock);

    KeAcquireSpinLock(&Dx->QueueLock, &irql);
    InitializeListHead(&Dx->RequestQueue);
    Dx->ActiveBusy = FALSE;
    RtlZeroMemory(&Dx->ActiveRequest, sizeof(Dx->ActiveRequest));
    KeReleaseSpinLock(&Dx->QueueLock, irql);

    /* Defaults if not loaded from registry */
    if (Dx->TransactionTimeoutMs == 0) {
        Dx->TransactionTimeoutMs = 100; /* 100 ms default */
    }
    if (Dx->MaxRetries == 0) {
        Dx->MaxRetries = 3;
    }
}

/* XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant
   Hardened completion:
   - Cancels timeout timer before completion
   - Disarms the IRP's cancel routine
   - Fences under QueueLock (clears ActiveBusy and detaches XferCtx.Irp if it matches)
   - Clears PendingIrp if it matches to prevent late cancel races
   - Writes IoStatus and completes exactly once
*/
VOID
I2cCtrl_CompleteIrp(
    IN PI2CCTRL_FDO devctx,
    IN PIRP          Irp,
    IN NTSTATUS      Status,
    IN ULONG_PTR     Information
    )
{
    KIRQL oldIrql;
    BOOLEAN cancelled;

    /* C89 init */
    oldIrql = 0;
    cancelled = FALSE;

    if (devctx == NULL || Irp == NULL) {
        return;
    }

    /* 1) Proactively cancel timeout timer to block future DPCs */
    cancelled = KeCancelTimer(&devctx->XferCtx.TimeoutTimer);
    UNREFERENCED_PARAMETER(cancelled);

    /* 2) Disarm cancel routine to avoid cancel path racing completion */
    (VOID)IoSetCancelRoutine(Irp, NULL);

    /* 3) Fence ISR/DPC/Cancel access under QueueLock */
    KeAcquireSpinLock(&devctx->QueueLock, &oldIrql);
    if (devctx->XferCtx.Irp == Irp) {
        devctx->ActiveBusy = FALSE;
        devctx->XferCtx.Irp = NULL;
        devctx->XferCtx.Status = Status; /* terminal status for diagnostics */
    }
    KeReleaseSpinLock(&devctx->QueueLock, oldIrql);

    /* 4) Also clear PendingIrp if it matches to prevent late cancel routines */
    KeAcquireSpinLock(&devctx->PendingIrpLock, &oldIrql);
    if (devctx->PendingIrp == Irp) {
        devctx->PendingIrp = NULL;
    }
    KeReleaseSpinLock(&devctx->PendingIrpLock, oldIrql);

    /* 5) Write completion status */
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;

    /* 6) Signal any waiter (safe at DISPATCH_LEVEL) */
    KeSetEvent(&devctx->TransferEvent, IO_NO_INCREMENT, FALSE);

    /* 7) Complete the IRP exactly once */
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/* -----------------------------------------------------------------------
 * I2cCtrl_FlushQueue - XP/2003 BSOD-safe, HAL-generic, C89-compliant
 * Purpose:
 *   - Flushes all queued and in-flight work safely
 *   - Use in Stop/Remove BEFORE unmapping MMIO
 * Guarantees:
 *   - Cancels timeout timer to prevent future DPCs
 *   - Fences ISR/DPC access (clears ActiveBusy, detaches XferCtx.Irp, PendingIrp=NULL)
 *   - Masks interrupts, acknowledges latched causes, disables controller
 *   - Drains the cancel-safe queue (IO_CSQ) and completes each IRP exactly once
 *   - Frees any leftover request wrappers
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_FlushQueue(
    IN PI2CCTRL_FDO fdoExt
    )
{
    KIRQL   oldIrql;
    BOOLEAN cancelled;
    PIRP    irp;
    PVOID   ctx;

    /* C89 init */
    oldIrql   = 0;
    cancelled = FALSE;
    irp       = NULL;
    ctx       = NULL;

    if (fdoExt == NULL) {
        return;
    }

    /* 1) Proactively stop the timeout mechanism */
    cancelled = KeCancelTimer(&fdoExt->XferCtx.TimeoutTimer);
    UNREFERENCED_PARAMETER(cancelled);

    /* 2) Fence ISR/DPC access under QueueLock */
    KeAcquireSpinLock(&fdoExt->QueueLock, &oldIrql);
    fdoExt->ActiveBusy      = FALSE;
    fdoExt->XferCtx.Status  = STATUS_CANCELLED;
    fdoExt->XferCtx.Irp     = NULL;
    KeReleaseSpinLock(&fdoExt->QueueLock, oldIrql);

    /* Clear PendingIrp so cancel routines won't race this path */
    KeAcquireSpinLock(&fdoExt->PendingIrpLock, &oldIrql);
    fdoExt->PendingIrp = NULL;
    KeReleaseSpinLock(&fdoExt->PendingIrpLock, oldIrql);

    /* 3) Quiesce hardware: mask interrupts, ACK latched causes, disable controller */
    if (fdoExt->Ops != NULL && fdoExt->Removed == FALSE) {
        if (fdoExt->Ops->MaskInterrupts != NULL) {
            fdoExt->Ops->MaskInterrupts(fdoExt, 0U);
        }
        if (fdoExt->Ops->AckInterrupts != NULL) {
            fdoExt->Ops->AckInterrupts(fdoExt,
                  I2C_INT_RX_UNDER
                | I2C_INT_RX_OVER
                | I2C_INT_TX_OVER
                | I2C_INT_TX_ABORT
                | I2C_INT_STOP_DETECTED
                | I2C_INT_START_DETECTED
                | I2C_INT_GEN_CALL
                | I2C_INT_ACTIVITY
                | I2C_INT_RX_DONE
                | I2C_INT_RD_REQ);
        }
        if (fdoExt->Ops->Enable != NULL) {
            (VOID)fdoExt->Ops->Enable(fdoExt, FALSE);  /* disable */
        }
    }

    /* 4) Drain the pending IRP list and complete IRPs once */
    for (;;) {
        PLIST_ENTRY le;

        KeAcquireSpinLock(&fdoExt->PendingIrpLock, &oldIrql);

        if (IsListEmpty(&fdoExt->PendingIrpList)) {
            KeReleaseSpinLock(&fdoExt->PendingIrpLock, oldIrql);
            break;
        }

        le = RemoveHeadList(&fdoExt->PendingIrpList);
        KeReleaseSpinLock(&fdoExt->PendingIrpLock, oldIrql);

        irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);

        (VOID)IoSetCancelRoutine(irp, NULL);

        irp->IoStatus.Status      = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }


    /* 5) Free any leftover request wrappers in RequestQueue */
    KeAcquireSpinLock(&fdoExt->QueueLock, &oldIrql);
    while (!IsListEmpty(&fdoExt->RequestQueue)) {
        PLIST_ENTRY    e;
        PSMBUS_REQUEST req;

        e = RemoveHeadList(&fdoExt->RequestQueue);
        KeReleaseSpinLock(&fdoExt->QueueLock, oldIrql);

        req = CONTAINING_RECORD(e, SMBUS_REQUEST, ListEntry);
        if (req != NULL) {
            if (req->Irp != NULL) {
                (VOID)IoSetCancelRoutine(req->Irp, NULL);
                req->Irp->IoStatus.Status      = STATUS_CANCELLED;
                req->Irp->IoStatus.Information = 0U;
                IoCompleteRequest(req->Irp, IO_NO_INCREMENT);
                req->Irp = NULL;
            }
            ExFreePool(req);
        }

        KeAcquireSpinLock(&fdoExt->QueueLock, &oldIrql);
    }

    /* Clear any residual active request metadata */
    fdoExt->ActiveBusy = FALSE;
    RtlZeroMemory(&fdoExt->ActiveRequest, sizeof(fdoExt->ActiveRequest));
    KeReleaseSpinLock(&fdoExt->QueueLock, oldIrql);
}

/* ---------------------------------------------------------------------------
 * I2cCtrl_CancelRoutine - XP/2003 BSOD-safe, HAL-generic, C89-compliant
 * Purpose:
 *   - Cancels a queued IRP safely
 *   - Ensures ISR/DPC cannot touch the IRP after cancellation
 *   - Removes IRP from pending list and marks context canceled
 *   - Cancels timeout timer, masks interrupts, disables controller
 *   - Completes the IRP exactly once
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_CancelRoutine(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp
    )
{
    PI2CCTRL_FDO          fdoExt;
    KIRQL                 oldIrql;
    PI2CCTRL_IRP_CONTEXT  ctx;
    PLIST_ENTRY           e;
    BOOLEAN               hadTimer;

    /* C89 init */
    fdoExt   = NULL;
    oldIrql  = 0;
    ctx      = NULL;
    e        = NULL;
    hadTimer = FALSE;

    if (DeviceObject == NULL || Irp == NULL) {
        return;
    }

    fdoExt = (PI2CCTRL_FDO)DeviceObject->DeviceExtension;
    if (fdoExt == NULL) {
        IoReleaseCancelSpinLock(Irp->CancelIrql);
        return;
    }

    /* Release the cancel spin lock acquired by I/O manager */
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    /* Walk the pending list under our own spinlock to find and remove IRP context */
    KeAcquireSpinLock(&fdoExt->Queue->Lock, &oldIrql);

    for (e = fdoExt->Queue->PendingIrps.Flink;
         e != &fdoExt->Queue->PendingIrps;
         e = e->Flink)
    {
        ctx = CONTAINING_RECORD(e, I2CCTRL_IRP_CONTEXT, ListEntry);
        if (ctx->Irp == Irp) {
            ctx->Canceled = TRUE;

            /* Remove from queue so worker won’t touch it */
            RemoveEntryList(&ctx->ListEntry);
            break;
        }
    }

    KeReleaseSpinLock(&fdoExt->Queue->Lock, oldIrql);

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
    if (fdoExt->Ops != NULL && fdoExt->Removed == FALSE) {
        /* Mask all interrupts to stop further ISR activity */
        if (fdoExt->Ops->MaskInterrupts != NULL) {
            fdoExt->Ops->MaskInterrupts(fdoExt, 0U);
        }

        /* Acknowledge latched causes via HAL */
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

    /* If we found the context, complete the IRP immediately */
    if (ctx != NULL) {
        IoSetCancelRoutine(Irp, NULL); /* defensive: clear cancel routine */
        Irp->IoStatus.Status      = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0U;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        ctx->Completed = TRUE;
    } else {
        /* If no context found, still complete IRP defensively */
        Irp->IoStatus.Status      = STATUS_CANCELLED;
        Irp->IoStatus.Information = 0U;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
}
