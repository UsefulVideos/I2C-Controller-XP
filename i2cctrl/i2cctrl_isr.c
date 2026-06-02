/* I2cCtrl_Isr.c - C89 compliant */

#include <ntddk.h>
#include "i2cctrl_isr.h"
#include "i2cctrl_ext.h"
#include "i2cctrl_hw.h"
#include "i2cctrl.h"
#include "i2cctrl_bsod.h"

CHAR  I2cCtrl_IsrLogBuffer[I2CCTRL_ISRLOG_SIZE];
ULONG I2cCtrl_IsrLogIndex = 0;
KSPIN_LOCK I2cCtrl_IsrLogLock;

/* ---------------------------------------------------------------------------
   Abort source -> NTSTATUS mapping (conservative, portable across DW_apb_i2c)
   --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_MapAbortSource(ULONG abrtSrc)
{
    /* 0: ABRT_7B_ADDR_NOACK (NACK on address) */
    if (abrtSrc & (1U << 0)) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    /* 1: ABRT_10B_ADDR_NOACK (NACK on 10-bit address) */
    if (abrtSrc & (1U << 1)) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    /* 2: ABRT_TXDATA_NOACK (NACK on data) */
    if (abrtSrc & (1U << 2)) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    /* 3: ABRT_LOST_ARBITRATION (arbitration lost) */
    if (abrtSrc & (1U << 3)) {
        return STATUS_IO_DEVICE_ERROR;
    }
    /* Master disabled mid-transfer (commonly bit 4 in some IPs) */
    if (abrtSrc & (1U << 4)) {
        return STATUS_CANCELLED;
    }
    /* Generic fallback */
    return STATUS_DEVICE_DATA_ERROR;
}

/* ---------------------------------------------------------------------------
   Helpers
   --------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * I2cCtrl_AckInterrupts - acknowledge per-source interrupt clears via HAL ops
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_AckInterrupts(
    PI2CCTRL_FDO dx,
    ULONG        intr
    )
{
    if (dx == NULL || dx->Ops == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_ISR,
                    "AckInterrupts called with NULL context or Ops");
        return;
    }

    /* TX abort: HAL may require reading source before clear */
    if ((intr & I2C_INT_TX_ABORT) != 0U) {
        if (dx->Ops->AckInterrupts != NULL) {
            dx->Ops->AckInterrupts(dx, I2C_INT_TX_ABORT);
        }
    }

    /* STOP detected: clear via HAL */
    if ((intr & I2C_INT_STOP_DETECTED) != 0U) {
        if (dx->Ops->AckInterrupts != NULL) {
            dx->Ops->AckInterrupts(dx, I2C_INT_STOP_DETECTED);
        }
    }

    /* Clear combined interrupt sources via HAL */
    if (dx->Ops->AckInterrupts != NULL) {
        dx->Ops->AckInterrupts(
            dx,
            I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
            I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
            I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
            I2C_INT_RD_REQ
        );
    }
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_IsArbitrationLost - detect arbitration loss via HAL ops
 * XP-BSOD-safe, HAL-generic, C89-compliant
 * --------------------------------------------------------------------------- */
BOOLEAN
I2cCtrl_IsArbitrationLost(
    PI2CCTRL_FDO dx,
    ULONG        intrSnapshot
    )
{
    I2C_HW_STATUS hwst;
    NTSTATUS      st;

    if (dx == NULL || dx->Ops == NULL) {
        return FALSE;
    }

    RtlZeroMemory(&hwst, sizeof(hwst));

    /* If HAL provides explicit arbitration detection, prefer that */
    if (dx->Ops->IsArbitrationLost) {
        return dx->Ops->IsArbitrationLost(dx, intrSnapshot);
    }

    /* Otherwise, use HAL status heuristics */
    if (dx->Ops->GetStatus) {
        st = dx->Ops->GetStatus(dx, &hwst);
        if (!NT_SUCCESS(st)) {
            return FALSE;
        }
    }

    /* If no TX abort, fallback heuristic: STOP_DET while controller active */
    if ((intrSnapshot & I2C_INT_TX_ABORT) == 0U) {
        if ((intrSnapshot & I2C_INT_STOP_DETECTED) != 0U) {
            if (hwst.ControllerActive) {
                return TRUE;
            }
        }
        return FALSE;
    }

    /* If TX abort present, heuristically treat STOP_DET as arbitration loss */
    if ((intrSnapshot & I2C_INT_STOP_DETECTED) != 0U) {
        return TRUE;
    }

    /* Fallback: rely on HAL status flags if available */
    if (hwst.TxAborted || hwst.ArbLost) {
        return TRUE;
    }

    return FALSE;
}


/* ---------------------------------------------------------------------------
   Handle arbitration loss: stats, clear sticky bits, optional controller re-enable
   HAL-based, XP-BSOD-hardened, WinDDK-compiler-safe, and C89-compliant.
   Safe to call from ISR, DPC, or polled worker (non-pageable).
   --------------------------------------------------------------------------- */
VOID
I2cCtrl_HandleArbitrationLost(
    IN PI2CCTRL_FDO           devctx,
    IN I2C_TRANSFER_CONTEXT*  Xc
    )
{
    BOOLEAN canTouchHw;
    ULONG   consecutive;
    LARGE_INTEGER now;

    /* Declare all locals at top (C89) */
    canTouchHw   = FALSE;
    consecutive  = 0U;
    now.QuadPart = 0;

    /* Defensive: validate device context before touching */
    if (devctx == NULL) {
        return;
    }

    /* Increment global error count safely */
    if (devctx->ErrorCount != 0xFFFFFFFFUL) {
        devctx->ErrorCount += 1U;
    }

    /* Update per-transfer stats if context provided */
    if (Xc != NULL) {
        Xc->ArbLostCount += 1U;
        Xc->ArbConsecutiveLost += 1U;

        /* KeQuerySystemTime is non-pageable and safe at any IRQL */
        KeQuerySystemTime(&now);
        Xc->LastArbLossTime = now;
    }

    /* Only touch hardware if device not removed/stopping */
    if (devctx->Removed == FALSE && devctx->Stopping == FALSE) {
        canTouchHw = TRUE;
    }

    /* Clear abort & stop bits to unblock the controller, if safe */
    if (canTouchHw != FALSE && devctx->Ops != NULL && devctx->Ops->AckInterrupts != NULL) {
        devctx->Ops->AckInterrupts(devctx, I2C_INT_TX_ABORT);
        devctx->Ops->AckInterrupts(devctx, I2C_INT_STOP_DETECTED);
    }

    /* Optional resilience: re-enable controller after repeated losses */
    if (Xc != NULL) {
        consecutive = Xc->ArbConsecutiveLost;
        if ((consecutive >= 3U) && (canTouchHw != FALSE) &&
            devctx->Ops != NULL && devctx->Ops->Enable != NULL) {

            /* Disable controller */
            (VOID)devctx->Ops->Enable(devctx, FALSE);

            /* Short stall to allow hardware to settle; safe at any IRQL */
            KeStallExecutionProcessor(10U);

            /* Re-enable controller */
            (VOID)devctx->Ops->Enable(devctx, TRUE);

            /* Reset consecutive counter to avoid repeated toggling */
            Xc->ArbConsecutiveLost = 0U;
        }
    }
}


BOOLEAN
I2cCtrl_Isr(
    PKINTERRUPT Interrupt,
    PVOID       ServiceContext
    )
{
    PI2CCTRL_FDO   dx;
    ULONG          intr;
    ULONG          masked;
    I2C_HW_STATUS  hwst;
    NTSTATUS       st;

    UNREFERENCED_PARAMETER(Interrupt);

    dx     = (PI2CCTRL_FDO)ServiceContext;
    intr   = 0U;
    masked = 0U;
    RtlZeroMemory(&hwst, sizeof(hwst));

    /* ISR must run at IRQL >= DISPATCH_LEVEL on XP/2003 */
    if (KeGetCurrentIrql() < DISPATCH_LEVEL) {
        I2cCtrl_LogIsr("ISR: wrong IRQL\n");
        return FALSE;
    }

    /* Defensive guards: device must be valid, started, mapped, and not removed/failed */
    if (dx == NULL ||
        dx->Ops == NULL ||
        dx->Removed ||
        !dx->Started ||
        dx->MmioBase == NULL ||
        dx->MmioLength == 0U) {
        I2cCtrl_LogIsr("ISR: invalid dx/state\n");
        return FALSE;
    }

    /* Read latched interrupt status via HAL (DIRQL-safe) */
    if (dx->Ops->GetRawIntr != NULL) {
        intr = dx->Ops->GetRawIntr(dx);
    } else if (dx->Ops->GetStatus != NULL) {
        st = dx->Ops->GetStatus(dx, &hwst);
        if (!NT_SUCCESS(st)) {
            I2cCtrl_LogIsr("ISR: GetStatus failed\n");
            return FALSE;
        }
        intr = hwst.RawIntr;
    } else {
        I2cCtrl_LogIsr("ISR: no GetRawIntr/GetStatus\n");
        return FALSE;
    }

    masked = intr & dx->IntrMask;

    /* Not our interrupt if none of our masked bits are set */
    if (masked == 0U) {
        return FALSE;
    }

    I2cCtrl_LogIsr("ISR: intr=0x%08lx masked=0x%08lx\n", intr, masked);

    /* Acknowledge quickly via HAL to deassert line and prevent storms */
    if (dx->Ops->AckInterrupts != NULL) {
        dx->Ops->AckInterrupts(dx, masked);
    } else {
        I2cCtrl_AckInterrupts(dx, masked);
    }

    /* Ensure ordering of MMIO writes before queuing DPC */
    KeMemoryBarrier();

    /* Lightweight accounting only; avoid pageable work at DIRQL */
    if ((masked & I2C_INT_TX_ABORT) != 0U) {
        if (dx->ErrorCount != 0xFFFFFFFFUL) {
            InterlockedIncrement((volatile LONG*)&dx->ErrorCount);
        }
        I2cCtrl_LogIsr("ISR: TX_ABORT\n");
    }

    /* Defer heavy work to DPC with snapshot of masked status bits */
    KeInsertQueueDpc(&dx->IsrDpc, (PVOID)(ULONG_PTR)masked, NULL);

    return TRUE;
}


VOID
I2cCtrl_DpcRoutine(
    PKDPC  Dpc,
    PVOID  DeferredContext,
    PVOID  SystemArg1,
    PVOID  SystemArg2
    )
{
    PI2CCTRL_FDO          dx;
    I2C_TRANSFER_CONTEXT* xc;
    ULONG                 intrSnapshot;
    ULONG                 maxIter;
    ULONG                 iter;
    PIRP                  irp;
    NTSTATUS              st;
    I2C_HW_STATUS         hwst;
    UCHAR                 byte;
    BOOLEAN               stopSeen;
    BOOLEAN               rxPath;
    BOOLEAN               txPath;
    ULONG                 ackMask;
    LONG                  alreadyCompleted;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArg2);

    /* DPC must run at DISPATCH_LEVEL on XP/2003 */
    if (KeGetCurrentIrql() != DISPATCH_LEVEL) {
        I2cCtrl_LogIsr("DPC: wrong IRQL\n");
        return;
    }

    dx = (PI2CCTRL_FDO)DeferredContext;
    if (dx == NULL ||
        dx->Ops == NULL ||
        dx->Removed ||
        !dx->Started ||
        dx->MmioBase == NULL ||
        dx->MmioLength == 0U) {
        I2cCtrl_LogIsr("DPC: invalid dx/state\n");
        return;
    }

    /* SystemArg1 is the masked interrupt snapshot from ISR */
    intrSnapshot = (ULONG)(ULONG_PTR)SystemArg1;
    if (intrSnapshot == 0U) {
        I2cCtrl_LogIsr("DPC: spurious or empty intrSnapshot\n");
        return;
    }

    I2cCtrl_LogIsr("DPC: intrSnapshot=0x%08lx\n", intrSnapshot);

    /* HID wakeup hook */
    if (dx->TouchpadPdo != NULL) {
        PI2CCTRL_PDO p = dx->TouchpadPdo;
        if (p->Started && !p->Removed) {
            KeSetEvent(&p->HidReportEvent, IO_NO_INCREMENT, FALSE);
            I2cCtrl_LogIsr("DPC: HID wake event signaled\n");
        }
    }

    maxIter      = 128U;
    iter         = 0U;
    irp          = NULL;
    st           = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));
    byte         = 0U;
    stopSeen     = FALSE;
    rxPath       = FALSE;
    txPath       = FALSE;
    ackMask      = 0U;
    alreadyCompleted = 0;

    xc = &dx->XferCtx;

    /* Defensive: ensure transfer context is valid */
    if (xc == NULL ||
        xc->Buffer == NULL ||
        xc->Length == 0U ||
        xc->BufferLen == 0U ||
        xc->Length > xc->BufferLen) {

        I2cCtrl_LogIsr("DPC: invalid transfer context\n");

        ackMask =
            I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
            I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
            I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
            I2C_INT_RD_REQ;
        if (dx->Ops->AckInterrupts != NULL) {
            dx->Ops->AckInterrupts(dx, ackMask);
        }
        return;
    }

    /* Arbitration lost */
    if (xc->MultiMasterEnabled && I2cCtrl_IsArbitrationLost(dx, intrSnapshot)) {
        I2cCtrl_LogIsr("DPC: arbitration lost\n");

        I2cCtrl_HandleArbitrationLost(dx, xc);

        if (xc->ArbAttempt < xc->ArbMaxRetries) {
            ULONG delayUs = (xc->ArbBackoffBaseUs == 0U) ? 10U : xc->ArbBackoffBaseUs;

            if (xc->ArbAttempt > 8U) {
                xc->ArbAttempt = 8U;
            }
            delayUs <<= xc->ArbAttempt;
            if (delayUs > 50U) {
                delayUs = 50U;
            }
            xc->ArbAttempt += 1U;

            I2cCtrl_LogIsr("DPC: backoff %lu us\n", delayUs);

            if (dx->Ops->MaskInterrupts != NULL) {
                dx->Ops->MaskInterrupts(dx, 0U);
            }
            KeStallExecutionProcessor(delayUs);
            if (dx->Ops->UnmaskInterrupts != NULL) {
                dx->Ops->UnmaskInterrupts(dx, dx->IntrMask);
            }

            return;
        } else {
            xc->Status = STATUS_IO_TIMEOUT;
            I2cCtrl_LogIsr("DPC: arbitration retries exhausted\n");
            goto CompleteXfer;
        }
    }

    /* TX_ABORT */
    if ((intrSnapshot & I2C_INT_TX_ABORT) != 0U) {
        I2cCtrl_LogIsr("DPC: TX_ABORT\n");

        if (dx->Ops->GetStatus != NULL) {
            st = dx->Ops->GetStatus(dx, &hwst);
            if (NT_SUCCESS(st)) {
                xc->Status = I2cCtrl_MapAbortSource(hwst.RawIntr);
            } else {
                xc->Status = STATUS_IO_DEVICE_ERROR;
            }
        } else {
            xc->Status = STATUS_IO_DEVICE_ERROR;
        }

        if (dx->Ops->AckInterrupts != NULL) {
            dx->Ops->AckInterrupts(dx, I2C_INT_TX_ABORT);
            if ((intrSnapshot & I2C_INT_STOP_DETECTED) != 0U) {
                dx->Ops->AckInterrupts(dx, I2C_INT_STOP_DETECTED);
            }
        }
        goto CompleteXfer;
    }

    /* Snapshot hardware status */
    if (dx->Ops->GetStatus != NULL) {
        st = dx->Ops->GetStatus(dx, &hwst);
        if (!NT_SUCCESS(st)) {
            xc->Status = st;
            I2cCtrl_LogIsr("DPC: GetStatus failed\n");
            goto CompleteXfer;
        }
    }

    rxPath = (xc->Direction == I2cDirRead);
    txPath = (xc->Direction == I2cDirWrite);

    /* RX path */
    if (rxPath) {
        I2cCtrl_LogIsr("DPC: RX path\n");
    }

    /* TX path */
    if (txPath) {
        I2cCtrl_LogIsr("DPC: TX path\n");
    }

    /* STOP detection */
    if (((intrSnapshot & I2C_INT_STOP_DETECTED) != 0U) || hwst.StopDetected) {
        stopSeen = TRUE;
        xc->StopSeen = TRUE;
        I2cCtrl_LogIsr("DPC: STOP detected\n");

        if (dx->Ops->AckInterrupts != NULL) {
            dx->Ops->AckInterrupts(dx, I2C_INT_STOP_DETECTED);
        }
    }

    /* Completion criteria */
    if (rxPath) {
        if ((xc->RxIndex >= xc->Length) || stopSeen) {
            xc->Status = STATUS_SUCCESS;
            I2cCtrl_LogIsr("DPC: RX complete (%lu/%lu)\n", xc->RxIndex, xc->Length);
            goto CompleteXfer;
        }
    } else if (txPath) {
        if ((xc->TxIndex >= xc->Length) || stopSeen) {
            xc->Status = STATUS_SUCCESS;
            I2cCtrl_LogIsr("DPC: TX complete (%lu/%lu)\n", xc->TxIndex, xc->Length);
            goto CompleteXfer;
        }
    }

    /* Not complete */
    I2cCtrl_LogIsr("DPC: not complete, re-enabling IRQs\n");

    if (dx->Ops->UnmaskInterrupts != NULL) {
        dx->Ops->UnmaskInterrupts(dx, dx->IntrMask);
    }
    ackMask =
        I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
        I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
        I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
        I2C_INT_RD_REQ;
    if (dx->Ops->AckInterrupts != NULL) {
        dx->Ops->AckInterrupts(dx, ackMask);
    }
    return;

CompleteXfer:
    I2cCtrl_LogIsr("DPC: completing transfer, status=0x%08lx\n", xc->Status);

    KeCancelTimer(&xc->TimeoutTimer);

    if (dx->Ops->MaskInterrupts != NULL) {
        dx->Ops->MaskInterrupts(dx, 0U);
    }
    if (dx->Ops->AckInterrupts != NULL) {
        dx->Ops->AckInterrupts(dx, intrSnapshot);
    }
    if (dx->Ops->GetStatus != NULL) {
        (VOID)dx->Ops->GetStatus(dx, &hwst);
    }

    xc->ArbAttempt          = 0U;
    xc->ArbConsecutiveLost  = 0U;

    alreadyCompleted = InterlockedCompareExchange(&xc->Completed, 1, 0);
    if (alreadyCompleted == 0) {
        irp = (PIRP)InterlockedExchangePointer((PVOID*)&xc->Irp, NULL);

        if (irp != NULL) {
            irp->IoStatus.Status = xc->Status;
            irp->IoStatus.Information =
                (xc->Direction == I2cDirRead) ? xc->RxIndex : xc->TxIndex;

            KeSetEvent(&dx->TransferEvent, IO_NO_INCREMENT, FALSE);
            IoCompleteRequest(irp, IO_NO_INCREMENT);

            I2cCtrl_LogIsr("DPC: IRP completed\n");
        }
    }

    if (dx->Started && !dx->Removed) {
        if (dx->Ops->UnmaskInterrupts != NULL) {
            dx->Ops->UnmaskInterrupts(dx, dx->IntrMask);
        }
    }
}


/* ---------------------------------------------------------------------------
 * Optional: polled-mode worker if no IRQ resource was provided
 * HAL-based, XP-safe, WinDDK-compiler-safe, and C89-compliant
 * Runs at PASSIVE_LEVEL (e.g., system thread or work item). No pageable calls at IRQL > PASSIVE.
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_PollWorker(
    IN PI2CCTRL_FDO devctx
    )
{
    I2C_TRANSFER_CONTEXT *xc;
    I2C_HW_STATUS         hwst;
    ULONG                 raw;
    BOOLEAN               isRead;
    BOOLEAN               isWrite;
    PIRP                  irp;
    ULONG                 base;
    ULONG                 jitter;
    ULONG                 cap;
    ULONG                 step;
    ULONG                 delay;
    ULONGLONG             ticks;
    ULONG                 j;
    NTSTATUS              st;
    UCHAR                 b;

    PAGED_CODE();

    xc     = NULL;
    raw    = 0U;
    isRead = FALSE;
    isWrite= FALSE;
    irp    = NULL;
    base   = 0U;
    jitter = 0U;
    cap    = 0U;
    step   = 0U;
    delay  = 0U;
    ticks  = 0ULL;
    j      = 0U;
    st     = STATUS_SUCCESS;
    b      = 0U;
    RtlZeroMemory(&hwst, sizeof(hwst));

    /* Defensive: validate device context and HAL ops before touching hardware */
    if (devctx == NULL || devctx->Ops == NULL) {
        I2cCtrl_Log("Poll: invalid devctx/Ops\n");
        return;
    }
    if (devctx->Removed || !devctx->Started) {
        I2cCtrl_Log("Poll: device not started or removed\n");
        return;
    }

    xc = &devctx->XferCtx;
    if (xc == NULL || xc->Irp == NULL || xc->Buffer == NULL || xc->Length == 0U) {
        I2cCtrl_Log("Poll: invalid transfer context\n");
        return;
    }

    isRead  = (xc->Direction == I2cDirRead)  ? TRUE : FALSE;
    isWrite = (xc->Direction == I2cDirWrite) ? TRUE : FALSE;

    /* Arbitration lost detection in polled mode (HAL-first, conservative) */
    if (devctx->Ops->GetRawIntr != NULL) {
        raw = devctx->Ops->GetRawIntr(devctx);
    } else if (devctx->Ops->GetStatus != NULL) {
        st = devctx->Ops->GetStatus(devctx, &hwst);
        raw = NT_SUCCESS(st) ? hwst.RawIntr : 0U;
    } else {
        raw = 0U;
    }

    if (xc->MultiMasterEnabled != FALSE &&
        (((raw & I2C_INT_TX_ABORT) != 0U) || ((raw & I2C_INT_STOP_DETECTED) != 0U))) {

        I2cCtrl_Log("Poll: arbitration indication, raw=0x%08lx\n", raw);

        /* Prefer HAL-provided arbitration check; fallback to generic helper */
        if (devctx->Ops->IsArbitrationLost != NULL) {
            if (devctx->Ops->IsArbitrationLost(devctx, raw) != FALSE) {
                I2cCtrl_HandleArbitrationLost(devctx, xc);
            }
        } else {
            if (I2cCtrl_IsArbitrationLost(devctx, raw) != FALSE) {
                I2cCtrl_HandleArbitrationLost(devctx, xc);
            }
        }

        if (xc->ArbAttempt < xc->ArbMaxRetries) {
            base   = xc->ArbBackoffBaseUs;
            jitter = xc->ArbBackoffJitterUs;
            cap    = xc->ArbBackoffMaxUs;

            step  = (xc->ArbAttempt > 8U) ? 8U : xc->ArbAttempt;
            delay = (base == 0U) ? 10U : (base << step);

            ticks = KeQueryInterruptTime();
            j     = (ULONG)(ticks % (jitter != 0U ? jitter : 1U));

            if (delay > cap && cap != 0U) {
                delay = cap;
            }

            if ((j & 1U) == 0U) {
                if (delay > j) {
                    delay -= j;
                }
            } else {
                delay += j;
                if (delay > cap && cap != 0U) {
                    delay = cap;
                }
            }

            xc->ArbAttempt += 1U;

            I2cCtrl_Log("Poll: arb backoff=%lu us, attempt=%lu\n", delay, xc->ArbAttempt);

            /* Busy-wait at PASSIVE_LEVEL for short backoff */
            KeStallExecutionProcessor(delay);

            /* Keep IRP attached for retry */
            return;
        } else {
            xc->Status = STATUS_IO_TIMEOUT;
            I2cCtrl_Log("Poll: arbitration retries exhausted\n");
            /* fall through to completion */
        }
    }

    /* Drain RX FIFO (HAL-first) */
    if (isRead != FALSE) {
        I2cCtrl_Log("Poll: RX path\n");
        while (xc->RxIndex < xc->Length) {
            if (devctx->Ops->GetStatus != NULL) {
                st = devctx->Ops->GetStatus(devctx, &hwst);
                if (!NT_SUCCESS(st)) {
                    xc->Status = st;
                    I2cCtrl_Log("Poll: GetStatus RX failed 0x%08lx\n", st);
                    break;
                }
                if (!hwst.RxFifoNotEmpty) {
                    break;
                }
            } else {
                /* Without GetStatus, we cannot safely poll generically */
                break;
            }

            if (devctx->Ops->ReadRxByte != NULL) {
                st = devctx->Ops->ReadRxByte(devctx, &b);
                if (!NT_SUCCESS(st)) {
                    xc->Status = st;
                    I2cCtrl_Log("Poll: ReadRxByte failed 0x%08lx\n", st);
                    break;
                }
                xc->Buffer[xc->RxIndex++] = b;
            } else if (devctx->Ops->DrainRxBounded != NULL) {
                devctx->Ops->DrainRxBounded(devctx);
                break;
            } else {
                /* No HAL method to read; abort safely */
                xc->Status = STATUS_NOT_SUPPORTED;
                I2cCtrl_Log("Poll: no RX HAL method\n");
                break;
            }
        }
    }

    /* Fill TX FIFO (HAL-first) */
    if (isWrite != FALSE) {
        I2cCtrl_Log("Poll: TX path\n");
        while (xc->TxIndex < xc->Length) {
            if (devctx->Ops->GetStatus != NULL) {
                st = devctx->Ops->GetStatus(devctx, &hwst);
                if (!NT_SUCCESS(st)) {
                    xc->Status = st;
                    I2cCtrl_Log("Poll: GetStatus TX failed 0x%08lx\n", st);
                    break;
                }
                if (!hwst.TxFifoNotFull) {
                    break;
                }
            } else {
                /* Without GetStatus, we cannot safely poll generically */
                break;
            }

            if (devctx->Ops->WriteTxByte != NULL) {
                st = devctx->Ops->WriteTxByte(devctx, xc->Buffer[xc->TxIndex]);
                if (!NT_SUCCESS(st)) {
                    xc->Status = st;
                    I2cCtrl_Log("Poll: WriteTxByte failed 0x%08lx\n", st);
                    break;
                }
            } else if (devctx->Ops->PrimeWrite != NULL) {
                ULONG pushed;
                pushed = 0U;
                st = devctx->Ops->PrimeWrite(devctx, &xc->Buffer[xc->TxIndex], 1U, &pushed);
                if (!NT_SUCCESS(st) || pushed == 0U) {
                    xc->Status = (NT_SUCCESS(st) ? STATUS_DEVICE_BUSY : st);
                    I2cCtrl_Log("Poll: PrimeWrite failed/pushed=0 st=0x%08lx\n", st);
                    break;
                }
            } else {
                /* No HAL method to write; abort safely */
                xc->Status = STATUS_NOT_SUPPORTED;
                I2cCtrl_Log("Poll: no TX HAL method\n");
                break;
            }
            xc->TxIndex += 1U;
        }
    }

    /* Check/clear STOP (complete condition) via HAL if available */
    if (devctx->Ops->GetRawIntr != NULL) {
        raw = devctx->Ops->GetRawIntr(devctx);
    } else if (devctx->Ops->GetStatus != NULL) {
        st = devctx->Ops->GetStatus(devctx, &hwst);
        raw = NT_SUCCESS(st) ? hwst.RawIntr : 0U;
    } else {
        raw = 0U;
    }

    if ((raw & I2C_INT_STOP_DETECTED) != 0U || hwst.StopDetected != FALSE) {
        xc->StopSeen = TRUE;
        I2cCtrl_Log("Poll: STOP detected, raw=0x%08lx\n", raw);

        if (devctx->Ops->AckInterrupts != NULL) {
            devctx->Ops->AckInterrupts(devctx, I2C_INT_STOP_DETECTED);
            devctx->Ops->AckInterrupts(devctx,
                I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
                I2C_INT_TX_ABORT  | I2C_INT_START_DETECTED | I2C_INT_GEN_CALL |
                I2C_INT_ACTIVITY | I2C_INT_RX_DONE   | I2C_INT_RD_REQ);
        }
    }

    /* Complete if done (read/write length reached or STOP seen) */
    if ((isRead  != FALSE && ((xc->RxIndex >= xc->Length) || (xc->StopSeen != FALSE))) ||
        (isWrite != FALSE && ((xc->TxIndex >= xc->Length) || (xc->StopSeen != FALSE)))) {

        xc->Status = (xc->Status == STATUS_SUCCESS || xc->Status == 0)
                        ? STATUS_SUCCESS
                        : xc->Status;

        I2cCtrl_Log("Poll: completing, dir=%s status=0x%08lx rx=%lu tx=%lu len=%lu\n",
                    (isRead ? "R" : "W"),
                    xc->Status,
                    xc->RxIndex,
                    xc->TxIndex,
                    xc->Length);

        /* Single completion path, atomically detach IRP */
        irp = (PIRP)InterlockedExchangePointer((PVOID*)&xc->Irp, NULL);
        if (irp != NULL) {
            /* Cancel timeout safely before completing */
            KeCancelTimer(&xc->TimeoutTimer);

            /* Mask interrupts defensively in polled mode (no IRQ expected) */
            if (devctx->Ops->MaskInterrupts != NULL) {
                devctx->Ops->MaskInterrupts(devctx, 0U);
            }

            /* Reset arbitration counters on completion */
            xc->ArbAttempt         = 0U;
            xc->ArbConsecutiveLost = 0U;

            /* Populate completion fields */
            irp->IoStatus.Status      = xc->Status;
            irp->IoStatus.Information =
                (isRead != FALSE) ? (ULONG_PTR)xc->RxIndex : (ULONG_PTR)xc->TxIndex;

            /* Notify any waiting thread/event; safe at PASSIVE_LEVEL */
            KeSetEvent(&devctx->TransferEvent, IO_NO_INCREMENT, FALSE);

            IoCompleteRequest(irp, IO_NO_INCREMENT);
        }
    }
}

/* ---------------------------------------------------------------------------
 * ISR runs at DIRQL: non-pageable, minimal, XP-BSOD-hardened, WinDDK-safe, C89-compliant
 * HAL-generic: masked-cause checks, spurious handling, specific acks, guarded DPC queuing.
 * --------------------------------------------------------------------------- */
BOOLEAN
I2cCtrl_InterruptService(
    IN PKINTERRUPT Interrupt,
    IN PVOID       Context
    )
{
    PI2CCTRL_FDO devctx;
    BOOLEAN      handled;
    BOOLEAN      queued;
    ULONG        raw;
    ULONG        mask;
    ULONG        causes;
    ULONG        rxEvents;
    ULONG        txEvents;
    ULONG        errEvents;
    ULONG        wakeEvents;
    ULONG        otherEvents;
    I2C_HW_STATUS hwst;
    NTSTATUS     st;

    UNREFERENCED_PARAMETER(Interrupt);

    devctx      = (PI2CCTRL_FDO)Context;
    handled     = FALSE;
    queued      = FALSE;
    raw         = 0U;
    mask        = 0U;
    causes      = 0U;
    rxEvents    = 0U;
    txEvents    = 0U;
    errEvents   = 0U;
    wakeEvents  = 0U;
    otherEvents = 0U;
    st          = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));

    /* Defensive gates */
    if (devctx == NULL || devctx->Ops == NULL) {
        I2cCtrl_LogIsr("ISR2: invalid devctx/Ops\n");
        return FALSE;
    }
    if (devctx->Removed || devctx->Stopping || !devctx->Started) {
        I2cCtrl_LogIsr("ISR2: device not active (Removed=%d Stopping=%d Started=%d)\n",
                       devctx->Removed, devctx->Stopping, devctx->Started);
        return FALSE;
    }

    /* If nothing enabled, treat as spurious */
    mask = devctx->IntrMask;
    if (mask == 0U) {
        I2cCtrl_LogIsr("ISR2: mask=0, spurious\n");
        return FALSE;
    }

    /* Read raw causes via HAL and intersect with our mask */
    if (devctx->Ops->GetRawIntr != NULL) {
        raw = devctx->Ops->GetRawIntr(devctx);
    } else if (devctx->Ops->GetStatus != NULL) {
        st = devctx->Ops->GetStatus(devctx, &hwst);
        if (!NT_SUCCESS(st)) {
            I2cCtrl_LogIsr("ISR2: GetStatus failed 0x%08lx\n", st);
            return FALSE;
        }
        raw = hwst.RawIntr;
    } else {
        I2cCtrl_LogIsr("ISR2: no GetRawIntr/GetStatus\n");
        return FALSE;
    }

    causes = raw & mask;
    if (causes == 0U) {
        /* Spurious interrupt (masked out or noise) */
        I2cCtrl_LogIsr("ISR2: spurious, raw=0x%08lx mask=0x%08lx\n", raw, mask);
        return FALSE;
    }

    I2cCtrl_LogIsr("ISR2: raw=0x%08lx mask=0x%08lx causes=0x%08lx\n", raw, mask, causes);

    /* Classify causes for fast-path acks */
    rxEvents    = causes & (I2C_INT_RX_FULL | I2C_INT_RX_OVER | I2C_INT_RX_UNDER);
    txEvents    = causes & (I2C_INT_TX_EMPTY | I2C_INT_TX_OVER | I2C_INT_TX_ABORT);
    errEvents   = causes & (I2C_INT_TX_ABORT | I2C_INT_RX_OVER | I2C_INT_RX_UNDER | I2C_INT_TX_OVER);
    wakeEvents  = causes & I2C_INT_STOP_DETECTED;
    otherEvents = causes & (I2C_INT_START_DETECTED | I2C_INT_RD_REQ | I2C_INT_ACTIVITY);

    /* Acknowledge specific latched causes via HAL to avoid storms */
    if ((errEvents & I2C_INT_TX_ABORT) != 0U) {
        if (devctx->Ops->AckInterrupts != NULL) {
            devctx->Ops->AckInterrupts(devctx, I2C_INT_TX_ABORT);
        }
        if (devctx->ErrorCount != 0xFFFFFFFFUL) {
            devctx->ErrorCount += 1U;
        }
        I2cCtrl_LogIsr("ISR2: TX_ABORT\n");
    }
    if ((rxEvents & I2C_INT_RX_OVER) != 0U) {
        if (devctx->Ops->AckInterrupts != NULL) {
            devctx->Ops->AckInterrupts(devctx, I2C_INT_RX_OVER);
        }
        if (devctx->ErrorCount != 0xFFFFFFFFUL) {
            devctx->ErrorCount += 1U;
        }
        I2cCtrl_LogIsr("ISR2: RX_OVER\n");
    }
    if ((rxEvents & I2C_INT_RX_UNDER) != 0U) {
        if (devctx->Ops->AckInterrupts != NULL) {
            devctx->Ops->AckInterrupts(devctx, I2C_INT_RX_UNDER);
        }
        if (devctx->ErrorCount != 0xFFFFFFFFUL) {
            devctx->ErrorCount += 1U;
        }
        I2cCtrl_LogIsr("ISR2: RX_UNDER\n");
    }
    if ((txEvents & I2C_INT_TX_OVER) != 0U) {
        if (devctx->Ops->AckInterrupts != NULL) {
            devctx->Ops->AckInterrupts(devctx, I2C_INT_TX_OVER);
        }
        if (devctx->ErrorCount != 0xFFFFFFFFUL) {
            devctx->ErrorCount += 1U;
        }
        I2cCtrl_LogIsr("ISR2: TX_OVER\n");
    }
    if ((otherEvents & I2C_INT_START_DETECTED) != 0U) {
        if (devctx->Ops->AckInterrupts != NULL) {
            devctx->Ops->AckInterrupts(devctx, I2C_INT_START_DETECTED);
        }
        I2cCtrl_LogIsr("ISR2: START_DETECTED\n");
    }
    if ((wakeEvents & I2C_INT_STOP_DETECTED) != 0U) {
        /* STOP_DET may be used for normal completion and wake */
        if (devctx->Ops->AckInterrupts != NULL) {
            devctx->Ops->AckInterrupts(devctx, I2C_INT_STOP_DETECTED);
        }
        I2cCtrl_LogIsr("ISR2: STOP_DETECTED\n");
    }

    /* Defensively acknowledge remaining latched causes to reduce storms */
    if (devctx->Ops->AckInterrupts != NULL) {
        devctx->Ops->AckInterrupts(
            devctx,
            I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
            I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
            I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
            I2C_INT_RD_REQ
        );
    }

    /* Queue DPC for heavy lifting (FIFO drain/fill, completion, retries) */
    if (devctx->IsrDpc.DeferredRoutine != NULL) {
        queued = KeInsertQueueDpc(&devctx->IsrDpc, (PVOID)(ULONG_PTR)causes, NULL);
        UNREFERENCED_PARAMETER(queued);
        I2cCtrl_LogIsr("ISR2: DPC queued, causes=0x%08lx\n", causes);
        handled = TRUE;
    } else {
        I2cCtrl_LogIsr("ISR2: no DPC routine, handled in ISR only\n");
        handled = TRUE;
    }

    return handled;
}

/* ---------------------------------------------------------------------------
 * ISR-chained DPC: completes wake if armed, performs bounded deferred work,
 * and avoids pageable operations. XP-BSOD-safe, HAL-generic, C89-compliant.
 * --------------------------------------------------------------------------- */
VOID
I2cCtrl_IsrDpc(
    PKDPC  Dpc,
    PVOID  DeferredContext,
    PVOID  SystemArg1,
    PVOID  SystemArg2
    )
{
    PI2CCTRL_FDO          devctx;
    I2C_TRANSFER_CONTEXT* xc;
    KIRQL                 irql;
    BOOLEAN               requeue;
    ULONG                 iter;
    ULONG                 maxIter;
    I2C_HW_STATUS         hwst;
    NTSTATUS              st;
    UCHAR                 byte;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArg1);
    UNREFERENCED_PARAMETER(SystemArg2);

    irql   = KeGetCurrentIrql();
    devctx = (PI2CCTRL_FDO)DeferredContext;

    /* DPC must run at DISPATCH_LEVEL */
    ASSERT(irql == DISPATCH_LEVEL);

    if (devctx == NULL || devctx->Ops == NULL) {
        return;
    }

    /* Device/mapping guards: no work if device is quiescing */
    if (devctx->Removed || devctx->Stopping || !devctx->Started) {
        return;
    }

    /* Complete wake IRP if armed (implementation must be non-pageable) */
    if (devctx->WakeArmed != FALSE) {
        I2cCtrl_CompleteWakeIfArmed(devctx);
    }

    /* Bounded, defensive deferred work: drain minimal residuals and nudge FIFOs.
       Keep this short to avoid DPC starvation; heavier work is done in QueueDpcRoutine. */
    xc = &devctx->XferCtx;
    requeue = FALSE;
    maxIter = 32U; /* conservative cap */
    iter    = 0U;
    st      = STATUS_SUCCESS;
    byte    = 0U;
    RtlZeroMemory(&hwst, sizeof(hwst));

    /* Drain sticky STOP_DET if latched and defensively clear any remaining causes */
    if (devctx->Ops->GetStatus != NULL) {
        st = devctx->Ops->GetStatus(devctx, &hwst);
        if (NT_SUCCESS(st) && hwst.StopDetected) {
            if (devctx->Ops->AckInterrupts != NULL) {
                devctx->Ops->AckInterrupts(devctx, I2C_INT_STOP_DETECTED);
                devctx->Ops->AckInterrupts(
                    devctx,
                    I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
                    I2C_INT_TX_ABORT  | I2C_INT_START_DETECTED | I2C_INT_GEN_CALL |
                    I2C_INT_ACTIVITY | I2C_INT_RX_DONE   | I2C_INT_RD_REQ
                );
            }
        }
    }

    /* If a transfer is in flight, do minimal progress and requeue detailed DPC */
    if (xc->Irp != NULL) {
        if (xc->IsRead != FALSE) {
            while (iter < maxIter && xc->RxIndex < xc->BufferLen) {
                if (devctx->Ops->GetStatus != NULL) {
                    st = devctx->Ops->GetStatus(devctx, &hwst);
                    if (!NT_SUCCESS(st) || !hwst.RxFifoNotEmpty) {
                        break;
                    }
                }
                if (xc->Buffer == NULL) {
                    xc->Status = STATUS_INVALID_PARAMETER;
                    break;
                }
                if (devctx->Ops->ReadRxByte != NULL) {
                    st = devctx->Ops->ReadRxByte(devctx, &byte);
                    if (!NT_SUCCESS(st)) {
                        xc->Status = st;
                        break;
                    }
                    xc->Buffer[xc->RxIndex++] = byte;
                } else if (devctx->Ops->DrainRxBounded != NULL) {
                    devctx->Ops->DrainRxBounded(devctx);
                    break;
                }
                iter++;
            }
        } else {
            while (iter < maxIter && xc->TxIndex < xc->BufferLen) {
                if (devctx->Ops->GetStatus != NULL) {
                    st = devctx->Ops->GetStatus(devctx, &hwst);
                    if (!NT_SUCCESS(st) || !hwst.TxFifoNotFull) {
                        break;
                    }
                }
                if (xc->Buffer == NULL) {
                    xc->Status = STATUS_INVALID_PARAMETER;
                    break;
                }
                if (devctx->Ops->WriteTxByte != NULL) {
                    st = devctx->Ops->WriteTxByte(devctx, xc->Buffer[xc->TxIndex]);
                    if (!NT_SUCCESS(st)) {
                        xc->Status = st;
                        break;
                    }
                    xc->TxIndex++;
                } else if (devctx->Ops->FlushTxBounded != NULL) {
                    devctx->Ops->FlushTxBounded(devctx);
                    break;
                }
                iter++;
            }
        }

        /* Ask the queued DPC to handle full completion logic and re-enabling policy */
        requeue = TRUE;
    }

    if (requeue != FALSE) {
        KeInsertQueueDpc(&devctx->QueueDpc, NULL, NULL);
    }
}
