/* I2CCTRL_EMU_hid.c
 * HID-over-I2C emulation profile for ASUS X509FA (9DE9) precision touchpad.
 * Provides a HID-I2C v1.0 descriptor and a minimal Precision Touchpad report
 * descriptor, and primes RX data based on the last register accessed.
 * XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant, verbose logs.
 */

#include <ntddk.h>
#include "i2cctrl_emu_ext.h"

/* Minimal Precision Touchpad report descriptor (bytes) */
static const UCHAR g_EmuReportDescriptor[] = {
    /* Usage Page (Digitizer), Usage (TouchPad) */
    0x05, 0x0D,       /* USAGE_PAGE (Digitizer) */
    0x09, 0x05,       /* USAGE (TouchPad) */
    0xA1, 0x01,       /* COLLECTION (Application) */

    /* Pointer collection */
    0x09, 0x22,       /* USAGE (Finger) */
    0xA1, 0x02,       /* COLLECTION (Logical) */
    0x09, 0x42,       /* USAGE (Tip Switch) */
    0x15, 0x00,       /* LOGICAL_MINIMUM (0) */
    0x25, 0x01,       /* LOGICAL_MAXIMUM (1) */
    0x75, 0x01,       /* REPORT_SIZE (1) */
    0x95, 0x01,       /* REPORT_COUNT (1) */
    0x81, 0x02,       /* INPUT (Data,Var,Abs) */

    /* Padding for alignment */
    0x75, 0x07,       /* REPORT_SIZE (7) */
    0x95, 0x01,       /* REPORT_COUNT (1) */
    0x81, 0x03,       /* INPUT (Cnst,Var,Abs) */

    /* X, Y coordinates */
    0x05, 0x01,       /* USAGE_PAGE (Generic Desktop) */
    0x09, 0x30,       /* USAGE (X) */
    0x09, 0x31,       /* USAGE (Y) */
    0x16, 0x00, 0x00, /* LOGICAL_MINIMUM (0) */
    0x26, 0xFF, 0x03, /* LOGICAL_MAXIMUM (1023) */
    0x75, 0x10,       /* REPORT_SIZE (16) */
    0x95, 0x02,       /* REPORT_COUNT (2) */
    0x81, 0x02,       /* INPUT (Data,Var,Abs) */

    0xC0,             /* END_COLLECTION (Finger) */
    0xC0              /* END_COLLECTION (TouchPad) */
};

/* Prebuilt HID-I2C v1.0 descriptor */
static const HID_I2C_DESCRIPTOR_V10 g_EmuHidDescriptor = {
    /* wHIDDescLength           */ (USHORT)sizeof(HID_I2C_DESCRIPTOR_V10),
    /* bcdVersion               */ (USHORT)0x0100,
    /* wReportDescLength        */ (USHORT)sizeof(g_EmuReportDescriptor),
    /* wReportDescRegister      */ (USHORT)I2CCTRL_EMU_REG_REPORT_DESC,
    /* wInputRegister           */ (USHORT)I2CCTRL_EMU_REG_INPUT,
    /* wOutputRegister          */ (USHORT)I2CCTRL_EMU_REG_OUTPUT,
    /* wCommandRegister         */ (USHORT)I2CCTRL_EMU_REG_COMMAND,
    /* wDataRegister            */ (USHORT)I2CCTRL_EMU_REG_DATA,
    /* wVendorID                */ (USHORT)0x0B05, /* ASUS */
    /* wProductID               */ (USHORT)0x9DE9, /* target variant */
    /* wVersionID               */ (USHORT)0x0001
};

/* ---------------------------------------------------------------------------
 * FIFO helpers (local, C89)
 * --------------------------------------------------------------------------- */
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

    /* Buffer size is power of two; use wrap mask */
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
 * Public APIs (call from i2cctrl_emu.c)
 * --------------------------------------------------------------------------- */

/* Initialize HID profile state */
VOID
I2CCTRL_EMU_HidInitProfile(
    PI2CCTRL_EMU_FDO_EXT ext
    )
{
    if (ext == NULL) {
        return;
    }
    /* Ensure clean RX state */
    EmuFifoReset(&ext->RxFifo);
    ext->RawIntr = 0UL;
    I2CCTRL_EMU_LOG("Init profile VID=0x%04X PID=0x%04X Len=%u\n",
        (unsigned)g_EmuHidDescriptor.wVendorID,
        (unsigned)g_EmuHidDescriptor.wProductID,
        (unsigned)g_EmuHidDescriptor.wHIDDescLength);
}

/* Prime RX FIFO according to last register accessed.
 * - For 0x01: first serve 8-byte header (desc length in first 2 bytes),
 *             then serve the full HID descriptor on subsequent reads.
 * - For report descriptor register: serve the report descriptor bytes.
 * - For input register: leave FIFO as-is (filled via IOCTL push).
 */
VOID
I2CCTRL_EMU_HidPrimeForRegister(
    PI2CCTRL_EMU_FDO_EXT ext
    )
{
    UCHAR header[8];
    const UCHAR* src;
    ULONG len;

    if (ext == NULL) {
        return;
    }

    /* Only act when target matches the emulated HID address */
    if (ext->Target7bit != ext->HidAddr) {
        return;
    }

    switch (ext->LastReg) {
    case I2CCTRL_EMU_REG_HID_HEADER:
        /* First 8 bytes: descriptor length at [0..1], rest zero */
        RtlZeroMemory(header, sizeof(header));
        header[0] = (UCHAR)(g_EmuHidDescriptor.wHIDDescLength & 0xFFU);
        header[1] = (UCHAR)((g_EmuHidDescriptor.wHIDDescLength >> 8) & 0xFFU);

        (VOID)EmuFifoPushBlock(&ext->RxFifo, header, (ULONG)sizeof(header));
        ext->RawIntr |= 0x00000001UL;
        I2CCTRL_EMU_LOG("Primed HID header len=%u\n", (unsigned)g_EmuHidDescriptor.wHIDDescLength);
        break;

    case I2CCTRL_EMU_REG_REPORT_DESC:
        src = (const UCHAR*)g_EmuReportDescriptor;
        len = (ULONG)sizeof(g_EmuReportDescriptor);
        (VOID)EmuFifoPushBlock(&ext->RxFifo, src, len);
        ext->RawIntr |= 0x00000001UL;
        I2CCTRL_EMU_LOG("Primed report descriptor len=%lu\n", len);
        break;

    default:
        /* No automatic priming for INPUT/OUTPUT/CMD/DATA.
           INPUT bytes are injected via IOCTL_PUSH_REPORT. */
        break;
    }
}

/* Push full HID-I2C descriptor bytes into RX FIFO (for reads after header) */
VOID
I2CCTRL_EMU_HidPrimeFullDescriptor(
    PI2CCTRL_EMU_FDO_EXT ext
    )
{
    const UCHAR* src;
    ULONG len;

    if (ext == NULL) {
        return;
    }
    if (ext->Target7bit != ext->HidAddr) {
        return;
    }

    src = (const UCHAR*)&g_EmuHidDescriptor;
    len = (ULONG)sizeof(g_EmuHidDescriptor);

    (VOID)EmuFifoPushBlock(&ext->RxFifo, src, len);
    ext->RawIntr |= 0x00000001UL;
    I2CCTRL_EMU_LOG("Primed full HID descriptor len=%lu\n", len);
}

/* Utility: return descriptor length (for consistency with validators) */
USHORT
I2CCTRL_EMU_HidGetDescriptorLength(
    VOID
    )
{
    return g_EmuHidDescriptor.wHIDDescLength;
}

/* Utility: return report descriptor size */
USHORT
I2CCTRL_EMU_HidGetReportDescriptorLength(
    VOID
    )
{
    return (USHORT)sizeof(g_EmuReportDescriptor);
}
