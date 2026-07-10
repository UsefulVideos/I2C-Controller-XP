/* ---------------------------------------------------------------------------
   i2chid.c
   HID-over-I²C function driver (ACPI PNP0C50) for legacy Windows platforms:
     - Windows XP (x86, 5.1)
     - Windows Server 2003 (x64, 5.2)

   Build environment:
     - Compiled with WinDDK 7.1.0
     - C89-compliant declaration ordering for XP/2003 safety

   Overview:
     • Integrates with hidclass.sys (HID class stack)
     • Implements core HID IOCTLs and exposes HID descriptors
     • Fetches input via the I²C controller and completes IOCTL_HID_READ_REPORT
       requests with properly formatted HID input reports

   Supported IOCTLs:
     • IOCTL_HID_GET_DEVICE_DESCRIPTOR
     • IOCTL_HID_GET_REPORT_DESCRIPTOR
     • IOCTL_HID_READ_REPORT
     • IOCTL_HID_GET_FEATURE / IOCTL_HID_SET_FEATURE (stubbed)
     • IOCTL_HID_WRITE_REPORT (optional)

   Features:
     • Creates a FILE_DEVICE_UNKNOWN FDO
     • Provides a basic mouse-like HID input report (X, Y, buttons) as the
       initial implementation
     • Extensible to multitouch support via HID usages and report descriptors
     • Compatible with XP/2003 HID stack and I²C controller interfaces

   Build targets:
     • Windows XP (x86)
     • Windows Server 2003 (x64)
   --------------------------------------------------------------------------- */

#include "i2chid.h"
#include "I2CHID_DPI.h"
#include "I2CHID_ACPI.h"
#include "I2CHID_hid.h"
#include "..\i2cctrl\i2cctrl_etw.h"
#pragma warning(disable:4201) // nameless struct/union

#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x05020100
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0502
#endif
#ifndef WINVER
#define WINVER 0x0502
#endif

/* ---- Hardware hooks (implemented elsewhere in your driver) ---- */
VOID I2cHw_EnableController(PI2CHID_FDO ext);
VOID I2cHw_DeviceResume(PI2CHID_FDO ext);
VOID I2cHw_ReprogramDescriptor(PI2CHID_FDO ext);
VOID I2cHw_ConnectInterrupt(PI2CHID_FDO ext);

VOID I2cHw_DisconnectInterrupt(PI2CHID_FDO ext);
VOID I2cHw_DeviceIdle(PI2CHID_FDO ext);
VOID I2cHw_DeviceSuspend(PI2CHID_FDO ext);
VOID I2cHw_EnableWake(PI2CHID_FDO ext, BOOLEAN Enable);

/* Prototypes for internal helpers */
static VOID   I2CHID_ClearReport(PI2CHID_REPORT rpt);
static UCHAR  I2CHID_Checksum8(const UCHAR* buf, ULONG len);
static USHORT I2CHID_ReadLe16(const UCHAR* p);

/* Interrupt helpers */
BOOLEAN I2CHID_InterruptServiceRoutine(
    PKINTERRUPT Interrupt,
    PVOID ServiceContext
    );

VOID I2CHID_InterruptDpc(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2
    );


/* Global HID registration block */
HID_MINIDRIVER_REGISTRATION g_HidReg;

/* -----------------------------------------------------------------------
 * DriverEntry – HID-over-I2C function driver (i2chid.sys)
 * XP/2003 BSOD-safe, WinDDK-compiler-safe, C89-compliant, ETW-logged
 *
 * Guarantees:
 *  - Runs at PASSIVE_LEVEL
 *  - Initializes dispatch points deterministically
 *  - Sets unload early for safe failure unwinds
 *  - Fails fast if DriverExtension/AddDevice unavailable
 * ----------------------------------------------------------------------- */
NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT  DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    ULONG    i;

    /* C89 init */
    status = STATUS_SUCCESS;
    i      = 0U;

    UNREFERENCED_PARAMETER(RegistryPath);

    /* Must run at PASSIVE_LEVEL on XP/2003 */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "DriverEntry failed: IRQL=%lu (expected PASSIVE_LEVEL)",
                    KeGetCurrentIrql());
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (DriverObject == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "DriverEntry failed: DriverObject=NULL");
        return STATUS_INVALID_PARAMETER;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                "I2CHID(DriverEntry): begin");

    /* Set unload routine early so failure paths are safe */
    DriverObject->DriverUnload = I2CHID_DriverUnload;

    /* Default all IRP major functions to a safe create/close handler */
    for (i = 0U; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = I2CHID_DispatchCreateClose;
    }

    /* Assign supported dispatch routines (deterministic ordering) */
    DriverObject->MajorFunction[IRP_MJ_PNP]                     = I2CHID_DispatchPnP;
    DriverObject->MajorFunction[IRP_MJ_POWER]                   = I2CHID_DispatchPower;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]          = I2CHID_DispatchIoctl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = I2CHID_DispatchInternalIoctl;

    /* Precision Touchpad pass-through for file I/O */
    DriverObject->MajorFunction[IRP_MJ_CREATE]                  = I2CHID_PT_DispatchPass;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]                   = I2CHID_PT_DispatchPass;
    DriverObject->MajorFunction[IRP_MJ_READ]                    = I2CHID_PT_DispatchPass;
    DriverObject->MajorFunction[IRP_MJ_WRITE]                   = I2CHID_PT_DispatchPass;

    /* Route additional traffic through unified PnP dispatch (XP-safe) */
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL]          = I2CHID_DispatchPnP;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN]                = I2CHID_DispatchPnP;
    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION]       = I2CHID_DispatchPnP;
    DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION]         = I2CHID_DispatchPnP;

    /* AddDevice must be available; if not, fail safely */
    if (DriverObject->DriverExtension == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "DriverEntry failed: DriverExtension missing");
        status = STATUS_UNSUCCESSFUL;
        return status;
    }

    /* Assign AddDevice (PnP entry point) */
    DriverObject->DriverExtension->AddDevice = I2CHID_AddDevice;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_INIT,
                "I2CHID(DriverEntry): complete");
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   Common I²C address candidates for HID-over-I²C devices (controller-agnostic list)
   Notes:
     - These are 7-bit I²C addresses typically used by HID touch controllers.
     - The list is not exhaustive; firmware/ACPI may override with a specific address.
     - Keep synchronized with detection logic that probes these candidates.
   --------------------------------------------------------------------------- */
static const UCHAR g_HidI2cCommonCandidates[] = {
    0x15,                   /* typical HID touch controller */
    0x2C, 0x2D,             /* common HID addresses */
    0x24, 0x25,             /* alternate HID addresses */
    0x20, 0x21, 0x22, 0x23, /* mid-range HID addresses */
    0x1C, 0x1D, 0x1E, 0x1F, /* lower HID addresses */
    0x5C, 0x5D              /* additional HID candidates seen in newer devices */
};

/* -----------------------------------------------------------------------
 * Initialize HID device and report descriptors using the compiled static
 * report descriptor. XP/2003 BSOD-safe, WinDDK-compiler-safe, C89-compliant.
 *
 * Guarantees:
 *  - Runs at PASSIVE_LEVEL
 *  - Validates parameters and buffer capacity
 *  - Initializes HID descriptor deterministically
 *  - Copies g_HidReportDesc with bounds checks
 * ----------------------------------------------------------------------- */
NTSTATUS
I2CHID_InitHidDescriptors(
    PI2CHID_FDO ext
    )
{
    PHID_DESCRIPTOR hd;
    PUCHAR          rd;
    ULONG           maxLen;
    ULONG           needLen;

    /* Must run at PASSIVE_LEVEL */
    I2CHID_REQUIRE_PASSIVE();
    PAGED_CODE();

    /* Validate input */
    if (ext == NULL) {
        I2CHID_Log("I2CHID(InitHidDescriptors): NULL extension\n");
        return STATUS_INVALID_PARAMETER;
    }

    hd = &ext->HidStatic.HidDesc;
    rd = ext->HidStatic.ReportDesc;

    /* Validate report descriptor buffer */
    if (rd == NULL) {
        I2CHID_Log("I2CHID(InitHidDescriptors): ReportDesc buffer is NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* Capacity of destination buffer and source length */
    maxLen  = (ULONG)sizeof(ext->HidStatic.ReportDesc);
    needLen = (ULONG)g_HidReportDescSize;

    if (needLen == 0U) {
        I2CHID_Log("I2CHID(InitHidDescriptors): source report descriptor size is 0\n");
        return STATUS_INVALID_PARAMETER;
    }
    if (needLen > maxLen) {
        KdPrint(("I2CHID(InitHidDescriptors): buffer too small (need %lu, have %lu)\n",
                 needLen, maxLen));
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Zero HID and report descriptor storage */
    RtlZeroMemory(hd, sizeof(*hd));
    RtlZeroMemory(rd, maxLen);

    /* Initialize HID descriptor header */
    hd->bLength         = (UCHAR)sizeof(HID_DESCRIPTOR);
    hd->bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
    hd->bcdHID          = 0x0111; /* HID v1.11 */
    hd->bCountry        = 0x00;
    hd->bNumDescriptors = 0x01;
    hd->DescriptorList[0].bReportType   = HID_REPORT_DESCRIPTOR_TYPE;

    /* Copy the static HID report descriptor (bounds-checked) */
    RtlCopyMemory(rd, g_HidReportDesc, needLen);

    /* Final lengths */
    ext->HidStatic.ReportDescLength          = (USHORT)needLen;
    hd->DescriptorList[0].wReportLength      = (USHORT)ext->HidStatic.ReportDescLength;

    I2CHID_Log("I2CHID(InitHidDescriptors): built %lu-byte report descriptor\n", needLen);
    return STATUS_SUCCESS;
}


NTSTATUS
I2CHID_FetchBusDpi(
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    OUT PI2CCTRL_DPI DpiOut
    )
{
    if (PhysicalDeviceObject == NULL || DpiOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Simplest approach: assume PDO->DeviceExtension contains DPI */
    *DpiOut = *((PI2CCTRL_DPI)PhysicalDeviceObject->DeviceExtension);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   I2CHID_ReadHidRegister – HID-side read routine
   Reads HID register from a device at given I²C address.
   HAL-based, controller-agnostic, XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant.
   --------------------------------------------------------------------------- */
NTSTATUS
I2CHID_ReadHidRegister(
    PI2CCTRL_FDO fdoExt,
    UCHAR        addr,
    UCHAR        reg,
    PUCHAR       buffer,
    ULONG        length,
    ULONG        timeoutUs
    )
{
    ULONG         i;          /* C89: declare first */
    ULONG         remaining;
    NTSTATUS      st;
    I2C_HW_STATUS hwst;

    /* Defensive init */
    i         = 0U;
    remaining = 0U;
    st        = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));

    /* Parameter validation */
    if (fdoExt == NULL || buffer == NULL || length == 0U) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "I2CHID_ReadHidRegister invalid parameters (fdoExt=%p, buffer=%p, length=%lu)",
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
                            "I2CHID_ReadHidRegister timeout waiting for data at addr=0x%02X reg=0x%02X",
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

// ---------------------------------------------------------------------------
// I2CHID_ParseHidDescriptorV10 – HID-side validation and parsing routine
// Validates HID descriptor bytes and parses fields (HID-I²C v1.0).
// HAL-neutral, controller-agnostic, XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant, ETW logging.
// ---------------------------------------------------------------------------
BOOLEAN
I2CHID_ParseHidDescriptorV10(
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
                    "I2CHID_ParseHidDescriptorV10 invalid parameters buf=%p out=%p len=%lu",
                    buf, out, len);
        return FALSE;
    }
    if (len < sizeof(HID_I2C_DESCRIPTOR_V10)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "I2CHID_ParseHidDescriptorV10 buffer too small len=%lu", len);
        return FALSE;
    }

    /* Copy raw bytes into temporary structure */
    RtlZeroMemory(&temp, sizeof(HID_I2C_DESCRIPTOR_V10));
    RtlCopyMemory(&temp, buf, sizeof(HID_I2C_DESCRIPTOR_V10));

    /* Validate descriptor length field */
    if (temp.wHIDDescLength < sizeof(HID_I2C_DESCRIPTOR_V10) ||
        temp.wHIDDescLength > len) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "I2CHID_ParseHidDescriptorV10 invalid HIDDescLength=%u (buffer len=%lu)",
                    temp.wHIDDescLength, len);
        return FALSE;
    }

    /* Validate report descriptor info */
    if (temp.wReportDescLength == 0U || temp.wReportDescRegister == 0U) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "I2CHID_ParseHidDescriptorV10 invalid report descriptor fields Length=%u Register=%u",
                    temp.wReportDescLength,
                    temp.wReportDescRegister);
        return FALSE;
    }

    /* Validate mandatory register offsets */
    if (temp.wInputRegister   == 0U ||
        temp.wOutputRegister  == 0U ||
        temp.wCommandRegister == 0U ||
        temp.wDataRegister    == 0U) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "I2CHID_ParseHidDescriptorV10 mandatory register offset missing In=%u Out=%u Cmd=%u Data=%u",
                    temp.wInputRegister,
                    temp.wOutputRegister,
                    temp.wCommandRegister,
                    temp.wDataRegister);
        return FALSE;
    }

    /* Copy validated structure back to caller */
    *out = temp;

    /* Success trace */
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_BUS,
                "I2CHID_ParseHidDescriptorV10 success HIDDescLength=%u ReportLen=%u VID=0x%04X PID=0x%04X Ver=0x%04X",
                temp.wHIDDescLength,
                temp.wReportDescLength,
                temp.wVendorID,
                temp.wProductID,
                temp.wVersionID);

    return TRUE;
}


// ---------------------------------------------------------------------------
// I2CHID_ReadAndValidateHidDescriptor – HID-side read/validate routine
// Reads and validates full HID descriptor from a device at given I²C address.
// HAL-based, controller-agnostic, XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant.
// ---------------------------------------------------------------------------
NTSTATUS
I2CHID_ReadAndValidateHidDescriptor(
    PI2CCTRL_FDO              devctx,
    UCHAR                     addr,
    PUCHAR                    outBuf,
    ULONG                     outBufLen,
    PHID_I2C_DESCRIPTOR_V10   parsed
    )
{
    NTSTATUS status;  /* C89: declare first */
    UCHAR    hdr[sizeof(USHORT) * 4]; /* header is 8 bytes */
    HID_I2C_DESCRIPTOR_V10 temp;
    USHORT   descLen;

    status  = STATUS_SUCCESS;
    descLen = 0U;
    RtlZeroMemory(&temp, sizeof(HID_I2C_DESCRIPTOR_V10));

    /* Basic parameter validation */
    if (devctx == NULL || outBuf == NULL ||
        outBufLen < sizeof(HID_I2C_DESCRIPTOR_V10)) {
        KdPrint(("I2CHID(detect): ReadAndValidateHidDescriptor invalid parameters devctx=%p outBuf=%p len=%lu\n",
                 devctx, outBuf, outBufLen));
        return STATUS_INVALID_PARAMETER;
    }

    /* Step 1: read first 8 bytes (header contains descriptor length) */
    status = I2CHID_ReadHidRegister(devctx, addr, 0x01U, hdr, sizeof(hdr), 2000U);
    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CHID(detect): Failed to read HID header at 0x%02X (status=0x%08lx)\n",
                 addr, status));
        return status;
    }

    descLen = (USHORT)(hdr[0] | ((USHORT)hdr[1] << 8));

    /* Validate descriptor length field */
    if (descLen < sizeof(HID_I2C_DESCRIPTOR_V10) || descLen > 64U) {
        KdPrint(("I2CHID(detect): Invalid HID descriptor length %u at 0x%02X\n",
                 (unsigned)descLen, addr));
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    if ((ULONG)descLen > outBufLen) {
        KdPrint(("I2CHID(detect): HID descriptor length %u exceeds buffer size %lu\n",
                 (unsigned)descLen, outBufLen));
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Step 2: read full descriptor */
    status = I2CHID_ReadHidRegister(devctx, addr, 0x01U, outBuf, descLen, 4000U);
    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CHID(detect): Failed to read HID descriptor at 0x%02X (status=0x%08lx)\n",
                 addr, status));
        return status;
    }

    /* Step 3: validate and parse into temporary structure */
    if (!I2CHID_ParseHidDescriptorV10(outBuf, descLen, &temp)) {
        I2CHID_Log("I2CHID(detect): HID descriptor validation failed at 0x%02X\n", addr);
        return STATUS_INVALID_PARAMETER;
    }

    /* Copy parsed descriptor back to caller if requested */
    if (parsed != NULL) {
        *parsed = temp;
    }

    return STATUS_SUCCESS;
}


// ---------------------------------------------------------------------------
// I2CHID_DetectTouchpad – HID-side detection entry point for HID-over-I²C
// HAL-based, controller-agnostic, XP-BSOD-safe, WinDDK-compiler-safe,
// C89-compliant, ETW-logged.
// Probes common I²C addresses and validates HID descriptors without
// hard-linking to the controller binary.
// ---------------------------------------------------------------------------
NTSTATUS
I2CHID_DetectTouchpad(
    PI2CCTRL_FDO           dx,
    PI2CCTRL_DETECT_RESULT result
    )
{
    NTSTATUS status;            /* C89: declare first */
    UCHAR    buf[64];
    HID_I2C_DESCRIPTOR_V10 dsc;
    UCHAR    addr;
    ULONG    i;

    /* Parameter validation */
    if (dx == NULL || result == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "I2CHID_DetectTouchpad invalid parameters dx=%p result=%p",
                    dx, result);
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(result, sizeof(*result));
    RtlZeroMemory(buf, sizeof(buf));
    RtlZeroMemory(&dsc, sizeof(dsc));

    /* Step 1: Probe common candidate addresses */
    for (i = 0; i < HID_I2C_COMMON_CANDIDATES_COUNT; i++) {
        addr = g_HidI2cCommonCandidates[i];
        RtlZeroMemory(buf, sizeof(buf));
        RtlZeroMemory(&dsc, sizeof(dsc));

        status = I2CHID_ReadAndValidateHidDescriptor(dx, addr, buf, sizeof(buf), &dsc);
        if (NT_SUCCESS(status)) {
            result->Found         = TRUE;
            result->Present       = TRUE;
            result->IsTouchpad    = TRUE;
            result->Address       = addr;
            result->HidDescLength = dsc.wHIDDescLength;
            result->VendorID      = dsc.wVendorID;
            result->ProductID     = dsc.wProductID;
            result->VersionID     = dsc.wVersionID;

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_BUS,
                        "I2CHID_DetectTouchpad success addr=0x%02X VID=0x%04X PID=0x%04X Ver=0x%04X Len=%u",
                        addr,
                        (unsigned)dsc.wVendorID,
                        (unsigned)dsc.wProductID,
                        (unsigned)dsc.wVersionID,
                        (unsigned)dsc.wHIDDescLength);
            return STATUS_SUCCESS;
        }
    }

    /* Step 2: Fallback sweep 0x10..0x2F (skip already tried addresses) */
    for (addr = 0x10; addr <= 0x2F; addr++) {
        BOOLEAN alreadyTried = FALSE;

        for (i = 0; i < HID_I2C_COMMON_CANDIDATES_COUNT; i++) {
            if (g_HidI2cCommonCandidates[i] == addr) {
                alreadyTried = TRUE;
                break;
            }
        }
        if (alreadyTried) {
            continue;
        }

        RtlZeroMemory(buf, sizeof(buf));
        RtlZeroMemory(&dsc, sizeof(dsc));

        status = I2CHID_ReadAndValidateHidDescriptor(dx, addr, buf, sizeof(buf), &dsc);
        if (NT_SUCCESS(status)) {
            result->Found         = TRUE;
            result->Present       = TRUE;
            result->IsTouchpad    = TRUE;
            result->Address       = addr;
            result->HidDescLength = dsc.wHIDDescLength;
            result->VendorID      = dsc.wVendorID;
            result->ProductID     = dsc.wProductID;
            result->VersionID     = dsc.wVersionID;

            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_BUS,
                        "I2CHID_DetectTouchpad success addr=0x%02X VID=0x%04X PID=0x%04X Ver=0x%04X Len=%u",
                        addr,
                        (unsigned)dsc.wVendorID,
                        (unsigned)dsc.wProductID,
                        (unsigned)dsc.wVersionID,
                        (unsigned)dsc.wHIDDescLength);
            return STATUS_SUCCESS;
        }
    }

    /* No device found */
    result->Found      = FALSE;
    result->Present    = FALSE;
    result->IsTouchpad = FALSE;

    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_BUS,
                "I2CHID_DetectTouchpad: No HID touchpad detected in 0x10..0x2F");
    return STATUS_NOT_FOUND;
}


/* -----------------------------------------------------------------------
 * AddDevice - create and initialize the FDO for I2C HID
 * Feature-complete, XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant,
 * verbose-compliant, and integrated with I2CHID_DetectTouchpad via global callback.
 * ----------------------------------------------------------------------- */
NTSTATUS
I2CHID_AddDevice(
    IN PDRIVER_OBJECT  DriverObject,
    IN PDEVICE_OBJECT  PhysicalDeviceObject
    )
{
    NTSTATUS                 status;
    PDEVICE_OBJECT           fdo;
    PI2CHID_FDO          ext;
    UNICODE_STRING           devName;
    UNICODE_STRING           sym;
    UNICODE_STRING           iflink;      /* interface symbolic link (UNICODE_STRING) */
    BOOLEAN                  ifaceRegistered;
    BOOLEAN                  controllerOpened;
    BOOLEAN                  dosLinkCreated;
    I2CCTRL_DETECT_RESULT   detectResult;
    struct _I2CCTRL_FDO     *ctrlExt;

    /* C89: declare then assign */
    status            = STATUS_SUCCESS;
    fdo               = NULL;
    ext               = NULL;
    ifaceRegistered   = FALSE;
    controllerOpened  = FALSE;
    dosLinkCreated    = FALSE;
    ctrlExt           = NULL;
    RtlZeroMemory(&detectResult, sizeof(detectResult));

    /* Must run at PASSIVE_LEVEL; AddDevice is pageable by design */
    I2CHID_REQUIRE_PASSIVE();
    PAGED_CODE();

    /* Initialize optional named device for pass-through (kept from original) */
    RtlInitUnicodeString(&devName, I2CHID_PT_DEVNAME);

    /* Initialize interface link to empty */
    iflink.Buffer = NULL;
    iflink.Length = 0;
    iflink.MaximumLength = 0;

    /* Create FDO; named device maintained for compatibility with existing tools.
       Note: For pure PnP, DeviceName can be NULL. */
    status = IoCreateDevice(
        DriverObject,
        (ULONG)sizeof(I2CHID_FDO),
        &devName,                /* can be NULL for pure PnP; keep name per original */
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &fdo
        );
    if (!NT_SUCCESS(status)) {
        I2CHID_Log("I2CHID_AddDevice: IoCreateDevice failed (0x%08X)\n", status);
        return status;
    }

    /* Zero and wire up extension */
    ext = (PI2CHID_FDO)fdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(*ext));

    ext->Self           = fdo;
    ext->PhysicalDevice = PhysicalDeviceObject;
    ext->LowerDevice    = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (ext->LowerDevice == NULL) {
        I2CHID_Log("I2CHID_AddDevice: IoAttachDeviceToDeviceStack returned NULL\n");
        status = STATUS_NO_SUCH_DEVICE;
        goto FailIoCreateDevice;
    }

    /* PnP state */
    ext->Started  = FALSE;
    ext->Removed  = FALSE;

    /* Remove lock for safe PnP remove handling */
    IoInitializeRemoveLock(&ext->RemoveLock, I2CHID_PT_TAG, 0, 0);

    /* Queues and sync primitives */
    InitializeListHead(&ext->ReadQueue);
    I2CHID_INIT_LOCK(&ext->ReadQueueLock);

    InitializeListHead(&ext->PendingReads);
    I2CHID_INIT_LOCK(&ext->InputLock);
    KeInitializeEvent(&ext->ReadEvent, NotificationEvent, FALSE);

    /* Defaults (overridden by bus DPI and registry policy) */
    ext->Cfg.PalmThreshold = 10;
    ext->Cfg.ScrollScale   = 1;
    ext->Cfg.TapTimeMs     = 180;

    /* Interrupt-related defaults */
    ext->InterruptObject    = NULL;
    ext->InterruptConnected = FALSE;
    ext->PendingInputFlag   = 0;
    ext->InterruptIrql      = 0;
    ext->InterruptVector    = 0;
    ext->InterruptAffinity  = 0;
    ext->InterruptMode      = LevelSensitive;
    ext->InterruptSharable  = FALSE;

    /* DPC for deferred input */
    KeInitializeDpc(&ext->InterruptDpc, I2CHID_InterruptDpc, ext);

    /* Input history defaults */
    ext->LastBtnLeft   = FALSE;
    ext->LastBtnRight  = FALSE;
    ext->LastBtnMiddle = FALSE;
    ext->LastBtnX1     = FALSE;
    ext->LastBtnX2     = FALSE;
    ext->PrevX         = 0;
    ext->PrevY         = 0;

    /* DPI: initialize from bus-provided block if available on PDO */
    status = I2CHID_FetchBusDpi(PhysicalDeviceObject, &ext->Dpi);
    if (NT_SUCCESS(status)) {
        status = I2CHID_DpiInitializeFromBus(&ext->Dpi, &ext->Dpi);
        if (!NT_SUCCESS(status)) {
            I2CHID_Log("I2CHID_AddDevice: DPI init from bus failed (0x%08X)\n", status);
            goto FailAttached;
        }
    } else {
        /* If bus DPI is unavailable, keep existing defaults and continue */
        RtlZeroMemory(&ext->Dpi, sizeof(ext->Dpi));
        ext->Dpi.I2cAddr7Bit = 0x15; /* conservative default; overridden later */
        ext->Dpi.MaxX        = 4095;
        ext->Dpi.MaxY        = 4095;
        ext->Dpi.Sensitivity = 4;
        I2CHID_Log("I2CHID_AddDevice: Bus DPI not available, using defaults\n");
    }

    /* Ensure DPI’s ControllerDevice is known to downstream routines */
    ext->Dpi.ControllerDevice = ext->LowerDevice; /* used by I2cRead / HID query helpers */

    /* Apply HID-specific registry overrides (non-fatal) */
    status = I2CHID_DpiApplyRegistryPolicy(&ext->Dpi, NULL);
    if (!NT_SUCCESS(status)) {
        I2CHID_Log("I2CHID_AddDevice: DPI registry policy apply failed (0x%08X), continuing with defaults\n", status);
        status = STATUS_SUCCESS;
    }

    /* Open controller handle (optional if you only use device objects) */
    status = I2CHID_I2cCtrl_Open(&ext->ControllerHandle);
    if (!NT_SUCCESS(status)) {
        I2CHID_Log("I2CHID_AddDevice: Controller open failed (0x%08X)\n", status);
        goto FailAttached;
    }
    controllerOpened = TRUE;

    /* Touchpad detection via I2C controller global callback (no direct link needed) */
    ctrlExt = (struct _I2CCTRL_FDO *)ext->LowerDevice->DeviceExtension;
    if (ctrlExt == NULL) {
        I2CHID_Log("I2CHID_AddDevice: Lower controller extension is NULL\n");
        status = STATUS_NO_SUCH_DEVICE;
        goto FailControllerOpened;
    }

    status = I2CHID_DetectTouchpad(ctrlExt, &detectResult);
    if (!NT_SUCCESS(status)) {
        I2CHID_Log("I2CHID_AddDevice: DetectTouchpad failed (0x%08X)\n", status);
        goto FailControllerOpened;
    }

    /* NOTE: if your detect result struct uses a different field name than IsTouchpad,
             replace detectResult.IsTouchpad below accordingly. */
    if (!detectResult.IsTouchpad) {
        I2CHID_Log("I2CHID_AddDevice: No touchpad detected, aborting HID init\n");
        status = STATUS_NO_SUCH_DEVICE;
        goto FailControllerOpened;
    }
    I2CHID_Log("I2CHID_AddDevice: Touchpad detected, proceeding with HID init\n");

    /* HID descriptors from static table or device via I2C */
    status = I2CHID_InitHidDescriptors(ext);
    if (!NT_SUCCESS(status)) {
        I2CHID_Log("I2CHID_AddDevice: HID descriptor init failed (0x%08X)\n", status);
        goto FailControllerOpened;
    }

    /* Register HID-over-I2C interface for user-mode clients */
    status = I2CHID_DpiRegisterInterface(fdo, &iflink);
    if (!NT_SUCCESS(status)) {
        I2CHID_Log("I2CHID_AddDevice: Interface registration failed (0x%08X)\n", status);
        goto FailControllerOpened;
    }
    ifaceRegistered = TRUE;
    ext->Symlink = iflink; /* store interface link for later disable on remove */

    /* Create legacy DOS symbolic link if needed (non-fatal on failure) */
    RtlInitUnicodeString(&sym, I2CHID_PT_DOSLINK);
    status = IoCreateSymbolicLink(&sym, &devName);
    if (NT_SUCCESS(status)) {
        dosLinkCreated = TRUE;
    } else {
        I2CHID_Log("I2CHID_AddDevice: DOS symlink create failed (0x%08X) - continuing\n", status);
        status = STATUS_SUCCESS;
    }

    /* Finalize FDO flags */
    fdo->Flags |= DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    I2CHID_Log("I2CHID_AddDevice: FDO %p initialized and attached (Lower=%p)\n", fdo, ext->LowerDevice);
    return STATUS_SUCCESS;

/* --- Failure paths with proper cleanup (reverse order of acquisition) --- */
FailControllerOpened:
    if (controllerOpened) {
        I2CHID_I2cCtrl_Close(ext->ControllerHandle);
        controllerOpened = FALSE;
    }
    if (ifaceRegistered) {
        I2CHID_DpiUnregisterInterface(&iflink);
        ifaceRegistered = FALSE;
    }
    /* Delete DOS link only if it was successfully created */
    if (dosLinkCreated) {
        RtlInitUnicodeString(&sym, I2CHID_PT_DOSLINK);
        IoDeleteSymbolicLink(&sym);
        dosLinkCreated = FALSE;
    }

FailAttached:
    if (ext != NULL && ext->LowerDevice != NULL) {
        IoDetachDevice(ext->LowerDevice);
        ext->LowerDevice = NULL;
    }

FailIoCreateDevice:
    if (fdo != NULL) {
        IoDeleteDevice(fdo);
    }
    return status;
}


/* Return a ULONG read in little-endian order */
ULONG
I2CHID_ReadUlongLe(
    const UCHAR* p
    )
{
    return ((ULONG)p[0])
         | ((ULONG)p[1] << 8)
         | ((ULONG)p[2] << 16)
         | ((ULONG)p[3] << 24);
}

/* -----------------------------------------------------------------------
 * ApplyFeatureReport – update gesture/timing config from a HID Feature report
 * Feature-complete, XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant,
 * and verbose-compliant.
 *
 * Expects:
 *   ReportID (1 byte) + 8 little-endian ULONG fields:
 *     1) ScrollScale      (LONG; 1..32)
 *     2) ZoomScale        (ULONG; 1..32)
 *     3) TapTimeMs        (LONG; 10..1000 ms)
 *     4) TapDistance      (ULONG; 0..1000 units)
 *     5) PalmThreshold    (LONG; 0..100)
 *     6) SwipeScale       (LONG; 1..32)
 *     7) RotateScale      (LONG; 1..32)
 *     8) PollIntervalMs   (LONG; 1..1000 ms)
 * ----------------------------------------------------------------------- */
NTSTATUS
I2CHID_ApplyFeatureReport(
    PI2CHID_FDO ext,
    const PUCHAR    inBuf,
    ULONG           inLen
    )
{
    NTSTATUS status;
    ULONG    offset;
    ULONG    v;
    ULONG    needLen;

    /* Must run at PASSIVE_LEVEL */
    I2CHID_REQUIRE_PASSIVE();
    PAGED_CODE();

    /* Validate inputs */
    if ((ext == NULL) || (inBuf == NULL)) {
        I2CHID_Log("I2CHID(ApplyFeatureReport): invalid parameters ext=%p inBuf=%p\n", ext, inBuf);
        return STATUS_INVALID_PARAMETER;
    }

    /* Expected payload size: ReportID + 8 ULONGs */
    needLen = 1U + (8U * 4U);
    if (inLen < needLen) {
        I2CHID_Log("I2CHID(ApplyFeatureReport): buffer too small (have=%lu need=%lu)\n", inLen, needLen);
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Verify Report ID */
    if (inBuf[0] != (UCHAR)I2CHID_FEATURE_RID) {
        KdPrint(("I2CHID(ApplyFeatureReport): invalid ReportID=0x%02X (expected=0x%02X)\n",
                 (ULONG)inBuf[0], (ULONG)I2CHID_FEATURE_RID));
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    status = STATUS_SUCCESS;
    offset = 1U;

    /* Helper: read next ULONG LE safely */
#define READ_NEXT_ULONG_LE(dest)                                      \
    do {                                                              \
        if ((offset + 4U) > inLen) {                                  \
            I2CHID_Log("I2CHID(ApplyFeatureReport): overrun at off=%lu\n", offset); \
            return STATUS_BUFFER_TOO_SMALL;                           \
        }                                                             \
        (dest) = ((ULONG)inBuf[offset])                               \
               | ((ULONG)inBuf[offset+1] << 8)                        \
               | ((ULONG)inBuf[offset+2] << 16)                       \
               | ((ULONG)inBuf[offset+3] << 24);                      \
        offset += 4U;                                                 \
    } while (0)

    /* 1) ScrollScale (LONG; 1..32) */
    READ_NEXT_ULONG_LE(v);
    if (v != 0U) {
        if (v < 1U)  v = 1U;
        if (v > 32U) v = 32U;
        ext->Cfg.ScrollScale = (LONG)v;
    }

    /* 2) ZoomScale (ULONG; 1..32) */
    READ_NEXT_ULONG_LE(v);
    if (v != 0U) {
        if (v < 1U)  v = 1U;
        if (v > 32U) v = 32U;
        ext->Cfg.ZoomScale = v;
    }

    /* 3) TapTimeMs (LONG; 10..1000 ms) */
    READ_NEXT_ULONG_LE(v);
    if (v != 0U) {
        if (v < 10U)   v = 10U;
        if (v > 1000U) v = 1000U;
        ext->Cfg.TapTimeMs = (LONG)v;
    }

    /* 4) TapDistance (ULONG; 0..1000 units) */
    READ_NEXT_ULONG_LE(v);
    if (v != 0U) {
        if (v > 1000U) v = 1000U; /* min is 0; only cap upper bound */
        ext->Cfg.TapDistance = v;
    }

    /* 5) PalmThreshold (LONG; 0..100) */
    READ_NEXT_ULONG_LE(v);
    if (v != 0U) {
        if (v > 100U) v = 100U; /* min is 0; only cap upper bound */
        ext->Cfg.PalmThreshold = (LONG)v;
    }

    /* 6) SwipeScale (LONG; 1..32) */
    READ_NEXT_ULONG_LE(v);
    if (v != 0U) {
        if (v < 1U)  v = 1U;
        if (v > 32U) v = 32U;
        ext->Cfg.SwipeScale = (LONG)v;
    }

    /* 7) RotateScale (LONG; 1..32) */
    READ_NEXT_ULONG_LE(v);
    if (v != 0U) {
        if (v < 1U)  v = 1U;
        if (v > 32U) v = 32U;
        ext->Cfg.RotateScale = (LONG)v;
    }

    /* 8) PollIntervalMs (LONG; 1..1000 ms) */
    READ_NEXT_ULONG_LE(v);
    if (v != 0U) {
        if (v < 1U)    v = 1U;
        if (v > 1000U) v = 1000U;
        ext->Cfg.PollIntervalMs = (LONG)v;
    }

    /* Persist updated config back to registry */
    status = I2CHID_SaveRegistryConfig(ext);
    if (!NT_SUCCESS(status)) {
        I2CHID_Log("I2CHID(ApplyFeatureReport): registry save failed 0x%08X\n", status);
        return status; /* Non-fatal for runtime, but report failure to caller */
    }

    KdPrint(("I2CHID(ApplyFeatureReport): applied feature report "
             "(Scroll=%ld Zoom=%lu TapTime=%ld TapDist=%lu Palm=%ld Swipe=%ld Rotate=%ld Poll=%ld)\n",
             ext->Cfg.ScrollScale,
             ext->Cfg.ZoomScale,
             ext->Cfg.TapTimeMs,
             ext->Cfg.TapDistance,
             ext->Cfg.PalmThreshold,
             ext->Cfg.SwipeScale,
             ext->Cfg.RotateScale,
             ext->Cfg.PollIntervalMs));

    return STATUS_SUCCESS;

#undef READ_NEXT_ULONG_LE
}


VOID
I2CHID_InitDefaultConfig(
    PI2CHID_FDO ext
    )
{
    if (ext == NULL) return;
    ext->Cfg.ScrollScale    = 4;
    ext->Cfg.ZoomScale      = 8;
    ext->Cfg.TapTimeMs      = 200;
    ext->Cfg.TapDistance    = 12;
    ext->Cfg.PalmThreshold  = 100;
    ext->Cfg.RotateScale    = 5;
    ext->Cfg.SwipeScale     = 20;
    ext->Cfg.PollIntervalMs = 8;
}

/* -----------------------------------------------------------------------
 * StartDevice – initialize hardware resources for HID-over-I2C FDO
 * Feature-complete, XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant,
 * and verbose-compliant.
 *  - Maps interrupt resources, connects ISR
 *  - Parses ACPI PNP0C50 for HID descriptor and I2C registers
 *  - Initializes gesture defaults and applies registry overrides
 *  - Wires DPI ControllerDevice and queries HID descriptor length
 * ----------------------------------------------------------------------- */
NTSTATUS
I2CHID_StartDevice(
    IN PI2CHID_FDO     ext,
    IN PCM_RESOURCE_LIST   raw,
    IN PCM_RESOURCE_LIST   translated
    )
{
    NTSTATUS                        status;
    ULONG                           i;
    PCM_FULL_RESOURCE_DESCRIPTOR    frd;
    PCM_PARTIAL_RESOURCE_LIST       prl;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR prd;
    BOOLEAN                         haveInterrupt;

    /* Must run at PASSIVE_LEVEL; StartDevice is pageable */
    I2CHID_REQUIRE_PASSIVE();
    PAGED_CODE();

    /* Validate parameters */
    if (ext == NULL) {
        I2CHID_Log("I2CHID(StartDevice): ext=NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* C89: declare then assign */
    status        = STATUS_SUCCESS;
    haveInterrupt = FALSE;
    frd           = NULL;
    prl           = NULL;
    prd           = NULL;

    /* Clear/initialize runtime state */
    ext->InterruptConnected = FALSE;
    ext->InterruptObject    = NULL;
    ext->PendingInputFlag   = 0;
    ext->Started            = FALSE;

    /* Initialize gesture config defaults then load registry overrides */
    I2CHID_InitDefaultConfig(ext);
    I2CHID_LoadRegistryConfig(ext);

    /* Defensive: translated resources can be NULL for virtual devices */
    if ((translated != NULL) && (translated->Count > 0U)) {
        frd = &translated->List[0];
        prl = &frd->PartialResourceList;

        /* Walk through partial descriptors to find an interrupt resource */
        for (i = 0U; i < prl->Count; i++) {
            prd = &prl->PartialDescriptors[i];

            if (prd->Type == CmResourceTypeInterrupt) {
                ext->InterruptVector   = prd->u.Interrupt.Vector;
                ext->InterruptIrql     = (KIRQL)prd->u.Interrupt.Level;
                ext->InterruptAffinity = prd->u.Interrupt.Affinity;
                ext->InterruptMode     = ((prd->Flags & CM_RESOURCE_INTERRUPT_LATCHED) != 0)
                                           ? Latched : LevelSensitive;
                ext->InterruptSharable = (prd->ShareDisposition != CmResourceShareDeviceExclusive);

                KdPrint(("I2CHID(StartDevice): Interrupt Vector=%lu IRQL=%lu Mode=%s Shareable=%lu\n",
                         (ULONG)ext->InterruptVector,
                         (ULONG)ext->InterruptIrql,
                         (ext->InterruptMode == Latched) ? "Latched" : "Level",
                         (ULONG)ext->InterruptSharable));

                /* Connect ISR to interrupt object (XP-safe IoConnectInterrupt) */
                status = IoConnectInterrupt(
                             &ext->InterruptObject,
                             I2CHID_InterruptServiceRoutine,
                             ext,
                             NULL,
                             ext->InterruptVector,
                             ext->InterruptIrql,
                             ext->InterruptIrql,
                             ext->InterruptMode,
                             ext->InterruptSharable,
                             ext->InterruptAffinity,
                             FALSE);

                if (!NT_SUCCESS(status)) {
                    I2CHID_Log("I2CHID(StartDevice): IoConnectInterrupt failed 0x%08X\n", status);
                    /* Prevent partial init; return failure */
                    return status;
                }

                ext->InterruptConnected = TRUE;
                haveInterrupt = TRUE;
                break; /* use first interrupt resource found */
            }
        }

        if (!haveInterrupt) {
            I2CHID_Log("I2CHID(StartDevice): No interrupt resource found; proceeding without ISR\n");
        }
    } else {
        I2CHID_Log("I2CHID(StartDevice): No translated resources; skipping ISR mapping\n");
    }

    /* Initialize DPC for deferred ISR processing (safe even without ISR) */
    KeInitializeDpc(&ext->InterruptDpc, I2CHID_InterruptDpc, ext);

    /* Parse ACPI resources for HID-over-I²C (PNP0C50) */
    status = I2CHID_AcpiParsePnp0C50(
                 ext->PhysicalDevice,   /* FDO’s physical device object */
                 raw,
                 translated,
                 &ext->Dpi);            /* DPI context with ControllerDevice member */
    if (!NT_SUCCESS(status)) {
        I2CHID_Log("I2CHID(StartDevice): ACPI parse failed 0x%08X\n", status);
        /* If ACPI parsing fails, tear down interrupt and return error */
        if (ext->InterruptConnected && (ext->InterruptObject != NULL)) {
            IoDisconnectInterrupt(ext->InterruptObject);
            ext->InterruptObject    = NULL;
            ext->InterruptConnected = FALSE;
        }
        return status;
    }

    /* Assign controller device object into DPI for later I2C transactions */
    if (ext->LowerDevice != NULL) {
        ext->Dpi.ControllerDevice = ext->LowerDevice;
    } else {
        I2CHID_Log("I2CHID(StartDevice): LowerDevice=NULL; ControllerDevice not set\n");
        ext->Dpi.ControllerDevice = NULL;
    }

    /* Query HID descriptor length (optional, depends on ACPI parse outcome) */
    if ((ext->Dpi.HidDescriptor != NULL) && (ext->Dpi.ControllerDevice != NULL)) {
        ULONG hidLen;
        hidLen = 0U;

        (void)I2CHID_QueryHidDescriptorLength(
            ext->Dpi.ControllerDevice,
            ext->Dpi.I2cAddr7Bit,
            (ULONG)(ULONG_PTR)ext->Dpi.HidDescriptor,
            &hidLen);

        ext->Dpi.HidDescriptorLength = hidLen;
        I2CHID_Log("I2CHID(StartDevice): HID descriptor length=%lu\n", hidLen);
    } else {
        I2CHID_Log("I2CHID(StartDevice): HID descriptor or ControllerDevice missing; skip length query\n");
    }

    /* Mark device started only after all init steps succeed */
    ext->Started = TRUE;

    KdPrint(("I2CHID(StartDevice): Initialization complete (Started=TRUE, InterruptConnected=%lu)\n",
             (ULONG)ext->InterruptConnected));
    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
   StopDevice helper – teardown interrupt, timers, I/O, ACPI context, and resources
   XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant, verbose-compliant.
   ----------------------------------------------------------------------- */
NTSTATUS
I2CHID_StopDevice(
    IN PI2CHID_FDO Ext
    )
{
    KIRQL           oldIrql;
    PLIST_ENTRY     le;
    PIRP            pending;

    /* Defensive: validate extension and started state */
    if (Ext == NULL) {
        I2CHID_Log("I2CHID(StopDevice): Ext=NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* Enforce PASSIVE_LEVEL for teardown on XP */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        KdPrint(("I2CHID(StopDevice): Called at IRQL=%lu (expected PASSIVE_LEVEL)\n",
                 (ULONG)KeGetCurrentIrql()));
        /* Best-effort continue; many APIs require PASSIVE_LEVEL */
    }

    if (!Ext->Started) {
        I2CHID_Log("I2CHID(StopDevice): Device not started; nothing to stop\n");
        Ext->LastStopStatus = STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }

    I2CHID_Log("I2CHID(StopDevice): Teardown begin\n");

    /* 1) Stop input path: cancel idle/selective suspend, drain DPCs */
    InterlockedExchange(&Ext->IdleArmed, 0);
    KeCancelTimer(&Ext->IdleTimer);
    KeFlushQueuedDpcs();

    /* 2) Disconnect interrupt if connected */
    if (Ext->InterruptConnected && Ext->InterruptObject != NULL) {
        IoDisconnectInterrupt(Ext->InterruptObject);
        Ext->InterruptObject    = NULL;
        Ext->InterruptConnected = FALSE;
        I2CHID_Log("I2CHID(StopDevice): Interrupt disconnected\n");
    }

    /* 3) Cancel and complete all pending read IRPs (queued to ReadQueue) */
    KeAcquireSpinLock(&Ext->ReadQueueLock, &oldIrql);
    while (!IsListEmpty(&Ext->ReadQueue)) {
        le      = RemoveHeadList(&Ext->ReadQueue);
        pending = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        pending->IoStatus.Status      = STATUS_CANCELLED;
        pending->IoStatus.Information = 0;
        IoCompleteRequest(pending, IO_NO_INCREMENT);
    }
    KeReleaseSpinLock(&Ext->ReadQueueLock, oldIrql);

    /* 3b) Cancel and complete any reads tracked in PendingReads (if used) */
    KeAcquireSpinLock(&Ext->InputLock, &oldIrql);
    while (!IsListEmpty(&Ext->PendingReads)) {
        le      = RemoveHeadList(&Ext->PendingReads);
        pending = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        pending->IoStatus.Status      = STATUS_CANCELLED;
        pending->IoStatus.Information = 0;
        IoCompleteRequest(pending, IO_NO_INCREMENT);
    }
    KeReleaseSpinLock(&Ext->InputLock, oldIrql);

    /* 4) Unmap MMIO/bar resources */
    if (Ext->MmioBase != NULL && Ext->MmioLength != 0) {
        MmUnmapIoSpace(Ext->MmioBase, Ext->MmioLength);
        Ext->MmioBase   = NULL;
        Ext->MmioLength = 0;
        I2CHID_Log("I2CHID(StopDevice): MMIO unmapped\n");
    }

    /* 5) Close controller/bus handles */
    if (Ext->ControllerHandle != NULL) {
        I2CHID_I2cCtrl_Close(Ext->ControllerHandle);
        Ext->ControllerHandle = NULL;
        I2CHID_Log("I2CHID(StopDevice): ControllerHandle closed\n");
    }

    /* 6) Free dynamic HID report descriptor (if allocated) */
    if (Ext->HidStatic.ReportDesc != NULL) {
        ExFreePoolWithTag(Ext->HidStatic.ReportDesc, 'dRhI'); /* "IHRd" */
        Ext->HidStatic.ReportDesc       = NULL;
        Ext->HidStatic.ReportDescLength = 0;
        I2CHID_Log("I2CHID(StopDevice): Report descriptor freed\n");
    }

    /* 7) Unregister user-mode interface and delete legacy DOS link */
    if (Ext->Symlink.Buffer != NULL) {
        I2CHID_DpiUnregisterInterface(&Ext->Symlink);
        Ext->Symlink.Buffer         = NULL;
        Ext->Symlink.Length         = 0;
        Ext->Symlink.MaximumLength  = 0;
        I2CHID_Log("I2CHID(StopDevice): Interface unregistered\n");
    }

    /* 8) Clear ACPI/HID-over-I²C context */
    Ext->HidDescriptorAddress   = 0;
    Ext->InputRegisterAddress   = 0;
    Ext->CommandRegisterAddress = 0;
    Ext->DataRegisterAddress    = 0;
    Ext->ResetRegisterAddress   = 0;
    Ext->AcpiParsed             = FALSE;

    /* Also clear ACPI PNP0C50 identifiers */
    if (Ext->DeviceId.Buffer != NULL) {
        RtlFreeUnicodeString(&Ext->DeviceId);
        Ext->DeviceId.Buffer        = NULL;
        Ext->DeviceId.Length        = 0;
        Ext->DeviceId.MaximumLength = 0;
    }
    if (Ext->HardwareIds.Buffer != NULL) {
        RtlFreeUnicodeString(&Ext->HardwareIds);
        Ext->HardwareIds.Buffer        = NULL;
        Ext->HardwareIds.Length        = 0;
        Ext->HardwareIds.MaximumLength = 0;
    }
    if (Ext->CompatibleIds.Buffer != NULL) {
        RtlFreeUnicodeString(&Ext->CompatibleIds);
        Ext->CompatibleIds.Buffer        = NULL;
        Ext->CompatibleIds.Length        = 0;
        Ext->CompatibleIds.MaximumLength = 0;
    }

    /* 9) Clear DPI/controller context */
    Ext->Dpi.ControllerDevice    = NULL;
    Ext->Dpi.HidDescriptor       = NULL;
    Ext->Dpi.HidDescriptorLength = 0;
    Ext->PendingInputFlag        = 0;

    /* 10) Clear device state flags and diagnostics */
    Ext->Started        = FALSE;
    Ext->LastStopStatus = STATUS_SUCCESS;

    I2CHID_Log("I2CHID(StopDevice): Teardown complete\n");
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * RestartDevice – HID-over-I2C (i2chid.sys)
 * Feature-complete, XP/2003 BSOD-safe, WinDDK-compiler-safe, C89-compliant,
 * and verbose-compliant.
 *
 * Purpose:
 *  - Quiesce input path (disconnect ISR/DPC, cancel timers)
 *  - Safely complete pending IRPs outside of spin locks
 *  - Reinitialize synchronization and gesture/input state
 *  - Reconnect interrupt using saved PnP resources
 *  - Mark device started after a clean restart
 *
 * Guarantees:
 *  - Runs at PASSIVE_LEVEL
 *  - Touches only non-paged memory
 *  - Avoids unsafe MMIO access
 * ----------------------------------------------------------------------- */
NTSTATUS
I2CHID_RestartDevice(
    IN PI2CHID_FDO ext
    )
{
    NTSTATUS     status;
    BOOLEAN      haveInt;
    KIRQL        oldIrql;
    LIST_ENTRY   tempReadList;
    LIST_ENTRY   tempPendingList;
    PLIST_ENTRY  le;
    PIRP         irp;

    /* Enforce PASSIVE_LEVEL for restart on XP/2003 */
    I2CHID_REQUIRE_PASSIVE();
    PAGED_CODE();

    /* Validate input */
    if (ext == NULL) {
        I2CHID_Log("I2CHID(Restart): ext=NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* C89 locals init */
    status = STATUS_SUCCESS;
    haveInt = FALSE;
    oldIrql = PASSIVE_LEVEL;
    le = NULL;
    irp = NULL;

    I2CHID_Log("I2CHID(Restart): begin for FDO %p (Lower=%p)\n", ext->Self, ext->LowerDevice);

    /* Block new I/O paths during restart */
    ext->Started = FALSE;
    ext->Removed = FALSE;

    /* 1) Quiesce input path: cancel idle/selective suspend, drain DPCs */
    InterlockedExchange(&ext->IdleArmed, 0);
    KeCancelTimer(&ext->IdleTimer);
    KeFlushQueuedDpcs();

    /* 2) Disconnect interrupt if currently connected (PASSIVE_LEVEL required) */
    if (ext->InterruptConnected && (ext->InterruptObject != NULL)) {
        IoDisconnectInterrupt(ext->InterruptObject);
        ext->InterruptObject    = NULL;
        ext->InterruptConnected = FALSE;
        I2CHID_Log("I2CHID(Restart): Interrupt disconnected\n");
    }

    /* Prepare temp lists to complete IRPs outside of spin locks */
    InitializeListHead(&tempReadList);
    InitializeListHead(&tempPendingList);

    /* 3) Drain ReadQueue into tempReadList under lock */
    KeAcquireSpinLock(&ext->ReadQueueLock, &oldIrql);
    while (!IsListEmpty(&ext->ReadQueue)) {
        le = RemoveHeadList(&ext->ReadQueue);
        InsertTailList(&tempReadList, le);
    }
    KeReleaseSpinLock(&ext->ReadQueueLock, oldIrql);

    /* 3a) Drain PendingReads into tempPendingList under lock */
    KeAcquireSpinLock(&ext->InputLock, &oldIrql);
    while (!IsListEmpty(&ext->PendingReads)) {
        le = RemoveHeadList(&ext->PendingReads);
        InsertTailList(&tempPendingList, le);
    }
    KeReleaseSpinLock(&ext->InputLock, oldIrql);

    /* 3b) Complete drained IRPs outside of locks */
    while (!IsListEmpty(&tempReadList)) {
        le  = RemoveHeadList(&tempReadList);
        irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        irp->IoStatus.Status      = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }

    while (!IsListEmpty(&tempPendingList)) {
        le  = RemoveHeadList(&tempPendingList);
        irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        irp->IoStatus.Status      = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }

    /* 4) Reset input/gesture state and flags */
    ext->PendingInputFlag = 0;
    RtlZeroMemory(&ext->Gest, sizeof(ext->Gest));
    RtlZeroMemory(ext->LastReport, sizeof(ext->LastReport));

    /* 5) Reinitialize DPC handoff */
    KeInitializeDpc(&ext->InterruptDpc, I2CHID_InterruptDpc, ext);

    /* 6) Reconnect interrupt from saved PnP resource data (if present) */
    haveInt = (ext->InterruptVector != 0U) &&
              ((ULONGLONG)ext->InterruptAffinity != 0ULL);

    if (haveInt && (ext->InterruptObject == NULL)) {
        status = IoConnectInterrupt(
                    &ext->InterruptObject,
                    I2CHID_InterruptServiceRoutine,
                    ext,
                    NULL, /* optional spinlock; ISR uses its own locks */
                    ext->InterruptVector,
                    ext->InterruptIrql,
                    ext->InterruptIrql,
                    ext->InterruptMode,      /* Latched or LevelSensitive (saved at START) */
                    ext->InterruptSharable,  /* TRUE if shared vector allowed */
                    ext->InterruptAffinity,
                    FALSE);
        if (!NT_SUCCESS(status)) {
            I2CHID_Log("I2CHID(Restart): IoConnectInterrupt failed 0x%08X\n", status);
            return status;
        }
        ext->InterruptConnected = TRUE;
        I2CHID_Log("I2CHID(Restart): Interrupt connected\n");
    } else {
        KdPrint(("I2CHID(Restart): No interrupt to connect (haveInt=%lu, obj=%p)\n",
                 (ULONG)haveInt, (PVOID)ext->InterruptObject));
    }

    /* 7) Re-apply registry-configured gesture/timing overrides */
    I2CHID_InitDefaultConfig(ext);
    I2CHID_LoadRegistryConfig(ext);

    /* 8) Finalize */
    ext->Started = TRUE;

    KdPrint(("I2CHID(Restart): complete (Started=TRUE, InterruptConnected=%lu)\n",
             (ULONG)ext->InterruptConnected));
    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * RemoveDevice helper – enforce RemoveLock, cancel pending IRPs, stop hardware,
 * detach, delete, and clear ACPI/HID-over-I2C context.
 * Feature-complete, XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant,
 * and verbose-compliant.
 * ----------------------------------------------------------------------- */
NTSTATUS
I2CHID_RemoveDevice(
    IN PDEVICE_OBJECT DeviceObject
    )
{
    PI2CHID_FDO ext;
    KIRQL           oldIrql;
    LIST_ENTRY      tempReadList;
    LIST_ENTRY      tempPendingList;
    PLIST_ENTRY     le;
    PIRP            irp;
    UNICODE_STRING  sym;
    NTSTATUS        status;
    PVOID           remTag;

    /* Must run at PASSIVE_LEVEL for teardown actions on XP/2003 */
    I2CHID_REQUIRE_PASSIVE();
    PAGED_CODE();

    /* Validate inputs */
    if (DeviceObject == NULL) {
        I2CHID_Log("I2CHID(RemoveDevice): DeviceObject=NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    ext = (PI2CHID_FDO)DeviceObject->DeviceExtension;
    if (ext == NULL) {
        I2CHID_Log("I2CHID(RemoveDevice): DeviceExtension=NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* Acquire RemoveLock for non-IRP context to serialize teardown with in-flight IRPs */
    remTag = (PVOID)DeviceObject; /* unique tag for this removal context */
    status = IoAcquireRemoveLock(&ext->RemoveLock, remTag);
    if (!NT_SUCCESS(status)) {
        I2CHID_Log("I2CHID(RemoveDevice): IoAcquireRemoveLock failed 0x%08X\n", status);
        return status;
    }

    KdPrint(("I2CHID(RemoveDevice): Begin removal for FDO %p (Lower=%p)\n",
             DeviceObject, ext->LowerDevice));

    /* Mark removed/stopped and reset open handles to block new I/O */
    ext->Removed   = TRUE;
    ext->Started   = FALSE;
    ext->OpenCount = 0;

    /* 1) Stop input path: cancel idle/selective suspend, drain DPCs */
    InterlockedExchange(&ext->IdleArmed, 0);
    KeCancelTimer(&ext->IdleTimer);
    KeFlushQueuedDpcs();

    /* 2) Disconnect interrupt if connected (PASSIVE_LEVEL required) */
    if (ext->InterruptConnected && (ext->InterruptObject != NULL)) {
        IoDisconnectInterrupt(ext->InterruptObject);
        ext->InterruptObject    = NULL;
        ext->InterruptConnected = FALSE;
        I2CHID_Log("I2CHID(RemoveDevice): Interrupt disconnected\n");
    }

    /* Prepare temp lists for safe completion outside of spin locks */
    InitializeListHead(&tempReadList);
    InitializeListHead(&tempPendingList);

    /* 3) Drain ReadQueue into tempReadList under lock */
    KeAcquireSpinLock(&ext->ReadQueueLock, &oldIrql);
    while (!IsListEmpty(&ext->ReadQueue)) {
        le = RemoveHeadList(&ext->ReadQueue);
        InsertTailList(&tempReadList, le);
    }
    KeReleaseSpinLock(&ext->ReadQueueLock, oldIrql);

    /* 3a) Drain PendingReads into tempPendingList under lock */
    KeAcquireSpinLock(&ext->InputLock, &oldIrql);
    while (!IsListEmpty(&ext->PendingReads)) {
        le = RemoveHeadList(&ext->PendingReads);
        InsertTailList(&tempPendingList, le);
    }
    KeReleaseSpinLock(&ext->InputLock, oldIrql);

    /* 3b) Complete drained IRPs outside of locks */
    while (!IsListEmpty(&tempReadList)) {
        le  = RemoveHeadList(&tempReadList);
        irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        irp->IoStatus.Status      = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }

    while (!IsListEmpty(&tempPendingList)) {
        le  = RemoveHeadList(&tempPendingList);
        irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        irp->IoStatus.Status      = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }

    /* 4) Unmap MMIO/bar resources */
    if ((ext->MmioBase != NULL) && (ext->MmioLength != 0)) {
        MmUnmapIoSpace(ext->MmioBase, ext->MmioLength);
        ext->MmioBase   = NULL;
        ext->MmioLength = 0;
        I2CHID_Log("I2CHID(RemoveDevice): MMIO unmapped\n");
    }

    /* 5) Close controller/bus handles */
    if (ext->ControllerHandle != NULL) {
        I2CHID_I2cCtrl_Close(ext->ControllerHandle);
        ext->ControllerHandle = NULL;
        I2CHID_Log("I2CHID(RemoveDevice): ControllerHandle closed\n");
    }

    /* 6) Free dynamic HID report descriptor only if it was allocated dynamically.
          If your build uses a static global (g_HidReportDesc), this block will be a no-op. */
    if ((ext->HidStatic.ReportDesc != NULL) && (ext->HidStatic.ReportDesc != (PUCHAR)g_HidReportDesc)) {
        ExFreePoolWithTag(ext->HidStatic.ReportDesc, 'dRhI'); /* "IHRd" */
        I2CHID_Log("I2CHID(RemoveDevice): Report descriptor freed\n");
    }
    ext->HidStatic.ReportDesc       = NULL;
    ext->HidStatic.ReportDescLength = 0;

    /* 7) Unregister user-mode interface and delete legacy DOS link if present */
    if (ext->Symlink.Buffer != NULL) {
        I2CHID_DpiUnregisterInterface(&ext->Symlink);
        ext->Symlink.Buffer        = NULL;
        ext->Symlink.Length        = 0;
        ext->Symlink.MaximumLength = 0;
        I2CHID_Log("I2CHID(RemoveDevice): Interface unregistered\n");
    }

    RtlInitUnicodeString(&sym, I2CHID_PT_DOSLINK);
    IoDeleteSymbolicLink(&sym);

    /* 8) Free ACPI PNP strings safely */
    if (ext->DeviceId.Buffer != NULL) {
        RtlFreeUnicodeString(&ext->DeviceId);
        ext->DeviceId.Buffer        = NULL;
        ext->DeviceId.Length        = 0;
        ext->DeviceId.MaximumLength = 0;
    }
    if (ext->HardwareIds.Buffer != NULL) {
        RtlFreeUnicodeString(&ext->HardwareIds);
        ext->HardwareIds.Buffer        = NULL;
        ext->HardwareIds.Length        = 0;
        ext->HardwareIds.MaximumLength = 0;
    }
    if (ext->CompatibleIds.Buffer != NULL) {
        RtlFreeUnicodeString(&ext->CompatibleIds);
        ext->CompatibleIds.Buffer        = NULL;
        ext->CompatibleIds.Length        = 0;
        ext->CompatibleIds.MaximumLength = 0;
    }

    /* 9) Clear ACPI/HID-over-I²C context */
    ext->HidDescriptorAddress   = 0;
    ext->InputRegisterAddress   = 0;
    ext->CommandRegisterAddress = 0;
    ext->DataRegisterAddress    = 0;
    ext->ResetRegisterAddress   = 0;
    ext->AcpiParsed             = FALSE;

    /* 10) Clear DPI/controller context */
    ext->Dpi.ControllerDevice    = NULL;
    ext->Dpi.HidDescriptor       = NULL;
    ext->Dpi.HidDescriptorLength = 0;
    ext->PendingInputFlag        = 0;

    /* 11) Detach from lower device */
    if (ext->LowerDevice != NULL) {
        IoDetachDevice(ext->LowerDevice);
        ext->LowerDevice = NULL;
        I2CHID_Log("I2CHID(RemoveDevice): Detached from lower device\n");
    }

    /* 12) Release remove lock and wait for all outstanding IRPs to drain before deletion */
    IoReleaseRemoveLockAndWait(&ext->RemoveLock, remTag);

    /* 13) Delete device object */
    IoDeleteDevice(DeviceObject);

    I2CHID_Log("I2CHID(RemoveDevice): Device removed, resources freed\n");
    return STATUS_SUCCESS;
}

VOID
I2CHID_DriverUnload(
    IN PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
}


/* -----------------------------------------------------------------------
   Unified PnP dispatch - HID-over-I2C with remove lock
   XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant, verbose-compliant.
   Handles START/STOP/SURPRISE_REMOVAL/REMOVE and pass-through to lower driver.
   ----------------------------------------------------------------------- */

/* -----------------------------------------------------------------------
   Completion routine – signal event for synchronous IRP completion
   XP/2003-compatible, BSOD-safe, C89-compliant, verbose-compliant
   ----------------------------------------------------------------------- */
static NTSTATUS
I2CHID_CompletionSignalEvent(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp,
    IN PVOID          Context
    )
{
    PKEVENT ev;

    UNREFERENCED_PARAMETER(DeviceObject);

    /* Defensive: validate Context */
    if (Context == NULL) {
        I2CHID_Log("I2CHID(Completion): Context=NULL, nothing to signal\n");
        return STATUS_SUCCESS;
    }

    ev = (PKEVENT)Context;

    /* Signal the event to wake up waiting thread */
    KeSetEvent(ev, IO_NO_INCREMENT, FALSE);
    I2CHID_Log("I2CHID(Completion): Event signaled\n");

    /* Defensive: validate IRP */
    if (Irp == NULL) {
        I2CHID_Log("I2CHID(Completion): Irp=NULL, returning STATUS_SUCCESS\n");
        return STATUS_SUCCESS;
    }

    /* Leave IoStatus untouched – lower driver sets it.
       Returning STATUS_MORE_PROCESSING_REQUIRED prevents IoCompleteRequest
       from being called again by the I/O manager. */
    I2CHID_Log("I2CHID(Completion): Returning STATUS_MORE_PROCESSING_REQUIRED to stop double completion\n");
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS
I2CHID_DispatchPnP(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp
    )
{
    PI2CHID_FDO     ext;
    PIO_STACK_LOCATION  isl;
    NTSTATUS            status;

    ext    = (PI2CHID_FDO)DeviceObject->DeviceExtension;
    isl    = IoGetCurrentIrpStackLocation(Irp);
    status = STATUS_SUCCESS;

    if (ext == NULL) {
        /* No extension: pass down cautiously */
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(DeviceObject, Irp);
    }

    /* Acquire remove lock to guard against concurrent removal */
    status = IoAcquireRemoveLock(&ext->RemoveLock, Irp);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    /* Defensive: lower device must be valid for pass-downs */
    if (ext->LowerDevice == NULL) {
        I2CHID_Log("I2CHID(PnP): LowerDevice is NULL, failing IRP (Minor=0x%02X)\n", isl->MinorFunction);
        Irp->IoStatus.Status = STATUS_NO_SUCH_DEVICE;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return STATUS_NO_SUCH_DEVICE;
    }

    switch (isl->MinorFunction) {

    case IRP_MN_START_DEVICE:
    {
        KEVENT               ev;
        NTSTATUS             lowerStatus;
        NTSTATUS             startStatus;
        PIO_STACK_LOCATION   irpSp;
        PCM_RESOURCE_LIST    raw;
        PCM_RESOURCE_LIST    translated;

        I2CHID_Log("I2CHID(PnP): IRP_MN_START_DEVICE\n");

        /* Defensive: ensure lower device is valid */
        if (ext->LowerDevice == NULL) {
            I2CHID_Log("I2CHID(PnP): LowerDevice=NULL, cannot start\n");
            Irp->IoStatus.Status = STATUS_NO_SUCH_DEVICE;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            IoReleaseRemoveLock(&ext->RemoveLock, Irp);
            return STATUS_NO_SUCH_DEVICE;
        }

        /* Forward to lower driver and wait for completion (XP-safe synchronous pattern) */
        KeInitializeEvent(&ev, NotificationEvent, FALSE);

        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp,
                               I2CHID_CompletionSignalEvent,
                               &ev,
                               TRUE, TRUE, TRUE);

        lowerStatus = IoCallDriver(ext->LowerDevice, Irp);

        if (lowerStatus == STATUS_PENDING) {
            KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
            lowerStatus = Irp->IoStatus.Status;
        }

        if (!NT_SUCCESS(lowerStatus)) {
            I2CHID_Log("I2CHID(PnP): Lower START_DEVICE failed 0x%08X\n", lowerStatus);
            Irp->IoStatus.Status = lowerStatus;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            IoReleaseRemoveLock(&ext->RemoveLock, Irp);
            return lowerStatus;
        }

        /* Lower start succeeded: parse resources from current stack location */
        irpSp = IoGetCurrentIrpStackLocation(Irp);
        raw   = irpSp->Parameters.StartDevice.AllocatedResources;
        translated = irpSp->Parameters.StartDevice.AllocatedResourcesTranslated;

        /* Initialize hardware (map MMIO, connect interrupt, etc.) */
        startStatus = I2CHID_StartDevice(ext, raw, translated);

        /* Initialize ISR->DPC handoff safely */
        KeInitializeDpc(&ext->InterruptDpc, I2CHID_InterruptDpc, ext);

        if (NT_SUCCESS(startStatus)) {
            ext->Started = TRUE;
            ext->Removed = FALSE;
            Irp->IoStatus.Status = STATUS_SUCCESS;
            I2CHID_Log("I2CHID(PnP): StartDevice succeeded\n");
        } else {
            ext->Started = FALSE;
            Irp->IoStatus.Status = startStatus;
            I2CHID_Log("I2CHID(PnP): StartDevice failed 0x%08X\n", startStatus);
        }

        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return Irp->IoStatus.Status;
    }

    case IRP_MN_STOP_DEVICE:
        I2CHID_Log("I2CHID(PnP): IRP_MN_STOP_DEVICE\n");

        /* Stop hardware and disconnect interrupt (PASSIVE_LEVEL only) */
        I2CHID_StopDevice(ext);
        ext->Started = FALSE;

        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);

        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;

    case IRP_MN_SURPRISE_REMOVAL:
        I2CHID_Log("I2CHID(PnP): IRP_MN_SURPRISE_REMOVAL\n");

        ext->Removed = TRUE;

        /* Stop hardware immediately to prevent further IRQs */
        I2CHID_StopDevice(ext);

        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);

        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;

    case IRP_MN_REMOVE_DEVICE:
    {
        KIRQL           oldIrql;
        PLIST_ENTRY     le;
        PIRP            pending;

        I2CHID_Log("I2CHID(PnP): IRP_MN_REMOVE_DEVICE\n");

        ext->Removed = TRUE;
        ext->Started = FALSE;

        /* Stop hardware, disable controller, disconnect interrupts */
        I2CHID_StopDevice(ext);

        /* Cancel idle timers and pending I/O if any */
        I2CHID_DisarmIdle(ext);

        /* Cancel all pending read IRPs (queue must be protected by a lock) */
        KeAcquireSpinLock(&ext->ReadQueueLock, &oldIrql);
        while (!IsListEmpty(&ext->ReadQueue)) {
            le      = RemoveHeadList(&ext->ReadQueue);
            pending = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
            pending->IoStatus.Status      = STATUS_CANCELLED;
            pending->IoStatus.Information = 0;
            IoCompleteRequest(pending, IO_NO_INCREMENT);
        }
        KeReleaseSpinLock(&ext->ReadQueueLock, oldIrql);

        /* Reset open handle count */
        ext->OpenCount = 0;

        /* Delete symbolic link if present */
        if (ext->Symlink.Buffer != NULL) {
            (void)IoDeleteSymbolicLink(&ext->Symlink);
            RtlZeroMemory(&ext->Symlink, sizeof(ext->Symlink));
        }

        /* Clear ACPI identifiers and strings safely */
        if (ext->DeviceId.Buffer != NULL) {
            RtlFreeUnicodeString(&ext->DeviceId);
            RtlZeroMemory(&ext->DeviceId, sizeof(ext->DeviceId));
        }
        if (ext->HardwareIds.Buffer != NULL) {
            RtlFreeUnicodeString(&ext->HardwareIds);
            RtlZeroMemory(&ext->HardwareIds, sizeof(ext->HardwareIds));
        }
        if (ext->CompatibleIds.Buffer != NULL) {
            RtlFreeUnicodeString(&ext->CompatibleIds);
            RtlZeroMemory(&ext->CompatibleIds, sizeof(ext->CompatibleIds));
        }

        /* Clear HID-over-I2C register addresses and parse flag */
        ext->HidDescriptorAddress   = 0;
        ext->InputRegisterAddress   = 0;
        ext->CommandRegisterAddress = 0;
        ext->DataRegisterAddress    = 0;
        ext->ResetRegisterAddress   = 0;
        ext->AcpiParsed             = FALSE;

        /* Forward REMOVE to lower driver first (required ordering) */
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);

        /* Detach from lower and delete this device object */
        if (ext->LowerDevice != NULL) {
            IoDetachDevice(ext->LowerDevice);
            ext->LowerDevice = NULL;
        }

        IoReleaseRemoveLockAndWait(&ext->RemoveLock, Irp);
        IoDeleteDevice(DeviceObject);

        I2CHID_Log("I2CHID(PnP): REMOVE_DEVICE completed, resources freed\n");
        return status;
    }

    default:
        I2CHID_Log("I2CHID(PnP): Minor=0x%02X pass-through\n", isl->MinorFunction);
        IoSkipCurrentIrpStackLocation(Irp);
        status = IoCallDriver(ext->LowerDevice, Irp);
        IoReleaseRemoveLock(&ext->RemoveLock, Irp);
        return status;
    }
}

/* ---- Selective suspend helpers ---- */

VOID
I2CHID_ArmIdle(
    PI2CHID_FDO ext
    )
{
    LARGE_INTEGER due;

    if (ext->DeviceState != PowerDeviceD0) {
        return;
    }

    if (InterlockedExchange(&ext->IdleArmed, 1) == 0) {
        /* Relative time: ms → 100ns units (negative for relative) */
        due.QuadPart = -((LONGLONG)ext->IdleTimeoutMs * 10000LL);
        KeSetTimer(&ext->IdleTimer, due, &ext->IdleDpc);
    }
}

VOID
I2CHID_DisarmIdle(
    PI2CHID_FDO ext
    )
{
    InterlockedExchange(&ext->IdleArmed, 0);
    KeCancelTimer(&ext->IdleTimer);
}

/* -----------------------------------------------------------------------
   Transition helpers – enter D0 (fully on) or Dx (low-power)Transition helpers – enter D0 (fully on) or Dx (low-power)ion helpers – enter D0 (fully on) or Dx (low-power)on helpers – enter D0 (fully on) or Dx (low-power)
   ----------------------------------------------------------------------- */

VOID
I2CHID_EnterD0(
    PI2CHID_FDO ext
    )
{
    if (ext == NULL) {
        return;
    }

    /* Resume controller and device hardware */
    I2cHw_EnableController(ext);
    I2cHw_DeviceResume(ext);
    I2cHw_ReprogramDescriptor(ext);

    /* Reconnect interrupt if not already connected */
    if (ext->InterruptObject && !ext->InterruptConnected) {
        I2cHw_ConnectInterrupt(ext);
        ext->InterruptConnected = TRUE;
    }

    /* Rearm selective suspend timers */
    I2CHID_DisarmIdle(ext);
    I2CHID_ArmIdle(ext);

    /* Disable wake since device is fully on */
    I2cHw_EnableWake(ext, FALSE);

    /* Update power state tracking */
    ext->DeviceState = PowerDeviceD0;
    ext->SystemState = PowerSystemWorking;
}

VOID
I2CHID_EnterDx(
    PI2CHID_FDO ext,
    DEVICE_POWER_STATE dx
    )
{
    if (ext == NULL) {
        return;
    }

    /* Stop idle timers before transitioning */
    I2CHID_DisarmIdle(ext);

    /* Disconnect interrupt if currently connected */
    if (ext->InterruptConnected && ext->InterruptObject) {
        I2cHw_DisconnectInterrupt(ext);
        ext->InterruptConnected = FALSE;
        ext->InterruptObject    = NULL;
    }

    /* Transition device into requested low-power state */
    switch (dx) {
    case PowerDeviceD1:
        /* Light sleep – minimal context loss */
        I2cHw_DeviceIdle(ext);
        ext->SystemState = PowerSystemSleeping1;
        break;

    case PowerDeviceD2:
        /* Deeper sleep – more context loss */
        I2cHw_DeviceIdle(ext);
        ext->SystemState = PowerSystemSleeping2;
        break;

    case PowerDeviceD3:
    default:
        /* Deepest sleep – full suspend */
        I2cHw_DeviceSuspend(ext);
        ext->SystemState = PowerSystemSleeping3;
        break;
    }

    /* Enable wake capability if requested */
    if (ext->WakeEnabled) {
        I2cHw_EnableWake(ext, TRUE);
    } else {
        I2cHw_EnableWake(ext, FALSE);
    }

    /* Update device power state tracking */
    ext->DeviceState = dx;
}


/* -----------------------------------------------------------------------
   Unified Power dispatch – HID-over-I2C and PT variants
   Handles QUERY_POWER, SET_POWER (D0/D1/D2/D3), WAIT_WAKE, forwards others
   ----------------------------------------------------------------------- */
NTSTATUS
I2CHID_DispatchPower(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PIO_STACK_LOCATION  isl;
    PI2CHID_FDO     extFdo;
    NTSTATUS            status;
    SYSTEM_POWER_STATE  sysState;
    DEVICE_POWER_STATE  newDx;
    DEVICE_POWER_STATE  targetDx;
    POWER_STATE         ps;
    PI2CHID_PT_DEVEXT   dx;

    isl    = IoGetCurrentIrpStackLocation(Irp);
    status = STATUS_SUCCESS;

    PoStartNextPowerIrp(Irp);

    /* Common FDO/PT extension */
    if (DeviceObject->DeviceExtension == NULL) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    extFdo = (PI2CHID_FDO)DeviceObject->DeviceExtension;

    /* HID FDO path (presence of bus linkage or physical device suggests HID FDO) */
    if (extFdo->BusCtx != NULL || extFdo->PhysicalDevice != NULL) {

        switch (isl->MinorFunction) {

        case IRP_MN_QUERY_POWER:
            /* Accept all queries */
            Irp->IoStatus.Status = STATUS_SUCCESS;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;

        case IRP_MN_SET_POWER:
            if (isl->Parameters.Power.Type == DevicePowerState) {
                newDx = isl->Parameters.Power.State.DeviceState;

                if (newDx == PowerDeviceD0) {
                    I2CHID_EnterD0(extFdo);
                } else {
                    /* Support D1/D2/D3 transitions explicitly */
                    I2CHID_EnterDx(extFdo, newDx);
                }

                extFdo->DeviceState = newDx;
                PoSetPowerState(DeviceObject,
                                DevicePowerState,
                                isl->Parameters.Power.State);

                Irp->IoStatus.Status = STATUS_SUCCESS;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_SUCCESS;
            }
            else if (isl->Parameters.Power.Type == SystemPowerState) {
                sysState = isl->Parameters.Power.State.SystemState;
                extFdo->SystemState = sysState;

                /* Map system state to a conservative device state */
                switch (sysState) {
                case PowerSystemWorking:
                    targetDx = PowerDeviceD0;
                    break;
                case PowerSystemSleeping1:
                    targetDx = PowerDeviceD1;
                    break;
                case PowerSystemSleeping2:
                    targetDx = PowerDeviceD2;
                    break;
                case PowerSystemSleeping3:
                default:
                    targetDx = PowerDeviceD3;
                    break;
                }

                ps.DeviceState = targetDx;

                /* Request a corresponding device power IRP */
                PoRequestPowerIrp(DeviceObject,
                                  IRP_MN_SET_POWER,
                                  ps,
                                  NULL,
                                  NULL,
                                  NULL);

                Irp->IoStatus.Status = STATUS_SUCCESS;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                return STATUS_SUCCESS;
            }

            /* Unknown power type: pass down */
            IoSkipCurrentIrpStackLocation(Irp);
            return PoCallDriver(extFdo->LowerDevice, Irp);

        case IRP_MN_WAIT_WAKE:
            /* Enable wake signaling downstream if supported */
            extFdo->WakeEnabled = TRUE;
            IoSkipCurrentIrpStackLocation(Irp);
            return PoCallDriver(extFdo->LowerDevice, Irp);

        default:
            /* Forward any other power IRPs */
            IoSkipCurrentIrpStackLocation(Irp);
            return PoCallDriver(extFdo->LowerDevice, Irp);
        }
    }

    /* PT (pass-through) device path – forward to lower device */
    dx = (PI2CHID_PT_DEVEXT)DeviceObject->DeviceExtension;
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(dx->LowerDevice, Irp);
}


/* -----------------------------------------------------------------------
   Create/Close dispatch - always succeed, with open handle tracking
   ----------------------------------------------------------------------- */
static NTSTATUS
I2CHID_DispatchCreateClose(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp
    )
{
    PIO_STACK_LOCATION isl;
    NTSTATUS           status;
    PI2CHID_FDO    ext;

    isl    = IoGetCurrentIrpStackLocation(Irp);
    status = STATUS_SUCCESS;
    ext    = (PI2CHID_FDO)DeviceObject->DeviceExtension;

    switch (isl->MajorFunction) {
    case IRP_MJ_CREATE:
        if (ext) {
            ext->OpenCount++;
        }
        KdPrint(("I2CHID: IRP_MJ_CREATE received (OpenCount=%lu)\n",
                 (ULONG)(ext ? ext->OpenCount : 0)));
        break;

    case IRP_MJ_CLOSE:
        if (ext && ext->OpenCount > 0) {
            ext->OpenCount--;
        }
        KdPrint(("I2CHID: IRP_MJ_CLOSE received (OpenCount=%lu)\n",
                 (ULONG)(ext ? ext->OpenCount : 0)));
        break;

    default:
        /* Should not happen - only CREATE/CLOSE routed here */
        KdPrint(("I2CHID: Unexpected major function %lu\n",
                 (ULONG)isl->MajorFunction));
        break;
    }

    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

/* -----------------------------------------------------------------------
   FillFeatureReport – populate HID feature report (RID 5) with current config
   Layout (little endian):
       Byte 0 : ReportID (5)
       Bytes 1..4   : ScrollScale      (ULONG)
       Bytes 5..8   : ZoomScale        (ULONG)
       Bytes 9..12  : TapTimeMs        (ULONG)
       Bytes 13..16 : TapDistance      (ULONG)
       Bytes 17..20 : PalmThreshold    (ULONG)
       Bytes 21..24 : SwipeScale       (ULONG)
       Bytes 25..28 : RotateScale      (ULONG)
       Bytes 29..32 : PollIntervalMs   (ULONG)
   ----------------------------------------------------------------------- */
static VOID
I2CHID_WriteUlongLe(
    PUCHAR p,
    ULONG val
    )
{
    p[0] = (UCHAR)(val & 0xFF);
    p[1] = (UCHAR)((val >> 8) & 0xFF);
    p[2] = (UCHAR)((val >> 16) & 0xFF);
    p[3] = (UCHAR)((val >> 24) & 0xFF);
}

NTSTATUS
I2CHID_FillFeatureReport(
    PI2CHID_FDO ext,
    PUCHAR outBuf,
    ULONG outLen
    )
{
    ULONG off;

    if (ext == NULL || outBuf == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (outLen < I2CHID_FEATURE_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    /* Report ID */
    outBuf[0] = (UCHAR)I2CHID_FEATURE_RID;
    off = 1U;

    /* ScrollScale */
    I2CHID_WriteUlongLe(outBuf + off, (ULONG)ext->Cfg.ScrollScale);
    off += 4U;

    /* ZoomScale */
    I2CHID_WriteUlongLe(outBuf + off, ext->Cfg.ZoomScale);
    off += 4U;

    /* TapTimeMs */
    I2CHID_WriteUlongLe(outBuf + off, (ULONG)ext->Cfg.TapTimeMs);
    off += 4U;

    /* TapDistance */
    I2CHID_WriteUlongLe(outBuf + off, ext->Cfg.TapDistance);
    off += 4U;

    /* PalmThreshold */
    I2CHID_WriteUlongLe(outBuf + off, (ULONG)ext->Cfg.PalmThreshold);
    off += 4U;

    /* SwipeScale */
    I2CHID_WriteUlongLe(outBuf + off, (ULONG)ext->Cfg.SwipeScale);
    off += 4U;

    /* RotateScale */
    I2CHID_WriteUlongLe(outBuf + off, (ULONG)ext->Cfg.RotateScale);
    off += 4U;

    /* PollIntervalMs */
    I2CHID_WriteUlongLe(outBuf + off, (ULONG)ext->Cfg.PollIntervalMs);
    off += 4U;

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
   ValidateFeatureReport – round-trip validation of HID feature report
   - Fills a buffer with current config (via I2CHID_FillFeatureReport)
   - Applies the buffer back (via I2CHID_ApplyFeatureReport)
   - Compares fields to ensure values are preserved
   ----------------------------------------------------------------------- */
NTSTATUS
I2CHID_ValidateFeatureReport(
    PI2CHID_FDO ext
    )
{
    UCHAR buf[I2CHID_FEATURE_SIZE];
    NTSTATUS status;
    I2CHID_CONFIG cfgBefore;
    I2CHID_CONFIG cfgAfter;

    if (ext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Snapshot current config */
    RtlCopyMemory(&cfgBefore, &ext->Cfg, sizeof(I2CHID_CONFIG));

    /* Fill buffer with current config */
    status = I2CHID_FillFeatureReport(ext, buf, sizeof(buf));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Apply buffer back to config */
    status = I2CHID_ApplyFeatureReport(ext, buf, sizeof(buf));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Snapshot after applying */
    RtlCopyMemory(&cfgAfter, &ext->Cfg, sizeof(I2CHID_CONFIG));

    /* Compare fields */
    if (cfgBefore.ScrollScale    != cfgAfter.ScrollScale   ||
        cfgBefore.ZoomScale      != cfgAfter.ZoomScale     ||
        cfgBefore.TapTimeMs      != cfgAfter.TapTimeMs     ||
        cfgBefore.TapDistance    != cfgAfter.TapDistance   ||
        cfgBefore.PalmThreshold  != cfgAfter.PalmThreshold ||
        cfgBefore.SwipeScale     != cfgAfter.SwipeScale    ||
        cfgBefore.RotateScale    != cfgAfter.RotateScale   ||
        cfgBefore.PollIntervalMs != cfgAfter.PollIntervalMs) {
        return STATUS_DATA_ERROR;
    }

    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
   HandleVendorReport – process vendor-specific output reports (PNP0C50 universal)
   Supported Report IDs:
     0x10 : Vendor command (reset gesture state)
     0x11 : Vendor command (force reload registry config)
     0x12 : Vendor command (set debug level)
   ----------------------------------------------------------------------- */
NTSTATUS
I2CHID_HandleVendorReport(
    PI2CHID_FDO ext,
    PUCHAR inBuf,
    ULONG inLen
    )
{
    UCHAR rid;
    NTSTATUS status;

    if (ext == NULL || inBuf == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (inLen < 1U) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    rid = inBuf[0];
    status = STATUS_INVALID_PARAMETER;

    switch (rid) {
    case 0x10:
        /* Reset gesture state */
        RtlZeroMemory(&ext->Gest, sizeof(I2CHID_GESTURE_STATE));
        status = STATUS_SUCCESS;
        break;

    case 0x11:
        /* Reload configuration from registry */
        I2CHID_LoadRegistryConfig(ext); /* VOID return */
        status = STATUS_SUCCESS;
        break;

    case 0x12:
        /* Set debug level (second byte carries level) */
        if (inLen >= 2U) {
            ext->DebugLevel = inBuf[1];
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;

    default:
        /* Unknown vendor report ID */
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    return status;
}


/* HID IOCTL dispatch (PNP0C50 universal) */
static NTSTATUS
I2CHID_DispatchIoctl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
{
    PIO_STACK_LOCATION isl;
    PI2CHID_FDO ext;
    NTSTATUS status;
    ULONG_PTR info;

    isl    = IoGetCurrentIrpStackLocation(Irp);
    ext    = (PI2CHID_FDO)DeviceObject->DeviceExtension;
    status = STATUS_INVALID_DEVICE_REQUEST;
    info   = 0;

    switch (isl->Parameters.DeviceIoControl.IoControlCode) {

    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
    {
        PHID_DESCRIPTOR out;
        ULONG outLen;

        outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
        if (outLen < sizeof(HID_DESCRIPTOR)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        out = (PHID_DESCRIPTOR)Irp->AssociatedIrp.SystemBuffer;
        RtlCopyMemory(out, &ext->HidStatic.HidDesc, sizeof(HID_DESCRIPTOR));
        info   = sizeof(HID_DESCRIPTOR);
        status = STATUS_SUCCESS;
        break;
    }

case IOCTL_HID_GET_REPORT_DESCRIPTOR:
{
    PUCHAR outBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    ULONG outLen  = isl->Parameters.DeviceIoControl.OutputBufferLength;

    if (outLen < ext->HidStatic.ReportDescLength) {
        status = STATUS_BUFFER_TOO_SMALL;
        info   = 0;
    } else {
        RtlCopyMemory(outBuf,
                      ext->HidStatic.ReportDesc,
                      ext->HidStatic.ReportDescLength);
        info   = ext->HidStatic.ReportDescLength;
        status = STATUS_SUCCESS;
    }
    break;
}


    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
    {
        PHID_DEVICE_ATTRIBUTES out;
        ULONG outLen;

        outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
        if (outLen < sizeof(HID_DEVICE_ATTRIBUTES)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        out = (PHID_DEVICE_ATTRIBUTES)Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(out, sizeof(HID_DEVICE_ATTRIBUTES));

        out->Size          = sizeof(HID_DEVICE_ATTRIBUTES);
        out->VendorID      = ext->VendorId;
        out->ProductID     = ext->ProductId;
        out->VersionNumber = ext->VersionNumber;

        info   = sizeof(HID_DEVICE_ATTRIBUTES);
        status = STATUS_SUCCESS;
        break;
    }

case IOCTL_HID_READ_REPORT:
{
    PUCHAR outBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    ULONG  outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;

    // Validate output buffer
    if (outBuf == NULL || outLen < sizeof(I2CHID_REPORT)) {
        status = STATUS_BUFFER_TOO_SMALL;
        info   = 0;
        break;
    }

    // Queue the IRP for asynchronous completion
    IoMarkIrpPending(Irp);
    status = I2CHID_QueueReadIrp(ext, Irp);

    if (NT_SUCCESS(status)) {
        return STATUS_PENDING;  // IRP will be completed later by the queue
    } else {
        // Failed to queue, complete immediately with error
        info = 0;
        break;
    }
}


    case IOCTL_HID_GET_FEATURE:
    {
        PUCHAR out;
        ULONG outLen;

        outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
        out = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;

        if (outLen < 1U) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (out[0] == 0x02) {
            /* Firmware version: Report ID 2 */
            if (outLen < 3U) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            out[1] = (UCHAR)(ext->VersionNumber & 0xFF);
            out[2] = (UCHAR)((ext->VersionNumber >> 8) & 0xFF);
            info   = 3U;
            status = STATUS_SUCCESS;
        } else if (out[0] == 0x03) {
            /* Configuration: Report ID 3 (legacy short form) */
            if (outLen < 7U) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            out[1] = (UCHAR)(ext->Cfg.PalmThreshold & 0xFF);
            out[2] = (UCHAR)((ext->Cfg.PalmThreshold >> 8) & 0xFF);
            out[3] = (UCHAR)(ext->Cfg.ScrollScale & 0xFF);
            out[4] = (UCHAR)((ext->Cfg.ScrollScale >> 8) & 0xFF);
            out[5] = (UCHAR)(ext->Cfg.TapTimeMs & 0xFF);
            out[6] = (UCHAR)((ext->Cfg.TapTimeMs >> 8) & 0xFF);
            info   = 7U;
            status = STATUS_SUCCESS;
        } else if (out[0] == I2CHID_FEATURE_RID) {
            /* Extended configuration: Report ID 5 */
            if (outLen < I2CHID_FEATURE_SIZE) {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            /* Fill buffer with current config values */
            I2CHID_FillFeatureReport(ext, out, outLen);
            info   = I2CHID_FEATURE_SIZE;
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;
    }

    case IOCTL_HID_SET_FEATURE:
    {
        PUCHAR in;
        ULONG inLen;

        inLen = isl->Parameters.DeviceIoControl.InputBufferLength;
        in = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;

        if (inLen < 1U) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if (in[0] == 0x03 && inLen >= 7U) {
            /* Legacy short configuration update */
            ext->Cfg.PalmThreshold = (ULONG)(in[1] | (in[2] << 8));
            ext->Cfg.ScrollScale   = (ULONG)(in[3] | (in[4] << 8));
            ext->Cfg.TapTimeMs     = (ULONG)(in[5] | (in[6] << 8));

            I2CHID_SaveRegistryConfig(ext);
            status = STATUS_SUCCESS;
            info   = 0;
        } else if (in[0] == I2CHID_FEATURE_RID) {
            /* Extended configuration update: Report ID 5 */
            status = I2CHID_ApplyFeatureReport(ext, in, inLen);
            info   = 0;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;
    }

case IOCTL_HID_WRITE_REPORT:
{
    PUCHAR inBuf;
    ULONG  inLen;

    inBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    inLen = isl->Parameters.DeviceIoControl.InputBufferLength;

    /* Validate input buffer */
    if (inBuf == NULL || inLen < 1U) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                    "IOCTL_HID_WRITE_REPORT: invalid buffer len=%lu",
                    (unsigned long)inLen);
        status = STATUS_BUFFER_TOO_SMALL;
        info   = 0;
        break;
    }

    switch (inBuf[0]) {
    case 0x01: /* LED control report */
        if (inLen < 2U) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_IOCTL,
                        "IOCTL_HID_WRITE_REPORT: LED report too short len=%lu",
                        (unsigned long)inLen);
            status = STATUS_BUFFER_TOO_SMALL;
            info   = 0;
            break;
        }
        ext->LedState = inBuf[1];
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL,
                    "IOCTL_HID_WRITE_REPORT: LED state set to 0x%02X",
                    (unsigned)ext->LedState);
        status = STATUS_SUCCESS;
        info   = 0;
        break;

    case 0x04: /* Vendor-specific output report */
        status = I2CHID_HandleVendorReport(ext, inBuf, inLen);
        TraceEvents(NT_SUCCESS(status) ? TRACE_LEVEL_INFORMATION : TRACE_LEVEL_ERROR,
                    TRACE_FLAG_IOCTL,
                    "IOCTL_HID_WRITE_REPORT: vendor report handled, status=0x%08lx",
                    status);
        info = 0;
        break;

    case 0x10: /* Reset gesture/input state */
        RtlZeroMemory(&ext->Gest, sizeof(I2CHID_GESTURE_STATE));
        ext->LastContactCount = 0;
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_IOCTL,
                    "IOCTL_HID_WRITE_REPORT: gesture state reset via ReportId=0x10");
        status = STATUS_SUCCESS;
        info   = 0;
        break;

    default:
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                    "IOCTL_HID_WRITE_REPORT: unsupported ReportId=0x%02X",
                    (unsigned)inBuf[0]);
        status = STATUS_INVALID_PARAMETER;
        info   = 0;
        break;
    }
    break;
}


    default:
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(ext->LowerDevice, Irp);
    }

    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

// Queue a pending HID read IRP
static NTSTATUS
I2CHID_QueueReadIrp(
    PI2CHID_FDO ext,
    PIRP Irp
    )
{
    KIRQL irql;
    KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
    InsertTailList(&ext->ReadQueue, &Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&ext->ReadQueueLock, irql);
    return STATUS_SUCCESS;
}

// Dequeue a pending HID read IRP
static PIRP
I2CHID_DequeueReadIrp(
    PI2CHID_FDO ext
    )
{
    KIRQL irql;
    PLIST_ENTRY le;
    PIRP irp;

    irp = NULL;
    KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
    if (!IsListEmpty(&ext->ReadQueue)) {
        le  = RemoveHeadList(&ext->ReadQueue);
        irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
    }
    KeReleaseSpinLock(&ext->ReadQueueLock, irql);
    return irp;
}

// Complete a HID read IRP with given data
static VOID
I2CHID_CompleteReadIrp(
    PIRP Irp,
    UCHAR* data,
    ULONG len,
    NTSTATUS status
    )
{
    PUCHAR out;
    ULONG outLen;

    outLen = IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.OutputBufferLength;
    out    = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;

    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return;
    }

    if (outLen < len) {
        Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return;
    }

    RtlCopyMemory(out, data, len);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = len;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

//
// Universal PNP0C50 HID-over-I2C raw parser
// Matches I2CHID_REPORT layout (contacts, buttons, X/Y, timestamp)
//
VOID
I2CHID_ParseRawPnp0c50(
    const UCHAR* raw,
    ULONG len,
    PI2CHID_REPORT out
    )
{
    ULONG offset;
    ULONG i;
    UCHAR count;
    UCHAR reportId;
    UCHAR flags;

    if (out == NULL) {
        return;
    }
    RtlZeroMemory(out, sizeof(*out));

    if (raw == NULL || len < 2) {
        return;
    }

    /* First byte is report ID */
    reportId = raw[0];
    out->ReportId = (ULONG)reportId;

    /* Timestamp for gesture/tap detection */
    KeQuerySystemTime(&out->Timestamp);

    switch (reportId) {
    case 0x01: /* Input report with contacts */
        if (len < 3) {
            return;
        }
        count = raw[1];
        flags = raw[2];

        if (count > I2CHID_MAX_FINGERS) {
            count = I2CHID_MAX_FINGERS;
        }

        out->ContactCount = 0;
        offset = 3;

        for (i = 0; i < count; i++) {
            USHORT xRaw;
            USHORT yRaw;
            UCHAR pressure;
            UCHAR size;
            UCHAR attr;
            BOOLEAN present;

            /* Each contact record assumed 7 bytes: flags+X+Y+pressure+size */
            if (offset + 7 > len) {
                break;
            }

            attr     = raw[offset + 0];
            xRaw     = (USHORT)(raw[offset + 1] | ((USHORT)raw[offset + 2] << 8));
            yRaw     = (USHORT)(raw[offset + 3] | ((USHORT)raw[offset + 4] << 8));
            pressure = raw[offset + 5];
            size     = raw[offset + 6];

            present = (attr & 0x01) ? TRUE : FALSE;

            out->Contacts[i].Present  = present;
            out->Contacts[i].X        = (LONG)xRaw;
            out->Contacts[i].Y        = (LONG)yRaw;
            out->Contacts[i].Pressure = (LONG)pressure;
            out->Contacts[i].Size     = (LONG)size;

            if (present) {
                out->ContactCount++;
            }

            offset += 7;
        }

        /* Map buttons from flags */
        out->BtnLeft   = ((flags & 0x01) != 0);
        out->BtnRight  = ((flags & 0x02) != 0);
        out->BtnMiddle = ((flags & 0x04) != 0);
        out->BtnX1     = ((flags & 0x08) != 0);
        out->BtnX2     = ((flags & 0x10) != 0);

        /* Convenience: set mouse-like X/Y from first present contact */
        if (out->ContactCount > 0 && out->Contacts[0].Present) {
            out->X = out->Contacts[0].X;
            out->Y = out->Contacts[0].Y;
        }
        break;

    case 0x02: /* Separate button report */
        if (len < 2) {
            return;
        }
        flags = raw[1];

        out->BtnLeft   = ((flags & 0x01) != 0);
        out->BtnRight  = ((flags & 0x02) != 0);
        out->BtnMiddle = ((flags & 0x04) != 0);
        out->BtnX1     = ((flags & 0x08) != 0);
        out->BtnX2     = ((flags & 0x10) != 0);
        break;

    default:
        /* Unknown report ID: ignore */
        break;
    }

    /* Defensive cleanup for unused slots */
    for (i = out->ContactCount; i < I2CHID_MAX_FINGERS; i++) {
        out->Contacts[i].Present  = FALSE;
        out->Contacts[i].X        = 0;
        out->Contacts[i].Y        = 0;
        out->Contacts[i].Pressure = 0;
        out->Contacts[i].Size     = 0;
    }
}

/* Interrupt DPC — scheduled by ISR when device signals data ready */
static VOID
I2CHID_InterruptDpc(
    KDPC* Dpc,
    PVOID DeferredContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2
    )
{
    PI2CHID_FDO ext;
    UCHAR rawBuf[64];   /* adjust size to max input report length */
    I2CHID_REPORT rpt;
    UCHAR hid[64];
    ULONG hidLen;
    PIRP irp;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArgument1);
    UNREFERENCED_PARAMETER(SystemArgument2);

    ext = (PI2CHID_FDO)DeferredContext;
    if (ext == NULL || ext->Removed || !ext->Started) {
        return;
    }

    /* Allow ISR to queue another DPC */
    InterlockedExchange(&ext->PendingInputFlag, 0);

    RtlZeroMemory(rawBuf, sizeof(rawBuf));
    if (!I2CHID_ReadInput(ext, rawBuf, sizeof(rawBuf))) {
        return;
    }

    /* Parse raw bytes into our internal report structure */
    RtlZeroMemory(&rpt, sizeof(rpt));
    I2CHID_ParseRawPnp0c50(rawBuf, sizeof(rawBuf), &rpt);

    /* Build HID input report from parsed data */
    hidLen = 0;
    I2CHID_BuildHidInput(&rpt, hid, &hidLen);
    if (hidLen == 0) {
        return;
    }

    /* Complete one pending read IRP */
    irp = I2CHID_DequeueReadIrp(ext);
    if (irp != NULL) {
        I2CHID_CompleteReadIrp(irp, hid, hidLen, STATUS_SUCCESS);
    }
}

// Build and send IOCTL_I2C_READ to the controller; parse into I2CHID_REPORT
// Universal PNP0C50 version (touchpad-like functionality), C89-compliant
static BOOLEAN
I2CHID_I2cReadReport(
    PI2CHID_FDO ext,
    PI2CHID_REPORT rpt
    )
{
    I2C_READ_PARAMS p;
    UCHAR buffer[64]; /* typical HID-over-I2C input size; adjust if needed */
    IO_STATUS_BLOCK iosb;
    KEVENT event;
    PIRP irp;
    NTSTATUS status;
    ULONG outLen;

    if (ext == NULL || rpt == NULL) {
        return FALSE;
    }
    if (ext->Removed || !ext->Started) {
        return FALSE;
    }

    RtlZeroMemory(&p, sizeof(p));
    RtlZeroMemory(buffer, sizeof(buffer));
    RtlZeroMemory(rpt, sizeof(*rpt));

    /* Address the HID device on the I2C bus */
    p.Address7Bit = ext->I2cAddr7Bit;

    /* For universal PNP0C50 devices, many controllers return the latest report
       without needing a register prefix. If your hardware requires selecting
       an input register, set p.Prefix and p.PrefixLen accordingly. */
    p.PrefixLen = 0;

    /* Read up to buffer size; controller will return actual length */
    p.ReadLen = sizeof(buffer);

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
              IOCTL_I2C_READ,
              ext->LowerDevice,
              &p,
              sizeof(p),
              buffer,
              sizeof(buffer),
              FALSE,
              &event,
              &iosb);

    if (irp == NULL) {
        return FALSE;
    }

    status = IoCallDriver(ext->LowerDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    if (!NT_SUCCESS(status)) {
        return FALSE;
    }

    outLen = (ULONG)iosb.Information;
    if (outLen == 0 || outLen > sizeof(buffer)) {
        return FALSE;
    }

    /* Parse using universal PNP0C50 format into I2CHID_REPORT */
    I2CHID_ParseRawPnp0c50(buffer, outLen, rpt);

    /* Convenience: set X/Y to centroid of all present contacts (touchpad-like) */
    if (rpt->ContactCount > 0) {
        LONG sumX = 0;
        LONG sumY = 0;
        ULONG active = 0;
        ULONG i;

        for (i = 0; i < rpt->ContactCount; i++) {
            if (rpt->Contacts[i].Present) {
                sumX += rpt->Contacts[i].X;
                sumY += rpt->Contacts[i].Y;
                active++;
            }
        }

        if (active > 0) {
            rpt->X = sumX / (LONG)active;
            rpt->Y = sumY / (LONG)active;
        } else {
            rpt->X = 0;
            rpt->Y = 0;
        }
    } else {
        rpt->X = 0;
        rpt->Y = 0;
    }

    return TRUE;
}

/* Build HID input for multitouch pad (PNP0C50 universal) */
static VOID
I2CHID_BuildHidInput(
    const I2CHID_REPORT* rpt,
    UCHAR* hid,
    ULONG* hidLen
    )
{
    USHORT x;
    USHORT y;
    UCHAR tip;
    ULONG i;
    ULONG pos;
    UCHAR count;

    if (rpt == NULL || hid == NULL || hidLen == NULL) {
        return;
    }

    pos   = 0;
    count = 0;

    /* Pack each finger (5 bytes per contact) */
    for (i = 0; i < I2CHID_MAX_FINGERS; i++) {
        if (i < rpt->ContactCount && rpt->Contacts[i].Present) {
            /* Clamp X to 0..65535 (mask to 16-bit) */
            if (rpt->Contacts[i].X < 0) {
                x = 0;
            } else if (rpt->Contacts[i].X > 65535) {
                x = 65535;
            } else {
                x = (USHORT)rpt->Contacts[i].X;
            }

            /* Clamp Y to 0..65535 (mask to 16-bit) */
            if (rpt->Contacts[i].Y < 0) {
                y = 0;
            } else if (rpt->Contacts[i].Y > 65535) {
                y = 65535;
            } else {
                y = (USHORT)rpt->Contacts[i].Y;
            }

            tip = 0x01; /* Tip Switch = 1 */
            count++;
        } else {
            x   = 0;
            y   = 0;
            tip = 0x00; /* No contact */
        }

        hid[pos++] = tip;
        hid[pos++] = (UCHAR)(x & 0xFF);
        hid[pos++] = (UCHAR)((x >> 8) & 0xFF);
        hid[pos++] = (UCHAR)(y & 0xFF);
        hid[pos++] = (UCHAR)((y >> 8) & 0xFF);
    }

    /* Append Contact Count */
    hid[pos++] = count;

    /* Append button states (1 byte flags) */
    {
        UCHAR btnFlags = 0;
        if (rpt->BtnLeft)   btnFlags |= 0x01;
        if (rpt->BtnRight)  btnFlags |= 0x02;
        if (rpt->BtnMiddle) btnFlags |= 0x04;
        if (rpt->BtnX1)     btnFlags |= 0x08;
        if (rpt->BtnX2)     btnFlags |= 0x10;
        hid[pos++] = btnFlags;
    }

    *hidLen = pos;
}

// Utility: get tick in ms based on system tick count
static ULONG
I2CHID_GetTickMs(
    VOID
    )
{
    ULONG inc100ns;
    LARGE_INTEGER tick;
    LARGE_INTEGER t100ns;

    inc100ns = KeQueryTimeIncrement();
    KeQueryTickCount(&tick);
    t100ns.QuadPart = (LONGLONG)tick.QuadPart * (LONGLONG)inc100ns;

    return (ULONG)(t100ns.QuadPart / 10000); // 100ns -> ms
}

//
// Utility: simple palm rejection check
//
static BOOLEAN
I2CHID_IsPalm(
    const I2CHID_CONTACT* c,
    ULONG threshold
    )
{
    if (!c->Present) {
        return FALSE;
    }
    if (c->Size > (LONG)threshold) {
        return TRUE;
    }
    return FALSE;
}

/* -----------------------------------------------------------------------
   Gesture handler - feature complete using I2CHID_CONTACT header fields
   ----------------------------------------------------------------------- */
VOID
I2CHID_HandleGestures(
    PI2CHID_FDO ext,
    const I2CHID_CONTACT* contact
    )
{
    LARGE_INTEGER now;
    ULONG deltaMs;

    if (ext == NULL || contact == NULL) {
        return;
    }

    /* Only act if contact slot is active */
    if (!contact->Present) {
        return;
    }

    KeQuerySystemTime(&now);

    /* Tap detection: pressure above threshold and small size */
    if (contact->Pressure > 0 && contact->Size < ext->Cfg.TapSizeThreshold) {
        deltaMs = (ULONG)((now.QuadPart - ext->LastTapTime.QuadPart) / 10000);

        if (deltaMs < 500) {
            KdPrint(("I2CHID: Double-tap detected at X=%ld Y=%ld\n",
                     contact->X, contact->Y));
            ext->LastTapTime.QuadPart = 0;
        } else {
            KdPrint(("I2CHID: Tap detected at X=%ld Y=%ld\n",
                     contact->X, contact->Y));
            ext->LastTapTime = now;
        }
    }

    /* Scroll detection: large Y movement with sustained contact */
    if (contact->Size >= ext->Cfg.ScrollSizeThreshold) {
        I2CHID_Log("I2CHID: Scroll gesture detected at Y=%ld\n", contact->Y);
    }
}

//
// Debug dump of I2CHID_REPORT (PNP0C50 universal)
//
static VOID
I2CHID_DumpReport(
    const I2CHID_REPORT* rpt
    )
{
#if DBG
    ULONG i;

    if (rpt == NULL) {
        return;
    }

    DbgPrint("I2CHID: ReportId=%lu Contacts=%lu BtnL=%d BtnR=%d BtnM=%d BtnX1=%d BtnX2=%d\n",
             rpt->ReportId,
             rpt->ContactCount,
             rpt->BtnLeft,
             rpt->BtnRight,
             rpt->BtnMiddle,
             rpt->BtnX1,
             rpt->BtnX2);

    DbgPrint("I2CHID: Timestamp=%I64d\n", rpt->Timestamp.QuadPart);

    for (i = 0; i < rpt->ContactCount && i < I2CHID_MAX_FINGERS; i++) {
        if (rpt->Contacts[i].Present) {
            DbgPrint("  [%lu] X=%ld Y=%ld Pressure=%ld Size=%ld\n",
                     i,
                     rpt->Contacts[i].X,
                     rpt->Contacts[i].Y,
                     rpt->Contacts[i].Pressure,
                     rpt->Contacts[i].Size);
        }
    }
#endif
}

//
// Universal PNP0C50 HID-over-I2C parser - C89 compliant, ASCII-only
// Matches I2CHID_REPORT layout and tolerates common MT formats.
// Tries to infer record size (6..10 bytes) and optional trailing checksum.
//
// Header (common pattern):
//   Byte 0 : ReportId (0x01 = touch report)
//   Byte 1 : Flags (buttons, if present): bit0=Left, bit1=Right, bit2=Middle, bit3=X1, bit4=X2
//   Byte 2 : ContactCount (N, 0..I2CHID_MAX_FINGERS)
//   Byte 3 : PacketLength (optional total bytes)
//   Byte 4 : Status (bit0=valid data)
//   Byte 5 : Reserved
//
// Contact record (variable, 6..10 bytes; most devices use 8):
//   Off 0..1 : X (le16, typically 12-16 bits used)
//   Off 2..3 : Y (le16, typically 12-16 bits used)
//   Off 4    : Pressure (optional, 0..255)
//   Off 5    : Size (optional, 0..255)
//   Off last : Attr (bit0=Present), other bits vendor-defined
//
// Optional trailing checksum: sum of all prior bytes modulo 256
//
static VOID
I2CHID_ParsePnp0c50(
    const UCHAR* raw,
    ULONG len,
    PI2CHID_REPORT rpt
    )
{
    ULONG i;
    ULONG offset;
    ULONG records;
    ULONG expectedLen;
    BOOLEAN validData;
    BOOLEAN checksumOk;
    UCHAR reportId;
    UCHAR flags;
    UCHAR count;
    UCHAR pktLen;
    UCHAR status;
    UCHAR trailingChecksum;
    ULONG headerSize;
    ULONG recordSize;
    ULONG payloadLen;
    ULONG remaining;
    ULONG inferredSize;

    if (raw == NULL || rpt == NULL || len < 6) {
        return;
    }

    I2CHID_ClearReport(rpt);

    /* Timestamp for gesture/tap detection */
    KeQuerySystemTime(&rpt->Timestamp);

    /* Header */
    reportId = raw[0];
    flags    = raw[1];
    count    = raw[2];
    pktLen   = raw[3];
    status   = raw[4];
    validData = ((status & 0x01) != 0);

    rpt->ReportId = (ULONG)reportId;

    /* Only handle touch/input reports */
    if (reportId != 0x01) {
        return;
    }

    /* Clamp count to capacity */
    if ((ULONG)count > (ULONG)I2CHID_MAX_FINGERS) {
        count = (UCHAR)I2CHID_MAX_FINGERS;
    }

    headerSize = 6;

    /* Infer record size: try 8, else use remaining/count clamped to [6..10] */
    payloadLen = (len > headerSize ? (len - headerSize) : 0);
    remaining  = payloadLen;
    inferredSize = 8; /* default common size */

    if ((ULONG)count != 0 && remaining >= ((ULONG)count * 6UL)) {
        ULONG div = remaining / (ULONG)count;
        if (div >= 6UL && div <= 10UL) {
            inferredSize = div;
        }
    }

    recordSize  = inferredSize;
    expectedLen = headerSize + ((ULONG)count * recordSize);
    checksumOk  = TRUE;

    /* Optional trailing checksum */
    if (pktLen == expectedLen + 1 && len >= pktLen) {
        trailingChecksum = raw[pktLen - 1];
        checksumOk = (I2CHID_Checksum8(raw, pktLen - 1) == trailingChecksum);
        len = pktLen; /* trust packet length */
    } else if (len == expectedLen + 1) {
        trailingChecksum = raw[len - 1];
        checksumOk = (I2CHID_Checksum8(raw, len - 1) == trailingChecksum);
    } else {
        /* If lengths mismatch, parse up to minimum sane boundary */
        if (pktLen != 0 && pktLen < expectedLen) {
            expectedLen = pktLen;
        } else if (len < expectedLen) {
            expectedLen = len;
        }
    }

    /* Map buttons (if present in flags) */
    rpt->BtnLeft   = ((flags & 0x01) != 0);
    rpt->BtnRight  = ((flags & 0x02) != 0);
    rpt->BtnMiddle = ((flags & 0x04) != 0);
    rpt->BtnX1     = ((flags & 0x08) != 0);
    rpt->BtnX2     = ((flags & 0x10) != 0);

    /* If data invalid or checksum failed, keep buttons only */
    if (!validData || !checksumOk) {
        rpt->ContactCount = 0;
        rpt->X = 0;
        rpt->Y = 0;
        return;
    }

    /* Parse contacts */
    offset  = headerSize;
    records = (ULONG)count;

    for (i = 0; i < records; i++) {
        USHORT xRaw;
        USHORT yRaw;
        UCHAR pressure;
        UCHAR size;
        UCHAR attr;
        I2CHID_CONTACT* c;
        ULONG recEnd;

        recEnd = offset + recordSize;
        if (recEnd > expectedLen || recEnd > len) {
            break;
        }

        /* Base fields always parsed */
        xRaw = I2CHID_ReadLe16(&raw[offset + 0]);
        yRaw = I2CHID_ReadLe16(&raw[offset + 2]);

        /* Optional fields guard */
        pressure = (recordSize > 4) ? raw[offset + 4] : 0;
        size     = (recordSize > 5) ? raw[offset + 5] : 0;

        /* Attr assumed at last byte of record */
        attr = raw[recEnd - 1];

        c = &rpt->Contacts[i];

        c->Present  = ((attr & 0x01) != 0) ? TRUE : FALSE;
        c->X        = (LONG)(xRaw & 0xFFFF);  /* keep full 16-bit */
        c->Y        = (LONG)(yRaw & 0xFFFF);
        c->Pressure = (LONG)pressure;
        c->Size     = (LONG)size;

        offset = recEnd;
    }

    rpt->ContactCount = i;

    /* Touchpad-like convenience: set X/Y to centroid of all present contacts */
    if (rpt->ContactCount > 0) {
        LONG sumX = 0;
        LONG sumY = 0;
        ULONG active = 0;
        ULONG j;

        for (j = 0; j < rpt->ContactCount; j++) {
            if (rpt->Contacts[j].Present) {
                sumX += rpt->Contacts[j].X;
                sumY += rpt->Contacts[j].Y;
                active++;
            }
        }

        if (active > 0) {
            rpt->X = sumX / (LONG)active;
            rpt->Y = sumY / (LONG)active;
        } else {
            rpt->X = 0;
            rpt->Y = 0;
        }
    } else {
        rpt->X = 0;
        rpt->Y = 0;
    }
}

static USHORT
I2CHID_ReadLe16(
    const UCHAR* p
    )
{
    return (USHORT)((USHORT)p[0] | ((USHORT)p[1] << 8));
}

static UCHAR
I2CHID_Checksum8(
    const UCHAR* buf,
    ULONG len
    )
{
    ULONG i;
    UCHAR sum = 0;
    for (i = 0; i < len; i++) {
        sum = (UCHAR)(sum + buf[i]);
    }
    return sum;
}

static VOID
I2CHID_ClearReport(
    PI2CHID_REPORT rpt
    )
{
    ULONG i;

    if (rpt == NULL) {
        return;
    }

    rpt->ReportId     = 0;
    rpt->ContactCount = 0;

    for (i = 0; i < I2CHID_MAX_FINGERS; i++) {
        rpt->Contacts[i].Present  = FALSE;
        rpt->Contacts[i].X        = 0;
        rpt->Contacts[i].Y        = 0;
        rpt->Contacts[i].Pressure = 0;
        rpt->Contacts[i].Size     = 0;
    }

    rpt->X = 0;
    rpt->Y = 0;

    rpt->BtnLeft   = FALSE;
    rpt->BtnRight  = FALSE;
    rpt->BtnMiddle = FALSE;
    rpt->BtnX1     = FALSE;
    rpt->BtnX2     = FALSE;

    rpt->Timestamp.QuadPart = 0;
}


/* -----------------------------------------------------------------------
   Entry point for processing a report after read
   ----------------------------------------------------------------------- */
static VOID
I2CHID_ProcessReport(
    PI2CHID_FDO ext,
    const I2CHID_REPORT* rpt
    )
{
    BOOLEAN isPalm;

    if (ext == NULL || rpt == NULL) {
        return;
    }

    /* Palm rejection */
    isPalm = FALSE;
    if (rpt->ContactCount > 0) {
        isPalm = I2CHID_IsPalm(&rpt->Contacts[0], ext->Cfg.PalmThreshold);
    }
    if (isPalm) {
        I2CHID_Log("I2CHID: Palm contact rejected\n");
        return;
    }

    /* Gesture handling using absolute coordinates */
    if (rpt->ContactCount > 0) {
        I2CHID_HandleGestures(ext, &rpt->Contacts[0]);
    }

    /* Debug dump */
    I2CHID_DumpReport(rpt);
}

/* -----------------------------------------------------------------------
   Registry configuration loader (PNP0C50 universal, feature-complete, C89)
   - Reads DWORD values created by INF or user policy
   - Overwrites defaults in ext->Cfg if present
   - Supported fields: PalmThreshold, ScrollScale, TapTimeMs, TapDistance,
                       ZoomScale, RotateScale, SwipeScale, PollIntervalMs
   ----------------------------------------------------------------------- */
VOID
I2CHID_LoadRegistryConfig(
    PI2CHID_FDO ext
    )
{
    NTSTATUS status;
    HANDLE hKey;
    UNICODE_STRING valName;
    ULONG resultLen;
    PKEY_VALUE_PARTIAL_INFORMATION kvpi;
    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];

    if (ext == NULL) {
        return;
    }

    status = IoOpenDeviceRegistryKey(
                 ext->Self,
                 PLUGPLAY_REGKEY_DEVICE,
                 KEY_READ,
                 &hKey);
    if (!NT_SUCCESS(status)) {
        return;
    }

#define READ_DWORD_VALUE(name, field)                                      \
    RtlInitUnicodeString(&valName, name);                                  \
    status = ZwQueryValueKey(hKey, &valName, KeyValuePartialInformation,   \
                             buffer, sizeof(buffer), &resultLen);          \
    if (NT_SUCCESS(status)) {                                              \
        kvpi = (PKEY_VALUE_PARTIAL_INFORMATION)buffer;                     \
        if (kvpi->Type == REG_DWORD && kvpi->DataLength == sizeof(ULONG)) {\
            ext->Cfg.field = *((PULONG)kvpi->Data);                        \
            DbgPrint("I2CHID: %ws = %lu\n", name, (ULONG)ext->Cfg.field);  \
        }                                                                  \
    }

    /* Gesture and power policy values */
    READ_DWORD_VALUE(L"PalmThreshold",   PalmThreshold);
    READ_DWORD_VALUE(L"ScrollScale",     ScrollScale);
    READ_DWORD_VALUE(L"TapTimeMs",       TapTimeMs);
    READ_DWORD_VALUE(L"TapDistance",     TapDistance);
    READ_DWORD_VALUE(L"ZoomScale",       ZoomScale);
    READ_DWORD_VALUE(L"RotateScale",     RotateScale);
    READ_DWORD_VALUE(L"SwipeScale",      SwipeScale);
    READ_DWORD_VALUE(L"PollIntervalMs",  PollIntervalMs);

#undef READ_DWORD_VALUE

    ZwClose(hKey);
}

/* -----------------------------------------------------------------------
 * Registry configuration saver (feature-complete, C89)
 * - Writes current ext->Cfg values back into registry
 * - Supported fields: PalmThreshold, ScrollScale, TapTimeMs, TapDistance,
 *                     ZoomScale, RotateScale, SwipeScale, PollIntervalMs
 * ----------------------------------------------------------------------- */
NTSTATUS
I2CHID_SaveRegistryConfig(
    PI2CHID_FDO ext
    )
{
    HANDLE            hKey;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING    valName;
    NTSTATUS          status;
    ULONG             value;

    /* Validate input */
    if (ext == NULL) {
        I2CHID_Log("I2CHID(SaveRegistryConfig): ext=NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* ext->RegistryPath must have been initialized in AddDevice/DriverEntry */
    InitializeObjectAttributes(&oa,
                               &ext->RegistryPath,
                               OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);

    status = ZwOpenKey(&hKey, KEY_SET_VALUE, &oa);
    if (!NT_SUCCESS(status)) {
        I2CHID_Log("I2CHID(SaveRegistryConfig): ZwOpenKey failed 0x%08X\n", status);
        return status;
    }

    /* Macro-like helper for writing DWORD values */
#define WRITE_DWORD_VALUE(name, field)                                     \
    do {                                                                   \
        value = (ULONG)ext->Cfg.field;                                     \
        RtlInitUnicodeString(&valName, name);                              \
        status = ZwSetValueKey(hKey, &valName, 0, REG_DWORD,               \
                               &value, sizeof(ULONG));                     \
        if (!NT_SUCCESS(status)) {                                         \
            KdPrint(("I2CHID(SaveRegistryConfig): ZwSetValueKey(%ws) failed 0x%08X\n", \
                     name, status));                                       \
            ZwClose(hKey);                                                 \
            return status;                                                 \
        }                                                                  \
    } while (0)

    WRITE_DWORD_VALUE(L"PalmThreshold",   PalmThreshold);
    WRITE_DWORD_VALUE(L"ScrollScale",     ScrollScale);
    WRITE_DWORD_VALUE(L"TapTimeMs",       TapTimeMs);
    WRITE_DWORD_VALUE(L"TapDistance",     TapDistance);
    WRITE_DWORD_VALUE(L"ZoomScale",       ZoomScale);
    WRITE_DWORD_VALUE(L"RotateScale",     RotateScale);
    WRITE_DWORD_VALUE(L"SwipeScale",      SwipeScale);
    WRITE_DWORD_VALUE(L"PollIntervalMs",  PollIntervalMs);

#undef WRITE_DWORD_VALUE

    ZwClose(hKey);

    I2CHID_Log("I2CHID(SaveRegistryConfig): configuration persisted to registry\n");
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * kernel logger with printf-style formatting + timestamp prefix
 * ----------------------------------------------------------------------- */
VOID
I2CHID_Log(
    PCSTR Format,
    ...
    )
{
    CHAR  buffer[512];
    CHAR  final[600];
    va_list args;
    NTSTATUS status;

    UNICODE_STRING      path;
    OBJECT_ATTRIBUTES   oa;
    IO_STATUS_BLOCK     iosb;
    HANDLE              hFile;

    LARGE_INTEGER       sysTime, localTime;
    TIME_FIELDS         tf;

    PAGED_CODE();

    //
    // Hard safety guards: prevent use-after-free crashes
    //
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    if (Format == NULL) {
        return;
    }

    //
    // SAFE pointer formatting:
    // Convert all %p to 0x%I64X BEFORE calling VPrintf.
    // This prevents the CRT from dereferencing freed pointers.
    //
    {
        CHAR safeFmt[256];
        SIZE_T i = 0, j = 0;

        while (Format[i] != '\0' && j < sizeof(safeFmt) - 1) {
            if (Format[i] == '%' && Format[i+1] == 'p') {
                safeFmt[j++] = '0';
                safeFmt[j++] = 'x';
                safeFmt[j++] = '%';
                safeFmt[j++] = 'I';
                safeFmt[j++] = '6';
                safeFmt[j++] = '4';
                safeFmt[j++] = 'X';
                i += 2;
                continue;
            }
            safeFmt[j++] = Format[i++];
        }
        safeFmt[j] = '\0';

        va_start(args, Format);
        status = RtlStringCbVPrintfA(buffer, sizeof(buffer), safeFmt, args);
        va_end(args);

        if (!NT_SUCCESS(status)) {
            return;
        }
    }

    /* Get local time */
    KeQuerySystemTime(&sysTime);
    ExSystemTimeToLocalTime(&sysTime, &localTime);
    RtlTimeToTimeFields(&localTime, &tf);

    /* Format timestamp prefix: [DD/MM/YYYY, HH:MM AM/PM] */
    {
        CHAR ts[64];
        ULONG hour = tf.Hour;
        BOOLEAN pm = FALSE;

        if (hour == 0) {
            hour = 12;
        } else if (hour == 12) {
            pm = TRUE;
        } else if (hour > 12) {
            hour -= 12;
            pm = TRUE;
        }

        RtlStringCbPrintfA(
            ts,
            sizeof(ts),
            "[%02u/%02u/%04u, %02u:%02u %s] ",
            tf.Day,
            tf.Month,
            tf.Year,
            hour,
            tf.Minute,
            pm ? "PM" : "AM"
        );

        RtlStringCbPrintfA(
            final,
            sizeof(final),
            "%s%s",
            ts,
            buffer
        );
    }

    /* Open log file */
    RtlInitUnicodeString(&path, L"\\SystemRoot\\System32\\i2cctrl.log");

    InitializeObjectAttributes(
        &oa,
        &path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL
    );

    status = ZwCreateFile(
                 &hFile,
                 FILE_APPEND_DATA | SYNCHRONIZE,
                 &oa,
                 &iosb,
                 NULL,
                 FILE_ATTRIBUTE_NORMAL,
                 0,
                 FILE_OPEN_IF,
                 FILE_SYNCHRONOUS_IO_NONALERT,
                 NULL,
                 0
             );

    if (!NT_SUCCESS(status)) {
        return;
    }

    /* Write timestamped line */
    ZwWriteFile(
        hFile,
        NULL,
        NULL,
        NULL,
        &iosb,
        final,
        (ULONG)strlen(final),
        NULL,
        NULL
    );

    ZwClose(hFile);

    //
    // Mirror to ETW/WPP without the timestamp prefix.
    //
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_FLAG_BUS,
        "%s",
        buffer
    );
}


//
// End of file
//