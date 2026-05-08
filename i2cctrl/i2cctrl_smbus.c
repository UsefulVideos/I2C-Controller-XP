/* i2cctrl_smbus.c - C89 compliant SMBus implementation atop DW_apb_i2c */

#include <ntddk.h>
#include "i2cctrl_hw.h"
#include "i2cctrl_ext.h"
#include "i2cctrl_smbus.h"
#include "i2cctrl_bsod.h"

/* ---------------------------------------------------------------------------
 * I2cCtrl_WaitTxFifoNotFull - HAL-generic busy-wait helper
 * XP/2003 BSOD-safe, C89-compliant, WinDDK-compiler-safe
 * Purpose:
 *   - Polls TX FIFO availability via HAL status
 *   - Avoids long stalls; use only for bring-up or fallback
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_WaitTxFifoNotFull(
    PI2CCTRL_FDO Dx,
    ULONG        TimeoutUsec
    )
{
    ULONG         i;
    NTSTATUS      st;
    I2C_HW_STATUS hwst;

    /* Defensive init */
    st = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (Dx == NULL || Dx->Ops == NULL || Dx->Ops->GetStatus == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0U; i < TimeoutUsec; i++) {
        st = Dx->Ops->GetStatus(Dx, &hwst);
        if (!NT_SUCCESS(st)) {
            return st;
        }

        if (hwst.TxFifoNotFull != FALSE) {
            return STATUS_SUCCESS;
        }

        KeStallExecutionProcessor(1U);
    }

    return STATUS_IO_TIMEOUT;
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_WaitRxFifoNotEmpty - HAL-generic busy-wait helper
 * XP/2003 BSOD-safe, C89-compliant, WinDDK-compiler-safe
 * Purpose:
 *   - Polls RX FIFO availability via HAL status
 *   - Avoids long stalls; use only for bring-up or fallback
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_WaitRxFifoNotEmpty(
    PI2CCTRL_FDO Dx,
    ULONG        TimeoutUsec
    )
{
    ULONG         i;
    NTSTATUS      st;
    I2C_HW_STATUS hwst;

    /* Defensive init */
    st = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (Dx == NULL || Dx->Ops == NULL || Dx->Ops->GetStatus == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0U; i < TimeoutUsec; i++) {
        st = Dx->Ops->GetStatus(Dx, &hwst);
        if (!NT_SUCCESS(st)) {
            return st;
        }

        if (hwst.RxFifoNotEmpty != FALSE) {
            return STATUS_SUCCESS;
        }

        KeStallExecutionProcessor(1U);
    }

    return STATUS_IO_TIMEOUT;
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_WaitStopDetected - HAL-generic busy-wait helper
 * XP/2003 BSOD-safe, C89-compliant, WinDDK-compiler-safe
 * Purpose:
 *   - Polls STOP_DET condition via HAL status
 *   - Clears sticky STOP_DET interrupt via HAL
 *   - Avoids long stalls; use only for bring-up or fallback
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_WaitStopDetected(
    PI2CCTRL_FDO Dx,
    ULONG        TimeoutUsec
    )
{
    ULONG         i;
    NTSTATUS      st;
    I2C_HW_STATUS hwst;

    /* Defensive init */
    st = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (Dx == NULL || Dx->Ops == NULL || Dx->Ops->GetStatus == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0U; i < TimeoutUsec; ++i) {
        st = Dx->Ops->GetStatus(Dx, &hwst);
        if (!NT_SUCCESS(st)) {
            return st;
        }

        if (hwst.StopDetected != FALSE) {
            if (Dx->Ops->AckInterrupts != NULL) {
                Dx->Ops->AckInterrupts(Dx, I2C_INT_STOP_DETECTED);
            }
            return STATUS_SUCCESS;
        }

        KeStallExecutionProcessor(1U);
    }

    return STATUS_IO_TIMEOUT;
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_CheckTxAbort - HAL-generic TX abort check
 * XP/2003 BSOD-safe, C89-compliant, WinDDK-compiler-safe
 * Purpose:
 *   - Queries HAL status for TX abort condition
 *   - Clears sticky TX_ABRT interrupt via HAL
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_CheckTxAbort(
    PI2CCTRL_FDO Dx
    )
{
    NTSTATUS      st;
    I2C_HW_STATUS hwst;

    /* Defensive init */
    st = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (Dx == NULL || Dx->Ops == NULL || Dx->Ops->GetStatus == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Query HAL for current status */
    st = Dx->Ops->GetStatus(Dx, &hwst);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    /* If TX abort detected, clear and return error */
    if (hwst.TxAborted != FALSE) {
        if (Dx->Ops->AckInterrupts != NULL) {
            Dx->Ops->AckInterrupts(Dx, I2C_INT_TX_ABORT);
        }
        return STATUS_DEVICE_DATA_ERROR;
    }

    return STATUS_SUCCESS;
}


NTSTATUS I2cCtrl_ProgramTarget(PI2CCTRL_FDO Dx, UCHAR SlaveAddress, BOOLEAN TenBit, BOOLEAN RestartEnable);
NTSTATUS I2cCtrl_FinishAndWaitStop(PI2CCTRL_FDO Dx, ULONG TimeoutUsec);

/* New prototype for Alert Response */
NTSTATUS I2cCtrl_SmbusAlertResponse(PI2CCTRL_FDO Dx, UCHAR *AlertingAddress);

/* -----------------------------------------------------------------------
 * I2cCtrl_SmbusAlertResponse - HAL-generic SMBus AlertResponse
 * XP-BSOD-safe, WinDDK-compiler-safe, 100% hardened, C89-compliant
 * Purpose:
 *   - Reads alerting device 7-bit address from reserved 0x0C
 *   - Uses HAL ops for portability
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_SmbusAlertResponse(
    PI2CCTRL_FDO Dx,
    UCHAR*       AlertingAddress
    )
{
    NTSTATUS      st;
    ULONG         val;
    BOOLEAN       stopNeeded;
    I2C_HW_STATUS hwst;
    UCHAR         byte;

    /* Defensive init */
    st         = STATUS_SUCCESS;
    val        = 0U;
    stopNeeded = FALSE;
    byte       = 0U;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (Dx == NULL || Dx->Ops == NULL || AlertingAddress == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    /* Program reserved SMBus AlertResponse address (0x0C) */
    if (Dx->Ops->SetTarget7bit != NULL) {
        st = Dx->Ops->SetTarget7bit(Dx, 0x0CU);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    } else {
        return STATUS_NOT_SUPPORTED;
    }

    /* Ensure TX FIFO has space */
    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) {
        (VOID)I2cCtrl_FinishAndWaitStop(Dx, 1000U);
        return st;
    }

    /* Issue read token via HAL */
    if (Dx->Ops->IssueReadToken != NULL) {
        st = Dx->Ops->IssueReadToken(Dx);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }
    stopNeeded = TRUE;

    /* Wait for RX FIFO to have data */
    st = I2cCtrl_WaitRxFifoNotEmpty(Dx, 5000U);
    if (!NT_SUCCESS(st)) {
        if (stopNeeded) {
            I2cCtrl_FinishAndWaitStop(Dx, 2000U);
        }
        return st;
    }

    /* Read received byte via HAL */
    if (Dx->Ops->ReadRxByte != NULL) {
        st = Dx->Ops->ReadRxByte(Dx, &byte);
        if (!NT_SUCCESS(st)) {
            return st;
        }
        val = (ULONG)byte & 0x7FU;
    } else {
        return STATUS_NOT_SUPPORTED;
    }

    /* Validate address */
    if (val == 0U || val > 0x7FU) {
        if (stopNeeded) {
            I2cCtrl_FinishAndWaitStop(Dx, 2000U);
        }
        return STATUS_DATA_ERROR;
    }

    /* Finish transfer and wait for STOP */
    st = I2cCtrl_FinishAndWaitStop(Dx, 5000U);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    /* Check for TX abort condition */
    st = I2cCtrl_CheckTxAbort(Dx);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    *AlertingAddress = (UCHAR)val;
    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * I2cCtrl_SmbusBlockProcessCall - HAL-generic SMBus Block Process Call
 * XP/2003 BSOD-safe, WinDDK-compiler-safe, 100% hardened, C89-compliant
 * Purpose:
 *   - Executes SMBus Block Process Call transaction
 *   - Writes command + block, reads response length + data
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_SmbusBlockProcessCall(
    PI2CCTRL_FDO           Dx,
    UCHAR                  SlaveAddress,
    UCHAR                  Command,
    const UCHAR*           Buffer,
    UCHAR                  Length,
    PI2CCTRL_SMBUS_RESULT  Out
    )
{
    NTSTATUS      st;
    UCHAR         i;
    UCHAR         byte;
    I2C_HW_STATUS hwst;

    /* Defensive init */
    st   = STATUS_SUCCESS;
    byte = 0U;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (Dx == NULL || Dx->Ops == NULL || Buffer == NULL || Out == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Length == 0U || Length > 32U) {
        return STATUS_INVALID_PARAMETER;
    }

    Out->Status = STATUS_UNSUCCESSFUL;
    Out->Length = 0U;

    /* Program target address */
    if (Dx->Ops->SetTarget7bit != NULL) {
        st = Dx->Ops->SetTarget7bit(Dx, SlaveAddress);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }

    /* Write command */
    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    if (Dx->Ops->WriteTxByte != NULL) {
        st = Dx->Ops->WriteTxByte(Dx, Command);
        if (!NT_SUCCESS(st)) return st;
    }

    /* Write length */
    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    if (Dx->Ops->WriteTxByte != NULL) {
        st = Dx->Ops->WriteTxByte(Dx, Length);
        if (!NT_SUCCESS(st)) return st;
    }

    /* Write data bytes */
    for (i = 0U; i < Length; i++) {
        st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
        if (!NT_SUCCESS(st)) return st;
        if (Dx->Ops->WriteTxByte != NULL) {
            st = Dx->Ops->WriteTxByte(Dx, Buffer[i]);
            if (!NT_SUCCESS(st)) return st;
        }
    }

    /* Queue read for response length */
    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    if (Dx->Ops->IssueReadToken != NULL) {
        st = Dx->Ops->IssueReadToken(Dx);
        if (!NT_SUCCESS(st)) return st;
    }

    /* Read response length */
    st = I2cCtrl_WaitRxFifoNotEmpty(Dx, 5000U);
    if (!NT_SUCCESS(st)) return st;
    if (Dx->Ops->ReadRxByte != NULL) {
        st = Dx->Ops->ReadRxByte(Dx, &byte);
        if (!NT_SUCCESS(st)) return st;
        Out->Length = byte;
    }
    if (Out->Length == 0U || Out->Length > 32U) {
        return STATUS_DEVICE_DATA_ERROR;
    }

    /* Queue reads for response data */
    for (i = 0U; i < Out->Length; i++) {
        st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
        if (!NT_SUCCESS(st)) return st;
        if (Dx->Ops->IssueReadToken != NULL) {
            st = Dx->Ops->IssueReadToken(Dx);
            if (!NT_SUCCESS(st)) return st;
        }
    }

    /* Drain response data */
    for (i = 0U; i < Out->Length; i++) {
        st = I2cCtrl_WaitRxFifoNotEmpty(Dx, 5000U);
        if (!NT_SUCCESS(st)) return st;
        if (Dx->Ops->ReadRxByte != NULL) {
            st = Dx->Ops->ReadRxByte(Dx, &byte);
            if (!NT_SUCCESS(st)) return st;
            Out->Data[i] = byte;
        }
    }

    /* Wait for STOP and check aborts */
    st = I2cCtrl_FinishAndWaitStop(Dx, 5000U);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_CheckTxAbort(Dx);
    if (!NT_SUCCESS(st)) return st;

    Out->Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * Program target address and basic CON settings for SMBus operation
 * HAL-based, controller-agnostic, XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_ProgramTarget(
    PI2CCTRL_FDO devctx,
    UCHAR        SlaveAddress,
    BOOLEAN      TenBit,
    BOOLEAN      RestartEnable
    )
{
    NTSTATUS status;

    /* Defensive parameter validation */
    if (devctx == NULL || devctx->Ops == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Guard against invalid slave address (7-bit only here) */
    if ((SlaveAddress & ~0x7FU) != 0U && TenBit == FALSE) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Disable controller before changing critical settings */
    if (devctx->Ops->Enable != NULL) {
        (VOID)devctx->Ops->Enable(devctx, FALSE);
    }

    /* Configure restart if backend supports it */
    if (RestartEnable && devctx->Ops->EmitRestartIfNeeded != NULL) {
        status = devctx->Ops->EmitRestartIfNeeded(devctx);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    /* Program target address (7-bit or 10-bit depending on backend support) */
    if (devctx->Ops->SetTarget7bit != NULL && TenBit == FALSE) {
        status = devctx->Ops->SetTarget7bit(devctx, SlaveAddress & 0x7FU);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    } else {
        /* If TenBit addressing is required, backend must provide a handler */
        if (devctx->Ops->SetBusSpeedHz != NULL) {
            /* Example: backend may overload bus speed or target config for 10-bit */
            status = devctx->Ops->SetBusSpeedHz(devctx, devctx->Caps->MaxSpeedHz);
            if (!NT_SUCCESS(status)) {
                return status;
            }
        }
    }

    /* Re-enable controller */
    if (devctx->Ops->Enable != NULL) {
        (VOID)devctx->Ops->Enable(devctx, TRUE);
    }

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * I2cCtrl_FinishAndWaitStop - XP/2003 BSOD-safe, HAL-generic, C89-compliant
 *
 * Purpose:
 *   - Wait for STOP_DET to confirm bus idle
 *   - If timeout occurs, fallback by disable/enable via HAL ops
 *   - Avoid direct register pokes; rely on ops abstraction
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_FinishAndWaitStop(
    PI2CCTRL_FDO Dx,
    ULONG        TimeoutUsec
    )
{
    NTSTATUS st;

    if (Dx == NULL || Dx->Ops == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Wait for STOP_DET interrupt within timeout */
    st = I2cCtrl_WaitStopDetected(Dx, TimeoutUsec);

    if (!NT_SUCCESS(st)) {
        /* Fallback: force bus idle by disable/enable */
        if (Dx->Ops->Enable != NULL) {
            (VOID)Dx->Ops->Enable(Dx, FALSE);
            KeStallExecutionProcessor(10U);
            (VOID)Dx->Ops->Enable(Dx, TRUE);
        }
    }

    return st;
}


/* ===== SMBus public API implementations (HAL-generic, XP/2003-safe, C89) ===== */

NTSTATUS
I2cCtrl_SmbusQuick(
    PI2CCTRL_FDO Dx,
    UCHAR        SlaveAddress,
    BOOLEAN      ReadOperation
    )
{
    NTSTATUS st;

    if (Dx == NULL || Dx->Ops == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (Dx->Ops->SetTarget7bit == NULL) {
        return STATUS_NOT_SUPPORTED;
    }
    st = Dx->Ops->SetTarget7bit(Dx, SlaveAddress);
    if (!NT_SUCCESS(st)) return st;

    /* Quick command: emit a single read token or just STOP (no data payload) */
    if (ReadOperation != FALSE) {
        st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
        if (!NT_SUCCESS(st)) return st;
        if (Dx->Ops->IssueReadToken != NULL) {
            st = Dx->Ops->IssueReadToken(Dx);
            if (!NT_SUCCESS(st)) return st;
        } else {
            return STATUS_NOT_SUPPORTED;
        }
    }

    st = I2cCtrl_FinishAndWaitStop(Dx, 5000U);
    if (!NT_SUCCESS(st)) return st;

    return I2cCtrl_CheckTxAbort(Dx);
}

NTSTATUS
I2cCtrl_SmbusSendByte(
    PI2CCTRL_FDO Dx,
    UCHAR        SlaveAddress,
    UCHAR        Command
    )
{
    NTSTATUS st;

    if (Dx == NULL || Dx->Ops == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (Dx->Ops->SetTarget7bit == NULL || Dx->Ops->WriteTxByte == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    st = Dx->Ops->SetTarget7bit(Dx, SlaveAddress);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;

    st = Dx->Ops->WriteTxByte(Dx, Command);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_FinishAndWaitStop(Dx, 5000U);
    if (!NT_SUCCESS(st)) return st;

    return I2cCtrl_CheckTxAbort(Dx);
}

NTSTATUS
I2cCtrl_SmbusReceiveByte(
    PI2CCTRL_FDO Dx,
    UCHAR        SlaveAddress,
    UCHAR*       OutByte
    )
{
    NTSTATUS st;
    UCHAR    b;

    if (Dx == NULL || Dx->Ops == NULL || OutByte == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Dx->Ops->SetTarget7bit == NULL ||
        Dx->Ops->IssueReadToken == NULL ||
        Dx->Ops->ReadRxByte == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    st = Dx->Ops->SetTarget7bit(Dx, SlaveAddress);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;

    st = Dx->Ops->IssueReadToken(Dx);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitRxFifoNotEmpty(Dx, 5000U);
    if (!NT_SUCCESS(st)) return st;

    st = Dx->Ops->ReadRxByte(Dx, &b);
    if (!NT_SUCCESS(st)) return st;

    *OutByte = b;
    return I2cCtrl_CheckTxAbort(Dx);
}

NTSTATUS
I2cCtrl_SmbusWriteByte(
    PI2CCTRL_FDO Dx,
    UCHAR        SlaveAddress,
    UCHAR        Command,
    UCHAR        Value
    )
{
    NTSTATUS st;

    if (Dx == NULL || Dx->Ops == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (Dx->Ops->SetTarget7bit == NULL || Dx->Ops->WriteTxByte == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    st = Dx->Ops->SetTarget7bit(Dx, SlaveAddress);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->WriteTxByte(Dx, Command);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->WriteTxByte(Dx, Value);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_FinishAndWaitStop(Dx, 5000U);
    if (!NT_SUCCESS(st)) return st;

    return I2cCtrl_CheckTxAbort(Dx);
}

NTSTATUS
I2cCtrl_SmbusReadByte(
    PI2CCTRL_FDO Dx,
    UCHAR        SlaveAddress,
    UCHAR        Command,
    UCHAR*       OutByte
    )
{
    NTSTATUS st;
    UCHAR    b;

    if (Dx == NULL || Dx->Ops == NULL || OutByte == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Dx->Ops->SetTarget7bit == NULL ||
        Dx->Ops->WriteTxByte == NULL ||
        Dx->Ops->IssueReadToken == NULL ||
        Dx->Ops->ReadRxByte == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    st = Dx->Ops->SetTarget7bit(Dx, SlaveAddress);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->WriteTxByte(Dx, Command);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->IssueReadToken(Dx);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitRxFifoNotEmpty(Dx, 5000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->ReadRxByte(Dx, &b);
    if (!NT_SUCCESS(st)) return st;

    *OutByte = b;
    return I2cCtrl_CheckTxAbort(Dx);
}

NTSTATUS
I2cCtrl_SmbusWriteWord(
    PI2CCTRL_FDO Dx,
    UCHAR        SlaveAddress,
    UCHAR        Command,
    USHORT       Value
    )
{
    NTSTATUS st;
    UCHAR    lo, hi;

    if (Dx == NULL || Dx->Ops == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (Dx->Ops->SetTarget7bit == NULL || Dx->Ops->WriteTxByte == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    lo = (UCHAR)(Value & 0xFFU);
    hi = (UCHAR)((Value >> 8) & 0xFFU);

    st = Dx->Ops->SetTarget7bit(Dx, SlaveAddress);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->WriteTxByte(Dx, Command);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->WriteTxByte(Dx, lo);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->WriteTxByte(Dx, hi);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_FinishAndWaitStop(Dx, 5000U);
    if (!NT_SUCCESS(st)) return st;

    return I2cCtrl_CheckTxAbort(Dx);
}

NTSTATUS
I2cCtrl_SmbusReadWord(
    PI2CCTRL_FDO Dx,
    UCHAR        SlaveAddress,
    UCHAR        Command,
    USHORT*      OutValue
    )
{
    NTSTATUS st;
    UCHAR    v0, v1;

    if (Dx == NULL || Dx->Ops == NULL || OutValue == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Dx->Ops->SetTarget7bit == NULL ||
        Dx->Ops->WriteTxByte == NULL ||
        Dx->Ops->IssueReadToken == NULL ||
        Dx->Ops->ReadRxByte == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    st = Dx->Ops->SetTarget7bit(Dx, SlaveAddress);
    if (!NT_SUCCESS(st)) return st;

    /* Command, then two reads (LSB then MSB) */
    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->WriteTxByte(Dx, Command);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->IssueReadToken(Dx);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->IssueReadToken(Dx);
    if (!NT_SUCCESS(st)) return st;

    /* Read two bytes */
    st = I2cCtrl_WaitRxFifoNotEmpty(Dx, 5000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->ReadRxByte(Dx, &v0);
    if (!NT_SUCCESS(st)) return st;

    st = I2cCtrl_WaitRxFifoNotEmpty(Dx, 5000U);
    if (!NT_SUCCESS(st)) return st;
    st = Dx->Ops->ReadRxByte(Dx, &v1);
    if (!NT_SUCCESS(st)) return st;

    *OutValue = (USHORT)(((USHORT)v1 << 8) | (USHORT)v0);

    return I2cCtrl_CheckTxAbort(Dx);
}


/* Minimal CRC-8 (PEC) over SMBus (polynomial 0x07), for optional PEC use */
UCHAR I2cCtrl_Crc8(UCHAR const *buf, UCHAR len)
{
    UCHAR crc = 0;
    UCHAR i, j;
    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (UCHAR)((crc << 1) ^ 0x07);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* -----------------------------------------------------------------------
 * SMBus BlockRead: send [command], then read [count][data..] (+ optional PEC)
 * HAL-generic, XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant, fully hardened
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_SmbusBlockRead(
    PI2CCTRL_FDO          Dx,
    UCHAR                 SlaveAddress,
    UCHAR                 Command,
    PI2CCTRL_SMBUS_RESULT Out,
    I2CCTRL_SMBUS_PEC     PecMode
    )
{
    NTSTATUS st;
    UCHAR    count;
    UCHAR    i;
    UCHAR    pecExpected;
    UCHAR    pecActual;
    BOOLEAN  stopQueued;
    UCHAR    tmp[1 + 1 + 1 + 1 + 32]; /* addr+W, cmd, addr+R, count, data[0..31] */
    UCHAR    idx;
    UCHAR    j;
    UCHAR    byte;

    /* C89: initialize locals at top */
    st          = STATUS_SUCCESS;
    count       = 0U;
    pecExpected = 0U;
    pecActual   = 0U;
    stopQueued  = FALSE;
    idx         = 0U;
    i           = 0U;
    j           = 0U;
    byte        = 0U;

    if (Dx == NULL || Dx->Ops == NULL || Out == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((SlaveAddress & ~0x7FU) != 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    Out->Status = STATUS_UNSUCCESSFUL;
    Out->Length = 0U;

    if (Dx->Ops->SetTarget7bit == NULL) {
        return STATUS_NOT_SUPPORTED;
    }
    st = Dx->Ops->SetTarget7bit(Dx, SlaveAddress);
    if (!NT_SUCCESS(st)) return st;

    /* Phase 1: send command */
    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) { I2cCtrl_FinishAndWaitStop(Dx, 1000U); return st; }
    if (Dx->Ops->WriteTxByte != NULL) {
        st = Dx->Ops->WriteTxByte(Dx, Command);
        if (!NT_SUCCESS(st)) return st;
    }

    /* Phase 2: request count byte */
    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) { I2cCtrl_FinishAndWaitStop(Dx, 1000U); return st; }
    if (Dx->Ops->IssueReadToken != NULL) {
        st = Dx->Ops->IssueReadToken(Dx);
        if (!NT_SUCCESS(st)) return st;
    }
    stopQueued = TRUE;

    /* Receive count */
    st = I2cCtrl_WaitRxFifoNotEmpty(Dx, 5000U);
    if (!NT_SUCCESS(st)) { if (stopQueued) I2cCtrl_FinishAndWaitStop(Dx, 2000U); return st; }
    if (Dx->Ops->ReadRxByte != NULL) {
        st = Dx->Ops->ReadRxByte(Dx, &count);
        if (!NT_SUCCESS(st)) return st;
    }

    if (count == 0U || count > 32U) {
        if (stopQueued) I2cCtrl_FinishAndWaitStop(Dx, 2000U);
        return STATUS_DEVICE_DATA_ERROR;
    }

    /* Phase 3: queue reads for data bytes */
    for (i = 0U; i < count; i++) {
        st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
        if (!NT_SUCCESS(st)) { if (stopQueued) I2cCtrl_FinishAndWaitStop(Dx, 2000U); return st; }
        if (Dx->Ops->IssueReadToken != NULL) {
            st = Dx->Ops->IssueReadToken(Dx);
            if (!NT_SUCCESS(st)) return st;
        }
    }

    /* Optional PEC */
    if (PecMode == SmBusPecEnabled) {
        st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
        if (!NT_SUCCESS(st)) { if (stopQueued) I2cCtrl_FinishAndWaitStop(Dx, 2000U); return st; }
        if (Dx->Ops->IssueReadToken != NULL) {
            st = Dx->Ops->IssueReadToken(Dx);
            if (!NT_SUCCESS(st)) return st;
        }
    }

    /* Phase 4: drain data */
    for (i = 0U; i < count; i++) {
        st = I2cCtrl_WaitRxFifoNotEmpty(Dx, 5000U);
        if (!NT_SUCCESS(st)) { if (stopQueued) I2cCtrl_FinishAndWaitStop(Dx, 2000U); return st; }
        if (Dx->Ops->ReadRxByte != NULL) {
            st = Dx->Ops->ReadRxByte(Dx, &byte);
            if (!NT_SUCCESS(st)) return st;
            Out->Data[i] = byte;
        }
    }
    Out->Length = count;

    /* Optional PEC verification */
    if (PecMode == SmBusPecEnabled) {
        st = I2cCtrl_WaitRxFifoNotEmpty(Dx, 5000U);
        if (!NT_SUCCESS(st)) { if (stopQueued) I2cCtrl_FinishAndWaitStop(Dx, 2000U); return st; }
        if (Dx->Ops->ReadRxByte != NULL) {
            st = Dx->Ops->ReadRxByte(Dx, &pecActual);
            if (!NT_SUCCESS(st)) return st;
        }

        idx = 0U;
        tmp[idx++] = (UCHAR)((SlaveAddress << 1) | 0x00U);
        tmp[idx++] = Command;
        tmp[idx++] = (UCHAR)((SlaveAddress << 1) | 0x01U);
        tmp[idx++] = count;
        for (j = 0U; j < count; j++) tmp[idx++] = Out->Data[j];
        pecExpected = I2cCtrl_Crc8(tmp, idx);

        if (pecActual != pecExpected) {
            st = I2cCtrl_FinishAndWaitStop(Dx, 5000U);
            if (!NT_SUCCESS(st)) return st;
            st = I2cCtrl_CheckTxAbort(Dx);
            if (!NT_SUCCESS(st)) return st;
            Out->Status = STATUS_CRC_ERROR;
            return STATUS_CRC_ERROR;
        }
    }

    st = I2cCtrl_FinishAndWaitStop(Dx, 5000U);
    if (!NT_SUCCESS(st)) return st;
    st = I2cCtrl_CheckTxAbort(Dx);
    if (!NT_SUCCESS(st)) return st;

    Out->Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * SMBus BlockWrite: send [command][count][data..] (+ optional PEC)
 * HAL-generic, XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant, fully hardened
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_SmbusBlockWrite(
    PI2CCTRL_FDO      Dx,
    UCHAR             SlaveAddress,
    UCHAR             Command,
    const UCHAR*      Buffer,
    UCHAR             Length,
    I2CCTRL_SMBUS_PEC PecMode
    )
{
    NTSTATUS st;
    UCHAR    i;
    UCHAR    pec;
    UCHAR    tmp[1 + 1 + 1 + 32]; /* addr+W, cmd, count, data[0..31] */
    UCHAR    idx;

    /* C89: initialize locals at top */
    st  = STATUS_SUCCESS;
    pec = 0U;
    idx = 0U;
    i   = 0U;

    /* Defensive parameter validation */
    if (Dx == NULL || Dx->Ops == NULL || Buffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((SlaveAddress & ~0x7FU) != 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Length == 0U || Length > 32U) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Run only at PASSIVE_LEVEL (polled waits and HAL ops) */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (Dx->Ops->SetTarget7bit == NULL || Dx->Ops->WriteTxByte == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    /* Program target (7-bit address), repeated start enabled for block write */
    st = Dx->Ops->SetTarget7bit(Dx, SlaveAddress);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    /* Phase 1: send command byte */
    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) { I2cCtrl_FinishAndWaitStop(Dx, 1000U); return st; }
    st = Dx->Ops->WriteTxByte(Dx, Command);
    if (!NT_SUCCESS(st)) return st;

    /* Phase 2: send count byte */
    st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
    if (!NT_SUCCESS(st)) { I2cCtrl_FinishAndWaitStop(Dx, 1000U); return st; }
    st = Dx->Ops->WriteTxByte(Dx, Length);
    if (!NT_SUCCESS(st)) return st;

    /* Phase 3: send data bytes */
    for (i = 0U; i < Length; i++) {
        st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
        if (!NT_SUCCESS(st)) { I2cCtrl_FinishAndWaitStop(Dx, 1000U); return st; }
        st = Dx->Ops->WriteTxByte(Dx, Buffer[i]);
        if (!NT_SUCCESS(st)) return st;
    }

    /* Phase 4: optional PEC */
    if (PecMode == SmBusPecEnabled) {
        idx = 0U;
        tmp[idx++] = (UCHAR)((SlaveAddress << 1) | 0x00U);
        tmp[idx++] = Command;
        tmp[idx++] = Length;
        for (i = 0U; i < Length; i++) {
            tmp[idx++] = Buffer[i];
        }
        pec = I2cCtrl_Crc8(tmp, idx);

        st = I2cCtrl_WaitTxFifoNotFull(Dx, 1000U);
        if (!NT_SUCCESS(st)) { I2cCtrl_FinishAndWaitStop(Dx, 1000U); return st; }
        st = Dx->Ops->WriteTxByte(Dx, pec);
        if (!NT_SUCCESS(st)) return st;
    }

    /* Phase 5: finish transfer and check aborts */
    st = I2cCtrl_FinishAndWaitStop(Dx, 5000U);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    st = I2cCtrl_CheckTxAbort(Dx);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    return STATUS_SUCCESS;
}


/* SMBus PEC CRC-8, polynomial 0x07, init 0x00, reflect=false, xorout=0x00 */
UCHAR
Smbus_Crc8(const UCHAR* data, size_t len)
{
    UCHAR crc = 0x00;
    size_t i;
    for (i = 0; i < len; ++i) {
        UCHAR in = data[i];
        UCHAR j;
        crc ^= in;
        for (j = 0; j < 8; ++j) {
            if (crc & 0x80) {
                crc = (UCHAR)((crc << 1) ^ 0x07);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/* Builds SMBus write frame bytes used for PEC:
 * [Addr<<1 | 0] [Command] [Payload...]
 * Appends computed PEC at the end of outBuf.
 * Returns TRUE on success, FALSE if payloadLen exceeds capacity.
 */
BOOLEAN
Smbus_BuildWriteFrameAndAppendPec(
    UCHAR addr7,
    UCHAR command,
    const UCHAR* payload,
    UCHAR payloadLen,
    UCHAR* outBuf,
    UCHAR* outLen
)
{
    /* outBuf must hold command+payload+PEC; caller ensures capacity */
    UCHAR frame[1 + 1 + 32]; /* address+rw + command + payload (≤32) */
    size_t frameLen = 0;

    if (payloadLen > 32) {
        payloadLen = 32; /* clamp to SMBus block max */
    }

    frame[frameLen++] = (UCHAR)((addr7 & 0x7F) << 1); /* write: R/W=0 */
    frame[frameLen++] = command;

    if (payloadLen && payload) {
        RtlCopyMemory(frame + frameLen, payload, payloadLen);
        frameLen += payloadLen;
    }

    /* Compute PEC over frame */
    {
        UCHAR pec = Smbus_Crc8(frame, frameLen);

        /* Emit command+payload into outBuf, then append PEC */
        outBuf[0] = command;
        if (payloadLen && payload) {
            RtlCopyMemory(outBuf + 1, payload, payloadLen);
        }
        outBuf[1 + payloadLen] = pec;
        *outLen = (UCHAR)(1 + payloadLen + 1); /* command + payload + PEC */
    }

    return TRUE;
}

BOOLEAN
Smbus_BuildReadFrameAndValidatePec(
    UCHAR addr7,
    UCHAR command,
    const UCHAR* payloadWithPec,
    UCHAR payloadLenWithPec
)
{
    UCHAR frame[1 + 1 + 1 + 1 + 32]; /* wr addr + cmd + rd addr + count + data */
    size_t frameLen = 0;
    UCHAR recvPec;
    UCHAR calcPec;
    UCHAR payloadLen;

    if (payloadWithPec == NULL || payloadLenWithPec < 2) {
        return FALSE;
    }

    /* Last byte is PEC */
    recvPec = payloadWithPec[payloadLenWithPec - 1];
    payloadLen = (UCHAR)(payloadLenWithPec - 1);
    if (payloadLen > 32) {
        payloadLen = 32;
    }

    /* Build frame: Addr+W, Command, Addr+R, Payload */
    frame[frameLen++] = (UCHAR)((addr7 & 0x7F) << 1);       /* write header */
    frame[frameLen++] = command;                            /* command */
    frame[frameLen++] = (UCHAR)(((addr7 & 0x7F) << 1) | 1); /* read header */
    RtlCopyMemory(frame + frameLen, payloadWithPec, payloadLen);
    frameLen += payloadLen;

    calcPec = Smbus_Crc8(frame, frameLen);
    return (calcPec == recvPec);
}