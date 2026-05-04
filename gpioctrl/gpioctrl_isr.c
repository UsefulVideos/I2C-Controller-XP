/* gpioctrl_isr.c
 * GPIO Controller Driver (gpioctrl.sys) – ISR/DPC implementation
 * WinDDK 7.1.0 – XP/2003 build environment – C89 compliant
 *
 * Implements:
 *  - GpioCtrl_Isr: top-half interrupt service routine
 *  - GpioCtrl_Dpc: bottom-half deferred procedure call
 *
 * Behavior:
 *  - ISR reads INT_STAT (pending bits), clears them (write-1-to-clear),
 *    latches the mask into Ext->PendingIntMask, and queues a DPC.
 *  - DPC drains Ext->PendingIntMask and is the place to complete any
 *    queued client IRPs or signal events. Template logic provided.
 *
 * Notes:
 *  - Synchronization uses Ext->RegLock; IoConnectInterrupt passes this
 *    lock for ISR-level coordination on XP/2003 WDM.
 *  - No allocations or blocking in ISR/DPC.
 */

#include <ntddk.h>
#include "gpioctrl_ext.h"

/* ---------------------------------------------------------------------------
   Optional: simple helper to apply basic debounce by re-checking the pin.
   Uses a very small stall to avoid long ISR delays. Keep usage conservative.
   --------------------------------------------------------------------------- */
static __inline ULONG
GpioIsr_RecheckPending(
    IN PGPIOCTRL_FDO_EXT Ext,
    IN ULONG PendingMask
    )
{
    ULONG mask2;

    /* Tiny re-check; avoid millisecond delays in ISR/DPC paths. */
    /* KeStallExecutionProcessor stalls in microseconds; use sparingly. */
    if (Ext->SupportsDebounce && Ext->DebounceDefaultMs != 0) {
        /* Convert a minimal fraction (e.g., 100 microseconds) */
        KeStallExecutionProcessor(100);
        mask2 = GpioRegRead(Ext, REG_INT_STAT_OFFSET);
        /* Keep only bits that remain asserted */
        PendingMask &= mask2;
    }

    return PendingMask;
}

/* ---------------------------------------------------------------------------
   ISR: latch pending mask, clear status, schedule DPC
   --------------------------------------------------------------------------- */
BOOLEAN
GpioCtrl_Isr(
    PKINTERRUPT Interrupt,
    PVOID ServiceContext
    )
{
    PGPIOCTRL_FDO_EXT ext;
    ULONG pending;

    UNREFERENCED_PARAMETER(Interrupt);
    ext = (PGPIOCTRL_FDO_EXT)ServiceContext;

    if (!ext->Started || ext->MmioBase == NULL) {
        return FALSE;
    }

    pending = GpioRegRead(ext, REG_INT_STAT_OFFSET);
    if (pending == 0) {
        return FALSE;
    }

    GpioRegWrite(ext, REG_INT_STAT_OFFSET, pending);
    ext->PendingIntMask |= pending;

    KeInsertQueueDpc(&ext->IsrDpc, NULL, NULL);
    return TRUE;
}


/* ---------------------------------------------------------------------------
   DPC: process latched events (template – extend to complete queued IRPs)
   --------------------------------------------------------------------------- */
VOID
GpioCtrl_Dpc(
    IN PKDPC  Dpc,
    IN PVOID  DeferredContext,
    IN PVOID  Arg1,
    IN PVOID  Arg2
    )
{
    PGPIOCTRL_FDO_EXT ext;
    KIRQL oldIrql;
    ULONG mask;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    ext = (PGPIOCTRL_FDO_EXT)DeferredContext;

    /* Drain latched mask atomically */
    KeAcquireSpinLock(&ext->RegLock, &oldIrql);
    mask = ext->PendingIntMask;
    ext->PendingIntMask = 0;
    KeReleaseSpinLock(&ext->RegLock, oldIrql);

    if (mask == 0) {
        return;
    }

    /* Template: iterate set bits and act per-pin.
       In a full implementation, this is where you would:
       - Complete queued event IRPs for each pin.
       - Signal per-pin KEVENT or notify a client via IOCTL wait.
       - Optionally re-enable pin interrupts after handling (already enabled by hardware).
    */
    {
        ULONG pin;
        ULONG bits;
        bits = mask;

        for (pin = 0; bits != 0; pin++) {
            if (bits & 1UL) {
                /* Handle pin 'pin' event: placeholder for client notification. */
                /* Example: write to a circular buffer, set an event, etc. */
            }
            bits >>= 1;
        }
    }
}
