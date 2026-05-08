/* -----------------------------------------------------------------------
   uartctrl_ext.c – helpers for UARTCTRL_DEVEXT lifecycle and utilities
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl_ext.h"
#include "uartctrl_hw.h"

/* -----------------------------------------------------------------------
   Initialize the device extension fields
   ----------------------------------------------------------------------- */
VOID
UARTCTRL_ExtInitialize(PUARTCTRL_DEVEXT ext)
{
    RtlZeroMemory(ext, sizeof(UARTCTRL_DEVEXT));

    IoInitializeRemoveLock(&ext->RemoveLock, 'traU', 0, 0);
    InitializeListHead(&ext->ReadQueue);
    KeInitializeSpinLock(&ext->ReadQueueLock);
    KeInitializeSpinLock(&ext->RxLock);
    KeInitializeSpinLock(&ext->TxLock);

    ext->Started = FALSE;
    ext->Removed = FALSE;
    ext->InterruptConnected = FALSE;
    ext->RxErrors = 0;
    ext->TxErrors = 0;

    ext->RxBuf = NULL;
    ext->TxBuf = NULL;
    ext->RxSize = 0;
    ext->TxSize = 0;
    ext->RxHead = ext->RxTail = 0;
    ext->TxHead = ext->TxTail = 0;
    ext->Polling = FALSE;
}

/* -----------------------------------------------------------------------
   Allocate RX/TX ring buffers
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_ExtAllocateBuffers(PUARTCTRL_DEVEXT ext, ULONG rxSize, ULONG txSize)
{
    ext->RxBuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, rxSize, 'RxUA');
    ext->TxBuf = (PUCHAR)ExAllocatePoolWithTag(NonPagedPoolNx, txSize, 'TxUA');

    if (!ext->RxBuf || !ext->TxBuf) {
        if (ext->RxBuf) {
            ExFreePoolWithTag(ext->RxBuf, 'RxUA');
            ext->RxBuf = NULL;
        }
        if (ext->TxBuf) {
            ExFreePoolWithTag(ext->TxBuf, 'TxUA');
            ext->TxBuf = NULL;
        }
        ext->RxSize = ext->TxSize = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ext->RxSize = rxSize;
    ext->TxSize = txSize;
    ext->RxHead = ext->RxTail = 0;
    ext->TxHead = ext->TxTail = 0;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
   Free RX/TX ring buffers
   ----------------------------------------------------------------------- */
VOID
UARTCTRL_ExtFreeBuffers(PUARTCTRL_DEVEXT ext)
{
    if (ext->RxBuf) {
        ExFreePoolWithTag(ext->RxBuf, 'RxUA');
        ext->RxBuf = NULL;
    }
    if (ext->TxBuf) {
        ExFreePoolWithTag(ext->TxBuf, 'TxUA');
        ext->TxBuf = NULL;
    }

    ext->RxSize = ext->TxSize = 0;
    ext->RxHead = ext->RxTail = 0;
    ext->TxHead = ext->TxTail = 0;
}

/* -----------------------------------------------------------------------
   Reset UART hardware state
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_ExtResetHardware(PUARTCTRL_DEVEXT ext)
{
    // Reset FIFOs, clear errors, disable interrupts
    UartDisableInterrupts(ext);
    UartEnableFifo(ext, FCR_TRIG_1);

    ext->RxErrors = 0;
    ext->TxErrors = 0;

    return STATUS_SUCCESS;
}
