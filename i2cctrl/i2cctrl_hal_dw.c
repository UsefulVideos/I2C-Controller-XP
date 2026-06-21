/* i2cctrl_hal_dw.c
 *
 * Synopsys DesignWare / Intel Serial IO I2C backend
 * XP/2003-safe, C89-compliant.
 */

#include <ntddk.h>
#include "i2cctrl_hal_ops.h"
#include "i2cctrl_hal_caps.h"
#include "i2cctrl_hal_dw.h"
#include "i2cctrl_ext.h"

static __inline ULONG
IntelReadReg(
    PI2CCTRL_FDO devctx,
    ULONG        offset
    )
{
    return I2cCtrl_ReadRegisterSafe(devctx, offset);
}

static __inline VOID
IntelWriteReg(
    PI2CCTRL_FDO devctx,
    ULONG        offset,
    ULONG        value
    )
{
    I2cCtrl_WriteRegisterSafe(devctx, offset, value);
}

/* Capabilities */

I2C_HW_CAPS DwI2cCaps = {
    16,
    16,

    TRUE,
    TRUE,
    TRUE,
    TRUE,
    TRUE,

    FALSE,
    FALSE,

    TRUE,
    TRUE,
    TRUE,

    TRUE,
    TRUE,
    FALSE,

    400000,
    0
};

/* Core ops */

static NTSTATUS
Intel_Enable(
    PI2CCTRL_FDO devctx,
    BOOLEAN      on
    )
{
    ULONG con;

    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    con = IntelReadReg(devctx, INTEL_REG_CON);

    if (on != FALSE) {
        con |= INTEL_CON_ENABLE_BIT | INTEL_CON_MASTER_MODE;
    } else {
        con &= ~INTEL_CON_ENABLE_BIT;
    }

    IntelWriteReg(devctx, INTEL_REG_CON, con);
    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_SetTarget7bit(
    PI2CCTRL_FDO devctx,
    UCHAR        addr7
    )
{
    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    IntelWriteReg(devctx, INTEL_REG_TAR, (ULONG)(addr7 & 0x7FU));
    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_SetSpeed(
    PI2CCTRL_FDO devctx,
    I2C_SPEED_MODE speed
    )
{
    ULONG clk;

    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    clk = IntelReadReg(devctx, INTEL_REG_SS_SCL_HCNT);

    switch (speed) {
    case I2C_SPEED_STANDARD:
        clk = 0x00000100U;
        break;
    case I2C_SPEED_FAST:
        clk = 0x00000040U;
        break;
    case I2C_SPEED_HIGH:
        clk = 0x00000010U;
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    IntelWriteReg(devctx, INTEL_REG_SS_SCL_HCNT, clk);
    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_SetBusSpeedHz(
    PI2CCTRL_FDO devctx,
    ULONG        speedHz
    )
{
    if (speedHz <= 100000U) {
        return Intel_SetSpeed(devctx, I2C_SPEED_STANDARD);
    } else if (speedHz <= 400000U) {
        return Intel_SetSpeed(devctx, I2C_SPEED_FAST);
    } else {
        return Intel_SetSpeed(devctx, I2C_SPEED_HIGH);
    }
}

static NTSTATUS
Intel_IssueWriteByte(
    PI2CCTRL_FDO devctx,
    UCHAR        byte
    )
{
    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    IntelWriteReg(devctx, INTEL_REG_DATA_CMD, (ULONG)byte);
    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_IssueReadToken(
    PI2CCTRL_FDO devctx
    )
{
    ULONG cmd;

    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    cmd = 0x00000100U;
    IntelWriteReg(devctx, INTEL_REG_DATA_CMD, cmd);
    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_ReadRxByte(
    PI2CCTRL_FDO devctx,
    UCHAR*       out
    )
{
    ULONG val;

    if (devctx == NULL || out == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    val = IntelReadReg(devctx, INTEL_REG_DATA_CMD);
    *out = (UCHAR)(val & 0xFFU);
    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_ReadRxByteSafe(
    PI2CCTRL_FDO devctx,
    UCHAR*       out
    )
{
    return Intel_ReadRxByte(devctx, out);
}

static NTSTATUS
Intel_PrimeWrite(
    PI2CCTRL_FDO devctx,
    const UCHAR* buf,
    ULONG        len,
    ULONG*       pushed
    )
{
    ULONG i;

    if (devctx == NULL || buf == NULL || pushed == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *pushed = 0U;

    for (i = 0U; i < len; i++) {
        IntelWriteReg(devctx, INTEL_REG_DATA_CMD, (ULONG)buf[i]);
        (*pushed)++;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_PrimeReadTokens(
    PI2CCTRL_FDO devctx,
    ULONG        count,
    ULONG*       queued
    )
{
    ULONG i;
    NTSTATUS st;

    if (devctx == NULL || queued == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *queued = 0U;

    for (i = 0U; i < count; i++) {
        st = Intel_IssueReadToken(devctx);
        if (!NT_SUCCESS(st)) {
            return st;
        }
        (*queued)++;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_GetStatus(
    PI2CCTRL_FDO   devctx,
    I2C_HW_STATUS* st
    )
{
    ULONG raw;
    ULONG txflr;
    ULONG rxflr;

    if (devctx == NULL || st == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    raw   = IntelReadReg(devctx, INTEL_REG_INTR_STATUS);
    txflr = IntelReadReg(devctx, INTEL_REG_TXFLR);
    rxflr = IntelReadReg(devctx, INTEL_REG_RXFLR);

    RtlZeroMemory(st, sizeof(*st));

    st->RawIntr        = raw;
    st->TxFifoLevel    = txflr;
    st->RxFifoLevel    = rxflr;
    st->TxFifoNotFull  = (txflr == 0U) ? TRUE : FALSE;
    st->RxFifoNotEmpty = (rxflr != 0U) ? TRUE : FALSE;
    st->AbortBits      = (raw & INTEL_STAT_TX_ABRT_BIT) ? INTEL_STAT_TX_ABRT_BIT : 0U;
    st->StopBits       = (raw & INTEL_STAT_STOP_DET_BIT) ? INTEL_STAT_STOP_DET_BIT : 0U;

    return STATUS_SUCCESS;
}

static VOID
Intel_AckInterrupts(
    PI2CCTRL_FDO devctx,
    ULONG        hwBits
    )
{
    if (devctx == NULL) {
        return;
    }

    if (hwBits == 0xFFFFFFFFU) {
        (VOID)IntelReadReg(devctx, INTEL_REG_CLR_INTR);
        return;
    }

    if ((hwBits & INTEL_STAT_TX_ABRT_BIT) != 0U) {
        (VOID)IntelReadReg(devctx, INTEL_REG_CLR_TX_ABRT);
    }

    if ((hwBits & INTEL_STAT_STOP_DET_BIT) != 0U) {
        (VOID)IntelReadReg(devctx, INTEL_REG_CLR_STOP_DET);
    }

    (VOID)IntelReadReg(devctx, INTEL_REG_CLR_INTR);
}

static VOID
Intel_MaskInterrupts(
    PI2CCTRL_FDO devctx,
    ULONG        hwMask
    )
{
    if (devctx == NULL) {
        return;
    }

    IntelWriteReg(devctx, INTEL_REG_INTR_MASK, hwMask);
}

static VOID
Intel_UnmaskInterrupts(
    PI2CCTRL_FDO devctx,
    ULONG        hwMask
    )
{
    ULONG cur;

    if (devctx == NULL) {
        return;
    }

    cur = IntelReadReg(devctx, INTEL_REG_INTR_MASK);
    cur |= hwMask;
    IntelWriteReg(devctx, INTEL_REG_INTR_MASK, cur);
}

static VOID
Intel_DrainRxBounded(
    PI2CCTRL_FDO devctx
    )
{
    ULONG rxflr;
    UCHAR dummy;

    if (devctx == NULL) {
        return;
    }

    rxflr = IntelReadReg(devctx, INTEL_REG_RXFLR);
    while (rxflr != 0U) {
        (VOID)Intel_ReadRxByte(devctx, &dummy);
        rxflr = IntelReadReg(devctx, INTEL_REG_RXFLR);
    }
}

static VOID
Intel_FlushTxBounded(
    PI2CCTRL_FDO devctx
    )
{
    ULONG txflr;

    if (devctx == NULL) {
        return;
    }

    txflr = IntelReadReg(devctx, INTEL_REG_TXFLR);
    while (txflr != 0U) {
        txflr = IntelReadReg(devctx, INTEL_REG_TXFLR);
    }
}

static NTSTATUS
Intel_EmitStopIfNeeded(
    PI2CCTRL_FDO devctx
    )
{
    ULONG cmd;

    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    cmd = 0x00000200U;
    IntelWriteReg(devctx, INTEL_REG_DATA_CMD, cmd);
    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_EmitRestartIfNeeded(
    PI2CCTRL_FDO devctx
    )
{
    ULONG con;

    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    con = IntelReadReg(devctx, INTEL_REG_CON);
    con |= INTEL_CON_RESTART_EN;
    IntelWriteReg(devctx, INTEL_REG_CON, con);
    return STATUS_SUCCESS;
}

static ULONG
Intel_GetRawIntr(
    PI2CCTRL_FDO devctx
    )
{
    if (devctx == NULL) {
        return 0U;
    }

    return IntelReadReg(devctx, INTEL_REG_INTR_STATUS);
}

static BOOLEAN
Intel_IsArbitrationLost(
    PI2CCTRL_FDO devctx,
    ULONG        snapshot
    )
{
    UNREFERENCED_PARAMETER(devctx);

    return ((snapshot & INTEL_STAT_ARB_LOST_BIT) != 0U) ? TRUE : FALSE;
}

static NTSTATUS
Intel_WriteTxByte(
    PI2CCTRL_FDO devctx,
    UCHAR        byte
    )
{
    return Intel_IssueWriteByte(devctx, byte);
}

static NTSTATUS
Intel_QuiesceFifos(
    PI2CCTRL_FDO devctx
    )
{
    Intel_DrainRxBounded(devctx);
    Intel_FlushTxBounded(devctx);
    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_DrainRxFifo(
    PI2CCTRL_FDO devctx
    )
{
    Intel_DrainRxBounded(devctx);
    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_FlushTxFifo(
    PI2CCTRL_FDO devctx
    )
{
    Intel_FlushTxBounded(devctx);
    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_IssueBlockWrite(
    PI2CCTRL_FDO devctx,
    UCHAR        slaveAddr,
    ULONG        reg,
    PUCHAR       buf,
    ULONG        len,
    PULONG       bytesDone
    )
{
    NTSTATUS st;
    ULONG    pushed;

    if (bytesDone != NULL) {
        *bytesDone = 0U;
    }

    st = Intel_SetTarget7bit(devctx, slaveAddr);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    st = Intel_IssueWriteByte(devctx, (UCHAR)(reg & 0xFFU));
    if (!NT_SUCCESS(st)) {
        return st;
    }

    st = Intel_PrimeWrite(devctx, buf, len, &pushed);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    if (bytesDone != NULL) {
        *bytesDone = pushed;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
Intel_IssueBlockRead(
    PI2CCTRL_FDO devctx,
    UCHAR        slaveAddr,
    ULONG        reg,
    PUCHAR       buf,
    ULONG        len,
    PULONG       bytesDone
    )
{
    NTSTATUS st;
    ULONG    i;

    if (bytesDone != NULL) {
        *bytesDone = 0U;
    }

    st = Intel_SetTarget7bit(devctx, slaveAddr);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    st = Intel_IssueWriteByte(devctx, (UCHAR)(reg & 0xFFU));
    if (!NT_SUCCESS(st)) {
        return st;
    }

    for (i = 0U; i < len; i++) {
        st = Intel_IssueReadToken(devctx);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }

    for (i = 0U; i < len; i++) {
        st = Intel_ReadRxByte(devctx, &buf[i]);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }

    if (bytesDone != NULL) {
        *bytesDone = len;
    }

    return STATUS_SUCCESS;
}

/* Initialize Caps field explicitly (C89-safe) */
static VOID
DwI2c_InitCaps(VOID)
{
    DwI2cOps.Caps = DwI2cCaps;
}

/* Global ops table */

I2C_HW_OPS DwI2cOps = {
    /* Resource mapping (use shared helpers) */
    I2cCtrl_MapMmio,
    I2cCtrl_UnmapMmio,

    /* Core control */
    Intel_Enable,
    Intel_SetTarget7bit,

    /* Speed */
    Intel_SetSpeed,
    Intel_SetBusSpeedHz,

    /* Byte-level I/O */
    Intel_IssueWriteByte,
    Intel_IssueReadToken,
    Intel_ReadRxByte,
    Intel_ReadRxByteSafe,

    /* FIFO priming */
    Intel_PrimeWrite,
    Intel_PrimeReadTokens,

    /* Status / interrupts */
    Intel_GetStatus,
    Intel_AckInterrupts,
    Intel_MaskInterrupts,
    Intel_UnmaskInterrupts,

    /* Bounded FIFO helpers */
    Intel_DrainRxBounded,
    Intel_FlushTxBounded,

    /* Stop / restart */
    Intel_EmitStopIfNeeded,
    Intel_EmitRestartIfNeeded,

    /* Raw interrupt + arbitration */
    Intel_GetRawIntr,
    Intel_IsArbitrationLost,
    Intel_WriteTxByte,

    /* Capabilities */
    { 0 },

    /* FIFO management */
    Intel_QuiesceFifos,
    Intel_DrainRxFifo,
    Intel_FlushTxFifo,

    /* Optional raw register access */
    NULL,
    NULL,

    /* Resource queries */
    NULL,
    NULL,
    NULL,
    NULL,

    /* Controller helpers */
    NULL,
    NULL,
    NULL,

    NULL,
    NULL,
    Intel_IssueBlockWrite,
    Intel_IssueBlockRead
};
