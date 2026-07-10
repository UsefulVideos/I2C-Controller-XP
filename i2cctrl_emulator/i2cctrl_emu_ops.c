/* I2CCTRL_EMU_ops.c
 * HAL-facing ops table for I2C controller emulator (ASUS X509FA 9DE9 profile).
 * Implements Enable, SetTarget7bit, IssueWriteByte, IssueReadToken, GetStatus,
 * ReadRxByte, and AckInterrupts. XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant.
 */

#include <ntddk.h>
#include "..\i2cctrl\i2cctrl_ext.h" /* PI2CCTRL_FDO, I2C_HW_STATUS */
#include "i2cctrl_emu_ext.h"

/* FIFO helpers */
static BOOLEAN EmuFifoPop(PI2CCTRL_EMU_FIFO q, PUCHAR b)
{
    if (q == NULL || b == NULL) return FALSE;
    if (q->Head == q->Tail) return FALSE;
    *b = q->Data[q->Tail];
    q->Tail = (q->Tail + 1UL) & (sizeof(q->Data) - 1UL);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * Ops: Enable
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_Enable(
    PI2CCTRL_FDO fdo,
    BOOLEAN on
    )
{
    PI2CCTRL_EMU_FDO_EXT ext = (PI2CCTRL_EMU_FDO_EXT)fdo;
    ext->Enabled = on ? TRUE : FALSE;
    I2cCtrl_Emu_Log("Enable=%u\n", (unsigned)ext->Enabled);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Ops: SetTarget7bit
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_SetTarget7bit(
    PI2CCTRL_FDO fdo,
    UCHAR addr
    )
{
    PI2CCTRL_EMU_FDO_EXT ext = (PI2CCTRL_EMU_FDO_EXT)fdo;
    ext->Target7bit = addr & 0x7FU;
    I2cCtrl_Emu_Log("SetTarget7bit=0x%02X\n", (unsigned)ext->Target7bit);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Ops: IssueWriteByte (latch register)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_IssueWriteByte(
    PI2CCTRL_FDO fdo,
    UCHAR reg
    )
{
    PI2CCTRL_EMU_FDO_EXT ext = (PI2CCTRL_EMU_FDO_EXT)fdo;
    ext->LastReg = reg;
    I2cCtrl_Emu_Log("IssueWriteByte reg=0x%02X\n", (unsigned)reg);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Ops: IssueReadToken (mark RX pending)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_IssueReadToken(
    PI2CCTRL_FDO fdo
    )
{
    PI2CCTRL_EMU_FDO_EXT ext = (PI2CCTRL_EMU_FDO_EXT)fdo;
    ext->RawIntr |= 0x00000001UL;
    I2cCtrl_Emu_Log("IssueReadToken\n");
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Ops: GetStatus (report RX FIFO state)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_GetStatus(
    PI2CCTRL_FDO fdo,
    PI2C_HW_STATUS st
    )
{
    PI2CCTRL_EMU_FDO_EXT ext = (PI2CCTRL_EMU_FDO_EXT)fdo;
    if (st == NULL) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(st, sizeof(*st));
    st->RawIntr = ext->RawIntr;
    st->RxFifoNotEmpty = (ext->RxFifo.Head != ext->RxFifo.Tail) ? TRUE : FALSE;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Ops: ReadRxByte (pop from FIFO or synthesize)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_ReadRxByte(
    PI2CCTRL_FDO fdo,
    PUCHAR out
    )
{
    PI2CCTRL_EMU_FDO_EXT ext;
    UCHAR b;

    ext = (PI2CCTRL_EMU_FDO_EXT)fdo;
    if (out == NULL) return STATUS_INVALID_PARAMETER;

    if (EmuFifoPop(&ext->RxFifo, &b)) {
        *out = b;
        return STATUS_SUCCESS;
    }

    /* Default filler if FIFO empty */
    *out = 0x00U;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Ops: AckInterrupts
 * --------------------------------------------------------------------------- */
VOID
I2CCTRL_EMU_AckInterrupts(
    PI2CCTRL_FDO fdo,
    ULONG rawIntr
    )
{
    PI2CCTRL_EMU_FDO_EXT ext = (PI2CCTRL_EMU_FDO_EXT)fdo;
    ext->RawIntr &= ~rawIntr;
    I2cCtrl_Emu_Log("AckInterrupts mask=0x%08lX\n", rawIntr);
}
