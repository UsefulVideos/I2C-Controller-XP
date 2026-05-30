// i2cctrl_detect.c (after improvements)
#include <ntddk.h>
#include "i2cctrl_ext.h"      // defines I2CCTRL_FDO
#include "i2cctrl_bsod.h"
#include "i2cctrl_detect.h"

/* ---------------------------------------------------------------------------
   Read HID register from a device at given I²C address (generic, HAL-based, XP-safe)
   --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_ReadHidRegister(
    PI2CCTRL_FDO fdoExt,
    UCHAR        addr,
    UCHAR        reg,
    PUCHAR       buffer,
    ULONG        length,
    ULONG        timeoutUs
    )
{
    ULONG         i;
    ULONG         remaining;
    NTSTATUS      st;
    I2C_HW_STATUS hwst;

    /* Defensive init */
    i         = 0U;
    remaining = 0U;
    st        = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (fdoExt == NULL || buffer == NULL || length == 0U) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ReadHidRegister invalid parameters (fdoExt=%p, buffer=%p, length=%lu)",
                    fdoExt, buffer, length);
        return STATUS_INVALID_PARAMETER;
    }

    /* Enable controller */
    if (fdoExt->Ops && fdoExt->Ops->Enable) {
        (VOID)fdoExt->Ops->Enable(fdoExt, TRUE);
    }

    /* Set target address */
    if (fdoExt->Ops && fdoExt->Ops->SetTarget7bit) {
        st = fdoExt->Ops->SetTarget7bit(fdoExt, addr);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }

    /* Write register offset (the HID register we want to read) */
    if (fdoExt->Ops && fdoExt->Ops->IssueWriteByte) {
        st = fdoExt->Ops->IssueWriteByte(fdoExt, reg);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }

    /* Queue read commands */
    for (i = 0U; i < length; i++) {
        if (fdoExt->Ops && fdoExt->Ops->IssueReadToken) {
            st = fdoExt->Ops->IssueReadToken(fdoExt);
            if (!NT_SUCCESS(st)) {
                return st;
            }
        }
    }

    /* Read back data with timeout */
    for (i = 0U; i < length; i++) {
        remaining = timeoutUs;
        while (TRUE) {
            if (fdoExt->Ops && fdoExt->Ops->GetStatus) {
                st = fdoExt->Ops->GetStatus(fdoExt, &hwst);
                if (!NT_SUCCESS(st)) {
                    return st;
                }
                if (hwst.RxFifoNotEmpty) {
                    break;
                }
            }
            if (remaining == 0U) {
                if (fdoExt->Ops && fdoExt->Ops->AckInterrupts) {
                    fdoExt->Ops->AckInterrupts(fdoExt, hwst.RawIntr);
                }
                TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_BUS,
                            "Timeout waiting for data at addr=0x%02X reg=0x%02X",
                            addr, reg);
                return STATUS_IO_TIMEOUT;
            }
            KeStallExecutionProcessor(1U);
            if (remaining > 0U) {
                remaining--;
            }
        }
        if (fdoExt->Ops && fdoExt->Ops->ReadRxByte) {
            st = fdoExt->Ops->ReadRxByte(fdoExt, &buffer[i]);
            if (!NT_SUCCESS(st)) {
                return st;
            }
        }
    }

    /* Acknowledge any pending interrupts */
    if (fdoExt->Ops && fdoExt->Ops->AckInterrupts) {
        fdoExt->Ops->AckInterrupts(fdoExt, hwst.RawIntr);
    }

    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   Read fixed-size HID descriptor structure from the HID descriptor register
   (HAL-based, controller-agnostic)
   --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_ReadHidDescriptor(
    PI2CCTRL_FDO     devctx,
    PI2CCTRL_PDO hidpdo,
    PHID_DESCRIPTOR  outDesc
    )
{
    NTSTATUS status = STATUS_SUCCESS;

    /* Defensive parameter validation */
    if (devctx == NULL || hidpdo == NULL || outDesc == NULL) {
        KdPrint(("I2CCTRL: ReadHidDescriptor invalid parameters devctx=%p hidpdo=%p outDesc=%p\n",
                 devctx, hidpdo, outDesc));
        return STATUS_INVALID_PARAMETER;
    }

    /* Perform a generic block read of the HID descriptor via HAL-backed I²C */
    status = I2cCtrl_ReadBlock(
                 devctx,
                 hidpdo->SlaveAddress,
                 hidpdo->HidDescRegister,
                 (PUCHAR)outDesc,
                 sizeof(HID_DESCRIPTOR));

    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL: ReadHidDescriptor failed Slave=0x%04X Reg=0x%lx Status=0x%08lx\n",
                 hidpdo->SlaveAddress,
                 hidpdo->HidDescRegister,
                 status));
    }

    return status;
}


/* ---------------------------------------------------------------------------
   Safe HID-over-I²C report descriptor reader
   - Validates length against HID descriptor
   - Prevents overflow into ACPI-owned pool blocks
   --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_ReadReportDescriptor(
    PI2CCTRL_FDO     devctx,
    PI2CCTRL_PDO hidpdo,
    PUCHAR           buf,
    ULONG            len
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    USHORT expectedLen = 0;   /* <-- declare here (C89-safe) */

    /* Defensive parameter validation */
    if (!devctx || !hidpdo || !buf || len == 0) {
        KdPrint(("I2CCTRL: ReadReportDescriptor invalid args devctx=%p hidpdo=%p buf=%p len=%lu\n",
                 devctx, hidpdo, buf, len));
        return STATUS_INVALID_PARAMETER;
    }

    /* Validate HID descriptor fields before trusting len */
    if (hidpdo->HidDesc.bNumDescriptors < 1 ||
        hidpdo->HidDesc.DescriptorList[0].bReportType != HID_REPORT_DESCRIPTOR_TYPE)
    {
        KdPrint(("I2CCTRL: ReadReportDescriptor invalid HID descriptor (no report desc)\n"));
        return STATUS_INVALID_DEVICE_STATE;
    }

    expectedLen = hidpdo->HidDesc.DescriptorList[0].wReportLength;

    if (expectedLen == 0 || expectedLen > HID_REPORT_MAX_LEN) {
        KdPrint(("I2CCTRL: ReadReportDescriptor invalid expectedLen=%u\n", expectedLen));
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (len < expectedLen) {
        KdPrint(("I2CCTRL: ReadReportDescriptor buffer too small len=%lu needed=%u\n",
                 len, expectedLen));
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Issue GET_REPORT_DESCRIPTOR command */
    if (devctx->Ops && devctx->Ops->IssueWriteByte) {
        status = devctx->Ops->IssueWriteByte(devctx, HID_I2C_GET_REPORT_DESCRIPTOR);
        if (!NT_SUCCESS(status)) {
            KdPrint(("I2CCTRL: GET_REPORT_DESCRIPTOR command failed 0x%08lx\n", status));
            return status;
        }
    }

    /* Read exactly expectedLen bytes */
    status = I2cCtrl_ReadBlock(
                 devctx,
                 hidpdo->SlaveAddress,
                 hidpdo->DataRegister,
                 buf,
                 expectedLen);

    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL: ReadReportDescriptor failed Slave=0x%02X Reg=0x%X Status=0x%08lx\n",
                 hidpdo->SlaveAddress,
                 hidpdo->DataRegister,
                 status));
        return status;
    }

    return STATUS_SUCCESS;
}


// ---------------------------------------------------------------------------
// Validate HID descriptor bytes and parse fields (HID-I²C v1.0, HAL-neutral,
// XP/2003-safe, hardened, ETW logging, modern report length cap)
// ---------------------------------------------------------------------------
BOOLEAN
ParseHidDescriptorV10(
    const UCHAR*              buf,
    ULONG                     len,
    PHID_I2C_DESCRIPTOR_V10   out
    )
{
    HID_I2C_DESCRIPTOR_V10 temp;

    /* Defensive init */
    if (out != NULL) {
        RtlZeroMemory(out, sizeof(HID_I2C_DESCRIPTOR_V10));
    }

    /* Basic sanity checks */
    if (buf == NULL || out == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ParseHidDescriptorV10 invalid parameters buf=%p out=%p len=%lu",
                    buf, out, len);
        return FALSE;
    }
    if (len < sizeof(HID_I2C_DESCRIPTOR_V10)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ParseHidDescriptorV10 buffer too small len=%lu", len);
        return FALSE;
    }

    /* Copy raw bytes into temporary structure */
    RtlZeroMemory(&temp, sizeof(HID_I2C_DESCRIPTOR_V10));
    RtlCopyMemory(&temp, buf, sizeof(HID_I2C_DESCRIPTOR_V10));

    /* Validate descriptor length field */
    if (temp.wHIDDescLength < sizeof(HID_I2C_DESCRIPTOR_V10) ||
        temp.wHIDDescLength > len || temp.wHIDDescLength > 256U) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ParseHidDescriptorV10 invalid HIDDescLength=%u (buffer len=%lu)",
                    temp.wHIDDescLength, len);
        return FALSE;
    }

    /* Validate report descriptor info with modern cap */
    if (temp.wReportDescLength == 0U ||
        temp.wReportDescLength > HID_REPORT_MAX_LEN ||   /* enforce 1024U cap */
        temp.wReportDescRegister == 0U) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ParseHidDescriptorV10 invalid report descriptor fields Length=%u (max=%u) Register=%u",
                    temp.wReportDescLength,
                    HID_REPORT_MAX_LEN,
                    temp.wReportDescRegister);
        return FALSE;
    }

    /* Validate mandatory register offsets */
    if (temp.wInputRegister   == 0U ||
        temp.wOutputRegister  == 0U ||
        temp.wCommandRegister == 0U ||
        temp.wDataRegister    == 0U) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ParseHidDescriptorV10 mandatory register offset missing In=%u Out=%u Cmd=%u Data=%u",
                    temp.wInputRegister,
                    temp.wOutputRegister,
                    temp.wCommandRegister,
                    temp.wDataRegister);
        return FALSE;
    }

    /* Additional sanity checks */
    if (temp.wMaxInputLength == 0U || temp.wMaxOutputLength == 0U) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ParseHidDescriptorV10 invalid max lengths InLen=%u OutLen=%u",
                    temp.wMaxInputLength,
                    temp.wMaxOutputLength);
        return FALSE;
    }

    /* Copy validated structure back to caller */
    *out = temp;

    /* Success trace */
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_BUS,
                "ParseHidDescriptorV10 success HIDDescLength=%u ReportLen=%u VID=0x%04X PID=0x%04X Ver=0x%04X",
                temp.wHIDDescLength,
                temp.wReportDescLength,
                temp.wVendorID,
                temp.wProductID,
                temp.wVersionID);

    return TRUE;
}


// ---------------------------------------------------------------------------
// Read and validate full HID descriptor from a device at given I²C address
// (HAL-based, controller-agnostic, XP/2003-safe, with retries, ETW logging,
//  and strict bounds including modern HID_REPORT_MAX_LEN enforcement via parser)
// ---------------------------------------------------------------------------
NTSTATUS
I2cCtrl_ReadAndValidateHidDescriptor(
    PI2CCTRL_FDO              devctx,
    UCHAR                     addr,
    PUCHAR                    outBuf,
    ULONG                     outBufLen,
    PHID_I2C_DESCRIPTOR_V10   parsed
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    UCHAR    hdr[sizeof(USHORT) * 4]; /* header is 8 bytes (little-endian) */
    HID_I2C_DESCRIPTOR_V10 temp;
    USHORT   descLen = 0U;
    int      tries   = 0;

    RtlZeroMemory(&temp, sizeof(HID_I2C_DESCRIPTOR_V10));
    if (outBuf && outBufLen) {
        RtlZeroMemory(outBuf, outBufLen);
    }

    /* Basic parameter validation */
    if (devctx == NULL || outBuf == NULL || outBufLen < sizeof(HID_I2C_DESCRIPTOR_V10)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ReadHidDesc: invalid params devctx=%p outBuf=%p outBufLen=%lu addr=0x%02X",
                    devctx, outBuf, outBufLen, addr);
        return STATUS_INVALID_PARAMETER;
    }

    /* Step 1: read first 8 bytes (header contains descriptor length) with retries */
    for (tries = 0; tries < 3; tries++) {
        status = I2cCtrl_ReadHidRegister(devctx, addr, 0x01U, hdr, sizeof(hdr), 2000U);
        if (NT_SUCCESS(status)) break;
        KeStallExecutionProcessor(1000); /* 1ms backoff */
    }
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ReadHidDesc: header read failed addr=0x%02X status=0x%08lx",
                    addr, status);
        return status;
    }

    /* HID over I2C: first two bytes are wHIDDescLength (LE) */
    descLen = (USHORT)(hdr[0] | ((USHORT)hdr[1] << 8));

    /* Validate descriptor length field (spec + implementation cap) */
    if (descLen < sizeof(HID_I2C_DESCRIPTOR_V10) || descLen > 256U) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ReadHidDesc: invalid HID descriptor length=%u addr=0x%02X",
                    (ULONG)descLen, addr);
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if ((ULONG)descLen > outBufLen) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ReadHidDesc: descLen=%u exceeds caller buffer len=%lu",
                    (ULONG)descLen, outBufLen);
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Step 2: read full descriptor with retries */
    for (tries = 0; tries < 3; tries++) {
        status = I2cCtrl_ReadHidRegister(devctx, addr, 0x01U, outBuf, descLen, 4000U);
        if (NT_SUCCESS(status)) break;
        KeStallExecutionProcessor(2000); /* 2ms backoff */
    }
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ReadHidDesc: full descriptor read failed addr=0x%02X status=0x%08lx",
                    addr, status);
        return status;
    }

    /* Step 3: validate and parse into temporary structure
     * ParseHidDescriptorV10 enforces HID_REPORT_MAX_LEN (e.g., 1024U) cap. */
    if (!ParseHidDescriptorV10(outBuf, descLen, &temp)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ReadHidDesc: descriptor validation failed addr=0x%02X len=%u",
                    addr, (ULONG)descLen);
        return STATUS_INVALID_PARAMETER;
    }

    /* Optional: warn if device advertises max input/output > our report cap */
    if (temp.wMaxInputLength  > HID_REPORT_MAX_LEN ||
        temp.wMaxOutputLength > HID_REPORT_MAX_LEN) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_BUS,
                    "ReadHidDesc: device max lengths In=%u Out=%u exceed cap=%u (will truncate)",
                    temp.wMaxInputLength, temp.wMaxOutputLength, HID_REPORT_MAX_LEN);
    }

    /* Copy parsed descriptor back to caller if requested */
    if (parsed != NULL) {
        *parsed = temp;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_BUS,
                "ReadHidDesc: success addr=0x%02X len=%u ReportLen=%u VID=0x%04X PID=0x%04X Ver=0x%04X",
                addr,
                (ULONG)descLen,
                (ULONG)temp.wReportDescLength,
                (ULONG)temp.wVendorID,
                (ULONG)temp.wProductID,
                (ULONG)temp.wVersionID);

    return STATUS_SUCCESS;
}


// ---------------------------------------------------------------------------
// Common I²C address candidates for HID-over-I²C devices (controller-agnostic list)
// ---------------------------------------------------------------------------
static const UCHAR g_HidI2cCommonCandidates[] = {
    0x15,             /* typical HID touch controller */
    0x2C, 0x2D,       /* common HID addresses */
    0x24, 0x25,       /* alternate HID addresses */
    0x20, 0x21, 0x22, 0x23, /* mid-range HID addresses */
    0x1C, 0x1D, 0x1E, 0x1F, /* lower HID addresses */
    0x5C, 0x5D        /* additional HID candidates seen in newer devices */
};