/* i2cctrl_hal_intel.c
 *
 * Intel DesignWare-style I2C backend for I2C-Controller-XP
 * XP/2003-safe, C89-compliant skeleton.
 */

#include <ntddk.h>
#include "i2cctrl_hal_ops.h"
#include "i2cctrl_hal_caps.h"
#include "i2cctrl_ext.h"    /* I2cCtrl_InstallBackend declaration and I2CCTRL_FDO */

/* ---------------------------------------------------------------------------
 * External helpers / globals from the rest of the driver
 * --------------------------------------------------------------------------- */

typedef struct _I2C_REG_MAP {
    ULONG ControlReg;
    ULONG StatusReg;
    ULONG DataReg;
    ULONG ClockReg;
    ULONG Quirks;
    ULONG BsodQuirks;
} I2C_REG_MAP, *PI2C_REG_MAP;

/* Some bits used by higher layers (see i2cctrl.c / i2cctrl_isr.c) */
#define INTEL_STAT_TX_EMPTY_BIT   0x00000004U  /* example: TX FIFO empty */
#define INTEL_STAT_RX_FULL_BIT    0x00000008U  /* example: RX FIFO full  */
#define INTEL_STAT_RX_NOT_EMPTY   0x00000010U  /* example: RX FIFO not empty */
#define INTEL_STAT_ARB_LOST_BIT   0x00000100U  /* example: arbitration lost */
#define INTEL_STAT_STOP_DET_BIT   0x00000020U  /* example: STOP detected */
#define INTEL_STAT_TX_ABRT_BIT    0x00000040U  /* example: TX abort */

/* You can adjust these to your real Intel controller’s register layout */
#define INTEL_REG_CON             0x00U  /* control */
#define INTEL_REG_TAR             0x04U  /* target address */
#define INTEL_REG_DATA_CMD        0x08U  /* data / command */
#define INTEL_REG_SS_SCL_HCNT     0x0CU  /* clock config (example) */
#define INTEL_REG_INTR_STATUS     0x10U  /* raw interrupt status */
#define INTEL_REG_INTR_MASK       0x14U  /* interrupt mask */
#define INTEL_REG_CLR_INTR        0x18U  /* clear-all interrupt */
#define INTEL_REG_CLR_TX_ABRT     0x1CU  /* clear TX abort */
#define INTEL_REG_CLR_STOP_DET    0x20U  /* clear STOP detect */
#define INTEL_REG_TXFLR           0x24U  /* TX FIFO level */
#define INTEL_REG_RXFLR           0x28U  /* RX FIFO level */

/* Control bits (example DW I2C-style) */
#define INTEL_CON_ENABLE_BIT      0x00000001U
#define INTEL_CON_MASTER_MODE     0x00000040U
#define INTEL_CON_RESTART_EN      0x00000020U

/* ---------------------------------------------------------------------------
 * Local helpers
 * --------------------------------------------------------------------------- */

static __inline PUCHAR
IntelGetMmioBase(
    PI2CCTRL_FDO devctx
    )
{
    return (devctx != NULL) ? devctx->MmioBase : NULL;
}

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


/* ---------------------------------------------------------------------------
 * Capabilities
 * --------------------------------------------------------------------------- */

I2C_HW_CAPS IntelI2cCaps = {
    16,         /* TxFifoDepth */
    16,         /* RxFifoDepth */

    TRUE,       /* SupportsRestart */
    TRUE,       /* SupportsStopBitInDataCmd */
    TRUE,       /* HasSeparateIntrClearRegs */
    TRUE,       /* HasDedicatedIntrMask */
    TRUE,       /* HasRawIntrStatus */

    FALSE,      /* Supports10BitAddr */
    FALSE,      /* SupportsSlaveMode */

    TRUE,       /* DetectsArbitrationLost */
    TRUE,       /* DetectsAddressNack */
    TRUE,       /* DetectsDataNack */

    TRUE,       /* SupportsStandard100k */
    TRUE,       /* SupportsFast400k */
    FALSE,      /* SupportsHigh3_4M (Intel DW rarely supports HS mode) */

    400000,     /* MaxSpeedHz */
    0           /* InputClockHz (unknown on XP) */
};


/* ---------------------------------------------------------------------------
 * Core HAL operations
 * --------------------------------------------------------------------------- */

static NTSTATUS
Intel_MapResources(
    PI2CCTRL_FDO devctx,
    PCM_RESOURCE_LIST translated
    )
{
    UNREFERENCED_PARAMETER(devctx);
    UNREFERENCED_PARAMETER(translated);

    /* StartDevice already maps MMIO and sets devctx->MmioBase.
       Nothing to do here for XP backend. */
    return STATUS_SUCCESS;
}

static VOID
Intel_UnmapResources(
    PI2CCTRL_FDO devctx
    )
{
    UNREFERENCED_PARAMETER(devctx);
    /* MMIO unmapping is handled in StopDevice/RemoveDevice. */
}

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

    /* 7-bit address in low bits; hardware usually expects this. */
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

    /* Very simple example: adjust clock register based on mode.
       TODO: replace with real timing values for your controller. */
    clk = IntelReadReg(devctx, INTEL_REG_SS_SCL_HCNT);

    switch (speed) {
    case I2C_SPEED_STANDARD:
        /* 100 kHz - larger high-count */
        clk = 0x00000100U;
        break;
    case I2C_SPEED_FAST:
        /* 400 kHz */
        clk = 0x00000040U;
        break;
    case I2C_SPEED_HIGH:
        /* 3.4 MHz */
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
    /* Map arbitrary Hz to one of the canonical modes. */
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

    /* Write data byte; no STOP/READ flag. */
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

    /* Issue a read command (READ bit set, no STOP). */
    cmd = 0x00000100U; /* READ flag in DATA_CMD for DW I2C-style */
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
    /* For now, same as ReadRxByte; you can add extra checks if needed. */
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

    if (devctx == NULL || queued == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *queued = 0U;

    for (i = 0U; i < count; i++) {
        NTSTATUS st = Intel_IssueReadToken(devctx);
        if (!NT_SUCCESS(st)) {
            return st;
        }
        (*queued)++;
    }

    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Status / interrupts
 * --------------------------------------------------------------------------- */

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

    /* Zero the struct first; higher layers expect defined fields. */
    RtlZeroMemory(st, sizeof(*st));

    /* These field names must match your actual I2C_HW_STATUS definition. */
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

    /* Clear-all if requested. */
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

    /* Always read CLR_INTR last to clear any remaining latched bits. */
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

    /* In this backend, hwMask is the mask to *enable*; higher layers
       pass in the hardware bits they want active. */
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

    /* There is no direct “flush” in many DW I2C variants; we just wait
       until TX FIFO is empty. */
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

    /* Issue a STOP on the next command (STOP bit in DATA_CMD). */
    cmd = 0x00000200U; /* STOP flag - adjust to your controller */
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

    /* Ensure restart is enabled in CON register. */
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

/* ---------------------------------------------------------------------------
 * Optional FIFO helpers (if you want to wire them later)
 * --------------------------------------------------------------------------- */

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

/* ---------------------------------------------------------------------------
 * Block I/O helpers (used by SMBus / HID paths)
 * --------------------------------------------------------------------------- */

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

    /* First write register index, then payload. */
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

    /* Write register index first. */
    st = Intel_IssueWriteByte(devctx, (UCHAR)(reg & 0xFFU));
    if (!NT_SUCCESS(st)) {
        return st;
    }

    /* Issue read tokens. */
    for (i = 0U; i < len; i++) {
        st = Intel_IssueReadToken(devctx);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }

    /* Read back bytes. */
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

/* ---------------------------------------------------------------------------
 * HAL ops table
 * --------------------------------------------------------------------------- */

I2C_HW_OPS IntelI2cOps = {
    /* Resource mapping */
    Intel_MapResources,
    Intel_UnmapResources,

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
    { 0 }, /* Caps - will be overwritten below */

    /* FIFO management */
    Intel_QuiesceFifos,
    Intel_DrainRxFifo,
    Intel_FlushTxFifo,

    /* Optional raw register access (not used here) */
    NULL,
    NULL,

    /* Resource queries (not used in XP backend) */
    NULL,
    NULL,
    NULL,
    NULL,

    /* Controller helpers */
    NULL,           /* ConfigureController */
    NULL,           /* EmitReadRequest */
    NULL,           /* ReadTxDiscard */
    NULL,           /* QueryTouchSample */

    /* Wake + block I/O */
    NULL,           /* EnableWakeSource */
    Intel_IssueBlockWrite,
    Intel_IssueBlockRead
};

/* Initialize Caps field explicitly (C89-safe) */
static VOID
Intel_InitOpsCaps(VOID)
{
    IntelI2cOps.Caps = IntelI2cCaps;
}

/* ---------------------------------------------------------------------------
 * Backend installer - called from FDO init / AddDevice
 * --------------------------------------------------------------------------- */

VOID
I2cCtrl_InstallBackend(
    PI2CCTRL_FDO devctx
    )
{
    if (devctx == NULL) {
        return;
    }

    Intel_InitOpsCaps();

    devctx->Ops  = &IntelI2cOps;
    devctx->Caps = &IntelI2cOps.Caps;
}
