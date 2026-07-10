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

/* ---------------------------------------------------------------------------
 * Initialize HID profile state (XP-safe, C89-compliant)
 * Prepares HID-I2C descriptor and report descriptor lengths.
 * --------------------------------------------------------------------------- */
VOID
I2CCTRL_EMU_HidInitProfile(
    PI2CCTRL_EMU_FDO FdoExt
    )
{
    if (FdoExt == NULL) {
        return;
    }

    /* Reset RX FIFO and interrupt state */
    EmuFifoReset(&FdoExt->RxFifo);
    FdoExt->RawIntr = 0UL;

    /* Cache HID descriptor lengths for ACPI\PNP0C50 child */
    FdoExt->HidDescLength    = I2CCTRL_EMU_HidGetDescriptorLength(FdoExt);
    FdoExt->ReportDescLength = I2CCTRL_EMU_HidGetReportDescriptorLength(FdoExt);

    I2cCtrl_Emu_Log(
        "Init HID profile: HIDDescLen=%u ReportDescLen=%u\n",
        (unsigned)FdoExt->HidDescLength,
        (unsigned)FdoExt->ReportDescLength
    );
}

/* ---------------------------------------------------------------------------
 * Prime RX FIFO according to last register accessed.
 * ACPI PNP0C50 synthetic HID-I2C device:
 * - HID header register: serve 8-byte header (desc length in first 2 bytes)
 * - Report descriptor register: serve full report descriptor
 * - Input register: FIFO is filled via IOCTL_PUSH_REPORT
 * --------------------------------------------------------------------------- */
VOID
I2CCTRL_EMU_HidPrimeForRegister(
    PI2CCTRL_EMU_FDO FdoExt
    )
{
    UCHAR header[8];
    const UCHAR* src;
    ULONG len;

    if (FdoExt == NULL) {
        return;
    }

    /* Only act when target matches the emulated HID address */
    if (FdoExt->Target7bit != FdoExt->HidAddr) {
        return;
    }

    switch (FdoExt->LastReg) {

    case I2CCTRL_EMU_REG_HID_HEADER:
        RtlZeroMemory(header, sizeof(header));

        /* First two bytes: HID descriptor length */
        header[0] = (UCHAR)(FdoExt->HidDescLength & 0xFFU);
        header[1] = (UCHAR)((FdoExt->HidDescLength >> 8) & 0xFFU);

        (VOID)EmuFifoPushBlock(&FdoExt->RxFifo, header, (ULONG)sizeof(header));
        FdoExt->RawIntr |= 0x00000001UL;

        I2cCtrl_Emu_Log("Primed HID header len=%u\n",
                        (unsigned)FdoExt->HidDescLength);
        break;

    case I2CCTRL_EMU_REG_REPORT_DESC:
        src = (const UCHAR*)g_EmuReportDescriptor;
        len = (ULONG)sizeof(g_EmuReportDescriptor);

        (VOID)EmuFifoPushBlock(&FdoExt->RxFifo, src, len);
        FdoExt->RawIntr |= 0x00000001UL;

        I2cCtrl_Emu_Log("Primed report descriptor len=%lu\n", len);
        break;

    default:
        /* INPUT register: data comes from IOCTL_PUSH_REPORT */
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Push full HID-I2C descriptor bytes into RX FIFO (for reads after header)
 * --------------------------------------------------------------------------- */
VOID
I2CCTRL_EMU_HidPrimeFullDescriptor(
    PI2CCTRL_EMU_FDO FdoExt
    )
{
    const UCHAR* src;
    ULONG len;

    if (FdoExt == NULL) {
        return;
    }

    if (FdoExt->Target7bit != FdoExt->HidAddr) {
        return;
    }

    src = (const UCHAR*)&g_EmuHidDescriptor;
    len = (ULONG)sizeof(g_EmuHidDescriptor);

    (VOID)EmuFifoPushBlock(&FdoExt->RxFifo, src, len);
    FdoExt->RawIntr |= 0x00000001UL;

    I2cCtrl_Emu_Log("Primed full HID descriptor len=%lu\n", len);
}

/* ---------------------------------------------------------------------------
 * Utility: return HID-I2C descriptor length (cached in FDO extension)
 * --------------------------------------------------------------------------- */
USHORT
I2CCTRL_EMU_HidGetDescriptorLength(
    PI2CCTRL_EMU_FDO FdoExt
    )
{
    if (FdoExt == NULL) {
        return 0U;
    }
    return FdoExt->HidDescLength;
}

/* ---------------------------------------------------------------------------
 * Utility: return HID-I2C report descriptor length (cached in FDO extension)
 * --------------------------------------------------------------------------- */
USHORT
I2CCTRL_EMU_HidGetReportDescriptorLength(
    PI2CCTRL_EMU_FDO FdoExt
    )
{
    if (FdoExt == NULL) {
        return 0U;
    }
    return FdoExt->ReportDescLength;
}
