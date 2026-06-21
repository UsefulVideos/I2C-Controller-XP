#include <ntddk.h>
#include "i2cctrl_hal_ops.h"
#include "i2cctrl_hal_caps.h"
#include "i2cctrl_ext.h"
#include "i2cctrl_hal_acpi.h"

/* ---------------------------------------------------------------------------
 * ACPI-backed I2C controller
 * Uses I2cCtrl_AcpiOpen / I2cCtrl_AcpiTransfer / I2cCtrl_AcpiClose
 * --------------------------------------------------------------------------- */

I2C_HW_CAPS AcpiI2cCaps = {
    1, 1,          /* FIFO depths (logical) */

    TRUE,          /* SupportsRestart */
    TRUE,          /* SupportsStopBitInDataCmd */
    FALSE,         /* HasSeparateIntrClearRegs */
    FALSE,         /* HasDedicatedIntrMask */
    FALSE,         /* HasRawIntrStatus */

    FALSE,         /* Supports10BitAddr */
    FALSE,         /* SupportsSlaveMode */

    TRUE,          /* DetectsArbitrationLost (ACPI returns NACK/ABORT) */
    TRUE,          /* DetectsAddressNack */
    TRUE,          /* DetectsDataNack */

    TRUE,          /* SupportsStandard100k */
    TRUE,          /* SupportsFast400k */
    FALSE,         /* SupportsHigh3_4M */

    400000,        /* MaxSpeedHz */
    0              /* InputClockHz unknown */
};

/* ---------------------------------------------------------------------------
 * Resource mapping
 * ACPI controllers do NOT expose MMIO; they use ACPI OpRegions.
 * --------------------------------------------------------------------------- */

static NTSTATUS
Acpi_MapResources(
    PI2CCTRL_FDO devctx,
    PCM_RESOURCE_LIST translated
    )
{
    UNREFERENCED_PARAMETER(devctx);
    UNREFERENCED_PARAMETER(translated);

    /* ACPI backend has no MMIO or IRQ resources to map */
    return STATUS_SUCCESS;
}

static VOID
Acpi_UnmapResources(
    PI2CCTRL_FDO devctx
    )
{
    I2cCtrl_AcpiClose(devctx);
}

/* ---------------------------------------------------------------------------
 * Core control
 * --------------------------------------------------------------------------- */

static NTSTATUS
Acpi_Enable(
    PI2CCTRL_FDO devctx,
    BOOLEAN      on
    )
{
    UNREFERENCED_PARAMETER(devctx);
    UNREFERENCED_PARAMETER(on);
    return STATUS_SUCCESS;
}

static NTSTATUS
Acpi_SetTarget7bit(
    PI2CCTRL_FDO devctx,
    UCHAR        addr7
    )
{
    devctx->AcpiTarget = addr7 & 0x7F;
    return STATUS_SUCCESS;
}

static NTSTATUS
Acpi_SetSpeed(
    PI2CCTRL_FDO devctx,
    I2C_SPEED_MODE speed
    )
{
    UNREFERENCED_PARAMETER(devctx);
    UNREFERENCED_PARAMETER(speed);
    return STATUS_SUCCESS;
}

static NTSTATUS
Acpi_SetBusSpeedHz(
    PI2CCTRL_FDO devctx,
    ULONG        hz
    )
{
    UNREFERENCED_PARAMETER(devctx);
    UNREFERENCED_PARAMETER(hz);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Byte-level I/O (implemented via ACPI transfer engine)
 * --------------------------------------------------------------------------- */

static NTSTATUS
Acpi_IssueWriteByte(
    PI2CCTRL_FDO devctx,
    UCHAR        byte
    )
{
    return I2cCtrl_AcpiTransfer(devctx,
                                devctx->AcpiTarget,
                                &byte,
                                1,
                                NULL,
                                0);
}

static NTSTATUS
Acpi_IssueReadToken(
    PI2CCTRL_FDO devctx
    )
{
    devctx->AcpiPendingReads++;
    return STATUS_SUCCESS;
}

static NTSTATUS
Acpi_ReadRxByte(
    PI2CCTRL_FDO devctx,
    UCHAR*       out
    )
{
    if (out == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    return I2cCtrl_AcpiTransfer(devctx,
                                devctx->AcpiTarget,
                                NULL,
                                0,
                                out,
                                1);
}

static NTSTATUS
Acpi_ReadRxByteSafe(
    PI2CCTRL_FDO devctx,
    UCHAR*       out
    )
{
    return Acpi_ReadRxByte(devctx, out);
}

/* ---------------------------------------------------------------------------
 * FIFO priming (ACPI has no FIFO)
 * --------------------------------------------------------------------------- */

static NTSTATUS
Acpi_PrimeWrite(
    PI2CCTRL_FDO devctx,
    const UCHAR* buf,
    ULONG        len,
    ULONG*       pushed
    )
{
    if (pushed != NULL) {
        *pushed = len;
    }

    return I2cCtrl_AcpiTransfer(devctx,
                                devctx->AcpiTarget,
                                (PUCHAR)buf,
                                len,
                                NULL,
                                0);
}

static NTSTATUS
Acpi_PrimeReadTokens(
    PI2CCTRL_FDO devctx,
    ULONG        count,
    ULONG*       queued
    )
{
    if (queued != NULL) {
        *queued = count;
    }

    devctx->AcpiPendingReads += count;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Status / interrupts (ACPI-backed controller)
 * ACPI has no hardware IRQs, but we expose meaningful status based on the
 * last ACPI transfer result.
 * --------------------------------------------------------------------------- */

static NTSTATUS
Acpi_GetStatus(
    PI2CCTRL_FDO   devctx,
    I2C_HW_STATUS* st
    )
{
    if (devctx == NULL || st == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(st, sizeof(*st));

    /*
     * ACPI transfer engine reports:
     *   devctx->AcpiLastStatus:
     *       STATUS_SUCCESS
     *       STATUS_IO_DEVICE_ERROR
     *       STATUS_DEVICE_BUSY
     *       STATUS_NO_SUCH_DEVICE
     *       STATUS_INVALID_PARAMETER
     *
     * We map these to logical I2C_HW_STATUS fields.
     */

    if (NT_SUCCESS(devctx->AcpiLastStatus)) {
        st->RawIntr        = 0;
        st->TxFifoLevel    = 0;
        st->RxFifoLevel    = 0;
        st->TxFifoNotFull  = TRUE;
        st->RxFifoNotEmpty = FALSE;
        st->AbortBits      = 0;
        st->StopBits       = 1; /* ACPI transfers always end with STOP */
    } else {
        /* Error path: treat as TX abort */
        st->AbortBits      = INTEL_STAT_TX_ABRT_BIT;
        st->StopBits       = 0;
    }

    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Interrupt handling for ACPI-backed I2C controllers
 * ACPI has no hardware IRQ lines, but we emulate interrupt state so that
 * upper layers (ISR/DPC logic) behave consistently.
 * --------------------------------------------------------------------------- */

static VOID
Acpi_AckInterrupts(
    PI2CCTRL_FDO devctx,
    ULONG        bits
    )
{
    if (devctx == NULL) {
        return;
    }

    /*
     * ACPI interrupt model:
     *   - STOP condition always ends a transfer
     *   - ABORT/NACK is reported via AcpiLastStatus
     *   - We maintain a software RawIntr bitmap
     */

    /* Clear only the bits the caller asked to acknowledge */
    devctx->AcpiRawIntr &= ~bits;
}

static VOID
Acpi_MaskInterrupts(
    PI2CCTRL_FDO devctx,
    ULONG        mask
    )
{
    if (devctx == NULL) {
        return;
    }

    /*
     * ACPI has no hardware mask register.
     * We maintain a software mask so that upper layers can
     * enable/disable logical interrupt sources.
     */

    devctx->AcpiIntrMask = mask;
}

static VOID
Acpi_UnmaskInterrupts(
    PI2CCTRL_FDO devctx,
    ULONG        mask
    )
{
    if (devctx == NULL) {
        return;
    }

    /*
     * Software unmask: OR the bits into the mask.
     */

    devctx->AcpiIntrMask |= mask;
}


/* ---------------------------------------------------------------------------
 * FIFO helpers (ACPI-backed controller)
 * ACPI has no hardware FIFOs, but we emulate FIFO semantics using the
 * pending-read counter and synchronous ACPI transfers.
 * --------------------------------------------------------------------------- */

static NTSTATUS
Acpi_QuiesceFifos(PI2CCTRL_FDO devctx)
{
    UCHAR dummy;
    NTSTATUS st;

    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Drain all pending reads */
    while (devctx->AcpiPendingReads > 0) {

        st = I2cCtrl_AcpiTransfer(
                devctx,
                devctx->AcpiTarget,
                NULL,
                0,
                &dummy,
                1);

        if (!NT_SUCCESS(st)) {
            return st;
        }

        devctx->AcpiPendingReads--;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
Acpi_DrainRxFifo(PI2CCTRL_FDO devctx)
{
    UCHAR dummy;
    NTSTATUS st;

    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Same as QuiesceFifos but without touching TX side */
    while (devctx->AcpiPendingReads > 0) {

        st = I2cCtrl_AcpiTransfer(
                devctx,
                devctx->AcpiTarget,
                NULL,
                0,
                &dummy,
                1);

        if (!NT_SUCCESS(st)) {
            return st;
        }

        devctx->AcpiPendingReads--;
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
Acpi_FlushTxFifo(PI2CCTRL_FDO devctx)
{
    if (devctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * ACPI writes are synchronous and blocking.
     * There is no TX FIFO to flush.
     */
    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
 * Block I/O (implemented via ACPI transfer engine)
 * --------------------------------------------------------------------------- */

static NTSTATUS
Acpi_IssueBlockWrite(
    PI2CCTRL_FDO devctx,
    UCHAR        slaveAddr,
    ULONG        reg,
    PUCHAR       buf,
    ULONG        len,
    PULONG       bytesDone
    )
{
    UCHAR tmp[1];

    tmp[0] = (UCHAR)(reg & 0xFF);

    return I2cCtrl_AcpiTransfer(devctx,
                                slaveAddr,
                                tmp,
                                1,
                                NULL,
                                0) ||
           I2cCtrl_AcpiTransfer(devctx,
                                slaveAddr,
                                buf,
                                len,
                                NULL,
                                0);
}

static NTSTATUS
Acpi_IssueBlockRead(
    PI2CCTRL_FDO devctx,
    UCHAR        slaveAddr,
    ULONG        reg,
    PUCHAR       buf,
    ULONG        len,
    PULONG       bytesDone
    )
{
    UCHAR tmp[1];

    tmp[0] = (UCHAR)(reg & 0xFF);

    return I2cCtrl_AcpiTransfer(devctx,
                                slaveAddr,
                                tmp,
                                1,
                                NULL,
                                0) ||
           I2cCtrl_AcpiTransfer(devctx,
                                slaveAddr,
                                NULL,
                                0,
                                buf,
                                len);
}

/* ---------------------------------------------------------------------------
 * ACPI backend ops table
 * --------------------------------------------------------------------------- */

I2C_HW_OPS AcpiI2cOps = {
    Acpi_MapResources,        /* MapResources */
    Acpi_UnmapResources,      /* UnmapResources */

    Acpi_Enable,              /* Enable */
    Acpi_SetTarget7bit,       /* SetTarget7bit */

    Acpi_SetSpeed,            /* SetSpeed */
    Acpi_SetBusSpeedHz,       /* SetBusSpeedHz */

    Acpi_IssueWriteByte,      /* IssueWriteByte */
    Acpi_IssueReadToken,      /* IssueReadToken */
    Acpi_ReadRxByte,          /* ReadRxByte */
    Acpi_ReadRxByteSafe,      /* ReadRxByteSafe */

    Acpi_PrimeWrite,          /* PrimeWrite */
    Acpi_PrimeReadTokens,     /* PrimeReadTokens */

    Acpi_GetStatus,           /* GetStatus */
    Acpi_AckInterrupts,       /* AckInterrupts */

    Acpi_MaskInterrupts,      /* MaskInterrupts */
    Acpi_UnmaskInterrupts,    /* UnmaskInterrupts */

    NULL,                     /* DrainRxBounded */
    NULL,                     /* FlushTxBounded */

    NULL,                     /* EmitStopIfNeeded */
    NULL,                     /* EmitRestartIfNeeded */

    NULL,                     /* GetRawIntr */
    NULL,                     /* IsArbitrationLost */
    NULL,                     /* WriteTxByte */

    { 0 },                    /* Caps (no HW FIFOs / pure ACPI backend) */

    Acpi_QuiesceFifos,        /* QuiesceFifos */
    Acpi_DrainRxFifo,         /* DrainRxFifo */
    Acpi_FlushTxFifo,         /* FlushTxFifo */

    NULL,                     /* ReadRegister32 */
    NULL,                     /* WriteRegister32 */

    NULL,                     /* QueryLegacyBusInfo */
    NULL,                     /* FilterResourceRequirements */
    NULL,                     /* QueryResourceRequirements */
    NULL,                     /* QueryResources */

    NULL,                     /* ConfigureController */
    NULL,                     /* EmitReadRequest */
    NULL,                     /* ReadTxDiscard */
    NULL,                     /* QueryTouchSample */

    NULL,                     /* EnableWakeSource */
    Acpi_IssueBlockWrite,     /* IssueBlockWrite */
    Acpi_IssueBlockRead       /* IssueBlockRead */
};
