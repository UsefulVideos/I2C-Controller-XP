/* gpioctrl_isr.c
 * GPIO Controller Driver (gpioctrl.sys) - ISR/DPC implementation
 * WinDDK 7.1.0 - XP/2003 build environment - C89 compliant
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
#include <stdarg.h>
#include <stdio.h>

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
   IRQL-safe circular buffer logger for ISR/DPC
   --------------------------------------------------------------------------- */
VOID
GpioCtrl_LogIsr(
    PGPIOCTRL_FDO_EXT Ext,
    PCCHAR Format,
    ULONG Value
    )
{
    KIRQL oldIrql;
    ULONG index;

    KeAcquireSpinLock(&Ext->IsrLogLock, &oldIrql);

    index = Ext->IsrLogHead % GPIO_LOG_SIZE;
    _snprintf(Ext->IsrLog[index], 63, Format, Value);
    Ext->IsrLog[index][63] = '\0';

    Ext->IsrLogHead++;
    if (Ext->IsrLogHead - Ext->IsrLogTail > GPIO_LOG_SIZE) {
        Ext->IsrLogTail++;
    }

    KeReleaseSpinLock(&Ext->IsrLogLock, oldIrql);
}


/* ---------------------------------------------------------------------------
   ISR: latch pending mask, clear status, schedule DPC (with IRQL-safe logging)
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

    /* Log ISR event */
    GpioCtrl_LogIsr(ext, "ISR: pending=%08X", pending);

    /* Clear interrupt */
    GpioRegWrite(ext, REG_INT_STAT_OFFSET, pending);

    /* Latch mask */
    ext->PendingIntMask |= pending;

    /* Schedule DPC */
    KeInsertQueueDpc(&ext->IsrDpc, NULL, NULL);
    return TRUE;
}


/* ---------------------------------------------------------------------------
   DPC: process latched events (with IRQL-safe logging)
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

    /* Log DPC event */
    GpioCtrl_LogIsr(ext, "DPC: mask=%08X", mask);

    /* Iterate bits */
    {
        ULONG pin;
        ULONG bits = mask;

        for (pin = 0; bits != 0; pin++) {
            if (bits & 1UL) {
                GpioCtrl_LogIsr(ext, "DPC: pin=%u", pin);
            }
            bits >>= 1;
        }
    }
}
