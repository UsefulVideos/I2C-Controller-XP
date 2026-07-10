/* I2CCTRL_EMU_ioctl.c
 * IOCTL interface for I2C controller emulator (ASUS X509FA 9DE9 profile).
 * Allows user-mode to inject input reports and reset emulator state.
 * XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant, verbose logs.
 */

#include <ntddk.h>
#include "i2cctrl_emu_ext.h"

/* FIFO helpers (local, C89) */
static VOID
EmuFifoReset(
    PI2CCTRL_EMU_FIFO q
    )
{
    if (q != NULL) {
        q->Head = 0UL;
        q->Tail = 0UL;
        RtlZeroMemory(q->Data, sizeof(q->Data));
    }
}

static BOOLEAN
EmuFifoPush(
    PI2CCTRL_EMU_FIFO q,
    UCHAR b
    )
{
    ULONG mask;
    ULONG next;

    if (q == NULL) {
        return FALSE;
    }
    mask = (ULONG)(sizeof(q->Data) - 1UL);
    next = (q->Head + 1UL) & mask;
    if (next == q->Tail) {
        return FALSE; /* full */
    }
    q->Data[q->Head] = b;
    q->Head = next;
    return TRUE;
}

static BOOLEAN
EmuFifoPushBlock(
    PI2CCTRL_EMU_FIFO q,
    const UCHAR* data,
    ULONG len
    )
{
    ULONG i;
    if (q == NULL || data == NULL) {
        return FALSE;
    }
    for (i = 0UL; i < len; i++) {
        if (!EmuFifoPush(q, data[i])) {
            return FALSE;
        }
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * Public dispatch routine for IOCTLs (to be called from i2cctrl_emu.c)
 * ACPI PNP0C50 friendly, XP-safe, C89-compliant
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_IoctlDispatchBuffered(
    PI2CCTRL_EMU_FDO FdoExt,
    ULONG IoctlCode,
    PUCHAR inBuf,
    ULONG inLen
    )
{
    NTSTATUS status;
    ULONG i;

    if (FdoExt == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = STATUS_INVALID_DEVICE_REQUEST;

    switch (IoctlCode) {

    case IOCTL_I2CCTRL_EMU_PUSH_REPORT:
        if (inBuf != NULL && inLen > 0UL) {

            /* Push bytes into emulator RX FIFO */
            for (i = 0UL; i < inLen; i++) {
                (VOID)EmuFifoPush(&FdoExt->RxFifo, inBuf[i]);
            }

            /* Signal data available */
            FdoExt->RawIntr |= 0x00000001UL;

            status = STATUS_SUCCESS;
            I2cCtrl_Emu_Log("PUSH_REPORT queued %lu bytes\n", inLen);

        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_I2CCTRL_EMU_RESET:
        EmuFifoReset(&FdoExt->RxFifo);
        FdoExt->RawIntr = 0UL;
        status = STATUS_SUCCESS;
        I2cCtrl_Emu_Log("RESET\n");
        break;

    default:
        /* No other IOCTLs are supported in ACPI PNP0C50 mode */
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    return status;
}
