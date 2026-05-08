/* -----------------------------------------------------------------------
   uartctrl_isr.c – ISR and DPC for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl.h"
#include "uartctrl_ext.h"
#include "uartctrl_hw.h"

/* -----------------------------------------------------------------------
   UARTCTRL_InterruptServiceRoutine – acknowledge UART interrupts
   ----------------------------------------------------------------------- */
BOOLEAN
UARTCTRL_InterruptServiceRoutine(PKINTERRUPT Interrupt, PVOID ServiceContext)
{
    PUARTCTRL_DEVEXT ext;
    UCHAR iir, lsr;
    BOOLEAN handled = FALSE;

    UNREFERENCED_PARAMETER(Interrupt);

    ext = (PUARTCTRL_DEVEXT)ServiceContext;

    /* Read Interrupt Identification Register */
    iir = UartRead8(ext, UART_IIR);

    if (iir & IIR_INT_PENDING) {
        return FALSE; // no interrupt pending
    }

    handled = TRUE;

    switch (iir & IIR_ID_MASK) {
    case IIR_RDA:   // Received Data Available
    case IIR_CTI:   // Character Timeout Indication
        lsr = UartReadLineStatus(ext);
        while (lsr & LSR_DR) {
            UCHAR byte = UartReadByte(ext);
            if (!RingPut(ext->RxBuf, ext->RxSize, &ext->RxHead, &ext->RxTail, byte)) {
                ext->RxErrors++;
            }
            lsr = UartReadLineStatus(ext);
        }
        break;

    case IIR_THRE:  // Transmitter Holding Register Empty
        // Don’t drain TX here; just schedule DPC to handle it
        break;

    case IIR_RLS:   // Receiver Line Status
        lsr = UartReadLineStatus(ext);
        if (lsr & (LSR_OE | LSR_PE | LSR_FE | LSR_BI)) {
            ext->RxErrors++;
        }
        break;

    default:
        break;
    }

    /* Always queue DPC for deferred work (completing IRPs, draining TX) */
    KeInsertQueueDpc(&ext->PollDpc, NULL, NULL);

    return handled;
}


/* -----------------------------------------------------------------------
   UARTCTRL_DpcRoutine – handles RX/TX and completes queued IRPs
   ----------------------------------------------------------------------- */
VOID
UARTCTRL_DpcRoutine(PKDPC Dpc,
                    PVOID DeferredContext,
                    PVOID SystemArg1,
                    PVOID SystemArg2)
{
    PUARTCTRL_DEVEXT ext;
    UCHAR lsr;
    KIRQL qirql, rxirql, txirql;
    ULONG completedCount = 0;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArg1);
    UNREFERENCED_PARAMETER(SystemArg2);

    ext = (PUARTCTRL_DEVEXT)DeferredContext;

    /* ----------------------------
       1) Drain RX into ring buffer
       ---------------------------- */
    lsr = UartReadLineStatus(ext);
    while (lsr & LSR_DR) {
        UCHAR byte = UartReadByte(ext);
        if (!RingPut(ext->RxBuf, ext->RxSize, &ext->RxHead, &ext->RxTail, byte)) {
            ext->RxErrors++;   // increment error counter instead of bit‑OR
        }
        lsr = UartReadLineStatus(ext);
    }

    /* ----------------------------
       2) Complete queued READ IRPs
       ---------------------------- */
    KeAcquireSpinLock(&ext->ReadQueueLock, &qirql);
    while (!IsListEmpty(&ext->ReadQueue)) {
        PLIST_ENTRY le = RemoveHeadList(&ext->ReadQueue);
        PIRP irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(irp);
        PUCHAR buf;
        ULONG outLen, copied = 0;

        /* Map buffer safely */
        if (irp->MdlAddress) {
            buf = (PUCHAR)MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
            outLen = isl->Parameters.Read.Length;
        } else {
            buf = (PUCHAR)irp->AssociatedIrp.SystemBuffer;
            outLen = isl->Parameters.Read.Length;
        }

        if (!buf) {
            irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            irp->IoStatus.Information = 0;
            KeReleaseSpinLock(&ext->ReadQueueLock, qirql);
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            KeAcquireSpinLock(&ext->ReadQueueLock, &qirql);
            continue;
        }

        /* Copy from RX ring */
        KeAcquireSpinLock(&ext->RxLock, &rxirql);
        while (copied < outLen) {
            UCHAR v;
            if (!RingGet(ext->RxBuf, ext->RxSize, &ext->RxHead, &ext->RxTail, &v)) {
                break;
            }
            buf[copied++] = v;
        }
        KeReleaseSpinLock(&ext->RxLock, rxirql);

        if (copied > 0) {
            irp->IoStatus.Status = STATUS_SUCCESS;
            irp->IoStatus.Information = copied;
        } else {
            irp->IoStatus.Status = STATUS_NO_DATA_DETECTED;
            irp->IoStatus.Information = 0;
        }

        KeReleaseSpinLock(&ext->ReadQueueLock, qirql);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        KeAcquireSpinLock(&ext->ReadQueueLock, &qirql);

        if (++completedCount >= 64) {
            /* Bound completions per DPC to avoid monopolizing CPU */
            break;
        }
    }
    KeReleaseSpinLock(&ext->ReadQueueLock, qirql);

    /* ----------------------------
       3) Drain TX ring if idle
       ---------------------------- */
    lsr = UartReadLineStatus(ext);
    if (lsr & LSR_THRE) {
        UCHAR v;
        KeAcquireSpinLock(&ext->TxLock, &txirql);
        while (RingGet(ext->TxBuf, ext->TxSize, &ext->TxHead, &ext->TxTail, &v)) {
            UartWriteByte(ext, v);
            if (!(UartReadLineStatus(ext) & LSR_THRE)) {
                break;
            }
        }
        KeReleaseSpinLock(&ext->TxLock, txirql);
    }
}
