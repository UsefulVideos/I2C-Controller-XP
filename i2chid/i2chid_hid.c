/* i2c_hid.c (C89 compliant)
 * I2C HID FDO: add/remove, PnP/Power, queueing, and HID descriptor plumbing.
 */
#include "I2CHID_hid.h"

/* ---- Forward declarations ---- */

/* Interrupt helpers */
BOOLEAN I2CHID_InterruptServiceRoutine(PKINTERRUPT Interrupt, PVOID ServiceContext);

/* Input helpers */
BOOLEAN I2CHID_ReadInput(PI2CHID_FDO ext, PUCHAR buf, ULONG bufLen);
VOID    I2CHID_CompleteQueuedReads(PI2CHID_FDO ext, PUCHAR data, ULONG len);


/* -----------------------------------------------------------------------
   Precision Touchpad-like HID report descriptor (XP/2003-friendly, C89)
   - Application 1: Digitizers / Touch Pad
       • Input Report ID 1:
           Contact Count + 5 Finger logical collections
           Per finger: Tip Switch, In Range, Confidence, X, Y, Contact Identifier
       • Feature Report ID 2:
           Contact Count Maximum (read-only hint for parsers)
       • Feature Report ID 3:
           Surface dimensions (X/Y logical maximums) for coordinate normalization
       • Physical buttons: 3 zones mapped as button inputs
   - Application 2: Mouse
       • Buttons (3) + relative X/Y + vertical wheel + horizontal wheel (AC Pan)
   - Application 3: Keyboard (modifiers only)
       • Modifier byte includes Left Ctrl (0xE0) through Right GUI (0xE7)
       • Used by driver to synthesize Ctrl+wheel events for pinch-zoom
   Notes:
   - Touch coordinates use 16-bit absolute values (0..32767). Adjust to your device range.
   - Mouse axes and wheels use 8-bit relative values (-127..127).
   - Keyboard modifiers are 1-bit flags (pressed/released) for standard HID usage IDs.
   - While full Windows 8+ Precision Touchpad requires additional usages/collections,
     this unified descriptor provides key PTP-adjacent elements (Confidence, Count Max,
     Surface dimensions) plus legacy mouse and keyboard collections, ensuring compatibility
     with XP/2003 HID stacks and applications expecting a standard mouse and keyboard.
   ----------------------------------------------------------------------- */


const UCHAR g_HidReportDesc[] = {

/* ===================== Touchpad Application ===================== */
    /* Top-level touchpad application */
    0x05, 0x0D,              /* Usage Page (Digitizers) */
    0x09, 0x05,              /* Usage (Touch Pad) */
    0xA1, 0x01,              /* Collection (Application) */

    /* ---------------- Input report (RID 1) ---------------- */
    0x85, 0x01,              /* Report ID (1) */

    /* Contact Count (input) */
    0x09, 0x54,              /* Usage (Contact Count) */
    0x15, 0x00,              /* Logical Minimum (0) */
    0x25, 0x05,              /* Logical Maximum (5) */
    0x75, 0x08,              /* Report Size (8) */
    0x95, 0x01,              /* Report Count (1) */
    0x81, 0x02,              /* Input (Data,Var,Abs) */

    /* ---------------- Finger 1 ---------------- */
    0x09, 0x22,              /*   Usage (Finger) */
    0xA1, 0x02,              /*   Collection (Logical) */
      0x05, 0x0D, 0x09, 0x42, 0x09, 0x32, 0x09, 0x47,
      0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
      0x75, 0x05, 0x95, 0x01, 0x81, 0x03,
      0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
      0x16, 0x00, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
      0x05, 0x0D, 0x09, 0x51, 0x15, 0x00, 0x25, 0xFF, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0xC0,
    /* ---------------- Finger 2 ---------------- */
    0x09, 0x22,              /*   Usage (Finger) */
    0xA1, 0x02,              /*   Collection (Logical) */
      0x05, 0x0D, 0x09, 0x42, 0x09, 0x32, 0x09, 0x47,
      0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
      0x75, 0x05, 0x95, 0x01, 0x81, 0x03,
      0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
      0x16, 0x00, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
      0x05, 0x0D, 0x09, 0x51, 0x15, 0x00, 0x25, 0xFF, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0xC0,

    /* ---------------- Finger 3 ---------------- */
    0x09, 0x22,              /*   Usage (Finger) */
    0xA1, 0x02,              /*   Collection (Logical) */
      0x05, 0x0D, 0x09, 0x42, 0x09, 0x32, 0x09, 0x47,
      0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
      0x75, 0x05, 0x95, 0x01, 0x81, 0x03,
      0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
      0x16, 0x00, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
      0x05, 0x0D, 0x09, 0x51, 0x15, 0x00, 0x25, 0xFF, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0xC0,

    /* ---------------- Finger 4 ---------------- */
    0x09, 0x22,              /*   Usage (Finger) */
    0xA1, 0x02,              /*   Collection (Logical) */
      0x05, 0x0D, 0x09, 0x42, 0x09, 0x32, 0x09, 0x47,
      0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
      0x75, 0x05, 0x95, 0x01, 0x81, 0x03,
      0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
      0x16, 0x00, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
      0x05, 0x0D, 0x09, 0x51, 0x15, 0x00, 0x25, 0xFF, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0xC0,

    /* ---------------- Finger 5 ---------------- */
    0x09, 0x22,              /*   Usage (Finger) */
    0xA1, 0x02,              /*   Collection (Logical) */
      0x05, 0x0D, 0x09, 0x42, 0x09, 0x32, 0x09, 0x47,
      0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02,
      0x75, 0x05, 0x95, 0x01, 0x81, 0x03,
      0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
      0x16, 0x00, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
      0x05, 0x0D, 0x09, 0x51, 0x15, 0x00, 0x25, 0xFF, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0xC0,

    /* ---------------- Feature: Contact Count Maximum (RID 2) ---------------- */
    0x85, 0x02,              /* Report ID (2) */
    0x09, 0x55,              /* Usage (Contact Count Maximum) */
    0x15, 0x00,              /* Logical Minimum (0) */
    0x25, 0x05,              /* Logical Maximum (5) */
    0x75, 0x08,              /* Report Size (8) */
    0x95, 0x01,              /* Report Count (1) */
    0xB1, 0x03,              /* Feature (Const,Var,Abs) */

    /* ---------------- Feature: Surface dimensions (RID 3, vendor-defined) ---------------- */
    0x85, 0x03,              /* Report ID (3) */
    0x06, 0x00, 0xFF,        /* Usage Page (Vendor Defined 0xFF00) */
    0x09, 0x02,              /* Usage (Surface Dimensions) */
    0xA1, 0x01,              /* Collection (Application) */
      0x75, 0x10,            /* Report Size (16) */
      0x95, 0x02,            /* Report Count (2 fields: X max, Y max) */
      0x15, 0x00,            /* Logical Minimum (0) */
      0x26, 0xFF, 0x7F,      /* Logical Maximum (32767) */
      0xB1, 0x02,            /* Feature (Data,Var,Abs) */
    0xC0,
    /* ---------------- Physical buttons (touchpad app) ---------------- */
    0x05, 0x09,              /* Usage Page (Button) */
    0x19, 0x01,              /* Usage Minimum (Button 1) */
    0x29, 0x03,              /* Usage Maximum (Button 3) */
    0x15, 0x00,              /* Logical Minimum (0) */
    0x25, 0x01,              /* Logical Maximum (1) */
    0x75, 0x01,              /* Report Size (1) */
    0x95, 0x03,              /* Report Count (3 buttons) */
    0x81, 0x02,              /* Input (Data,Var,Abs) */
    0x75, 0x05,              /* Report Size (5) */
    0x95, 0x01,              /* Report Count (padding) */
    0x81, 0x03,              /* Input (Const,Var,Abs) */

    0xC0,                    /* End Collection (Touchpad Application) */

/* ===================== Mouse Application ===================== */
    0x05, 0x01,              /* Usage Page (Generic Desktop) */
    0x09, 0x02,              /* Usage (Mouse) */
    0xA1, 0x01,              /* Collection (Application) */
      0x09, 0x01,            /*   Usage (Pointer) */
      0xA1, 0x00,            /*   Collection (Physical) */
        0x05, 0x09,          /*     Usage Page (Button) */
        0x19, 0x01,          /*     Usage Minimum (Button 1) */
        0x29, 0x03,          /*     Usage Maximum (Button 3) */
        0x15, 0x00,          /*     Logical Minimum (0) */
        0x25, 0x01,          /*     Logical Maximum (1) */
        0x95, 0x03,          /*     Report Count (3 buttons) */
        0x75, 0x01,          /*     Report Size (1) */
        0x81, 0x02,          /*     Input (Data,Var,Abs) */
        0x95, 0x01,          /*     Report Count (padding) */
        0x75, 0x05,          /*     Report Size (5) */
        0x81, 0x03,          /*     Input (Const,Var,Abs) */

        0x05, 0x01,          /*     Usage Page (Generic Desktop) */
        0x09, 0x30,          /*     Usage (X) */
        0x09, 0x31,          /*     Usage (Y) */
        0x15, 0x81,          /*     Logical Minimum (-127) */
        0x25, 0x7F,          /*     Logical Maximum (127) */
        0x75, 0x08,          /*     Report Size (8) */
        0x95, 0x02,          /*     Report Count (2 axes) */
        0x81, 0x06,          /*     Input (Data,Var,Rel) */

        0x09, 0x38,          /*     Usage (Wheel - vertical) */
        0x15, 0x81,          /*     Logical Minimum (-127) */
        0x25, 0x7F,          /*     Logical Maximum (127) */
        0x75, 0x08,          /*     Report Size (8) */
        0x95, 0x01,          /*     Report Count (1) */
        0x81, 0x06,          /*     Input (Data,Var,Rel) */

        0x09, 0x37,          /*     Usage (AC Pan - horizontal wheel) */
        0x15, 0x81,          /*     Logical Minimum (-127) */
        0x25, 0x7F,          /*     Logical Maximum (127) */
        0x75, 0x08,          /*     Report Size (8) */
        0x95, 0x01,          /*     Report Count (1) */
        0x81, 0x06,          /*     Input (Data,Var,Rel) */
      0xC0,                  /*   End Collection (Physical) */
    0xC0,                    /* End Collection (Mouse Application) */

/* ===================== Keyboard Application (for Ctrl modifier) ===================== */
    0x05, 0x01,              /* Usage Page (Generic Desktop) */
    0x09, 0x06,              /* Usage (Keyboard) */
    0xA1, 0x01,              /* Collection (Application) */
      0x05, 0x07,            /*   Usage Page (Keyboard/Keypad) */
      0x19, 0xE0,            /*   Usage Minimum (Left Control) */
      0x29, 0xE7,            /*   Usage Maximum (Right GUI) */
      0x15, 0x00,            /*   Logical Minimum (0) */
      0x25, 0x01,            /*   Logical Maximum (1) */
      0x75, 0x01,            /*   Report Size (1) */
      0x95, 0x08,            /*   Report Count (8 modifier bits) */
      0x81, 0x02,            /*   Input (Data,Var,Abs) */
      /* Key array (optional, can be added if needed) */
    0xC0,                    /* End Collection (Keyboard Application) */
};

const size_t g_HidReportDescSize = sizeof(g_HidReportDesc);



/* ---------------------------------------------------------------------------
 * Driver housekeeping
 * --------------------------------------------------------------------------- */

VOID
I2CHID_Unload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

/* 
 * Issue an IOCTL_I2C_READ to the lower i2cctrl device.
 * ext    – our FDO extension
 * params – pointer to I2C_READ_PARAMS (address, prefix, lengths)
 * buffer – caller’s buffer to receive data
 * length – size of buffer
 */
NTSTATUS
I2CHID_IoctlRead(PI2CHID_FDO ext,
                 PI2C_READ_PARAMS params,
                 PUCHAR buffer,
                 ULONG length)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    NTSTATUS status;

    if (ext == NULL || ext->LowerDevice == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(IOCTL_I2C_READ,
                                        ext->LowerDevice,
                                        params,
                                        sizeof(I2C_READ_PARAMS),
                                        buffer,
                                        length,
                                        FALSE,
                                        &event,
                                        &iosb);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(ext->LowerDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        status = iosb.Status;
    }

    return status;
}

NTSTATUS
I2CHID_DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
 * Unified HIDCLASS internal IOCTL dispatch – supports FDO (I²C HID) and PT
 * XP-BSOD-safe, WinDDK-compiler-safe, C89-compliant, METHOD_BUFFERED-consistent
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CHID_DispatchInternalIoctl(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PIO_STACK_LOCATION isl;
    ULONG code;
    NTSTATUS status;
    ULONG_PTR info;

    PI2CHID_FDO   extFdo;
    PI2CHID_PT_DEVEXT extPt;

    /* Initialize locals first (C89) */
    isl    = IoGetCurrentIrpStackLocation(Irp);
    code   = isl->Parameters.DeviceIoControl.IoControlCode;
    status = STATUS_INVALID_DEVICE_REQUEST;
    info   = 0;

    /* Interpret extension as either HID FDO or PT
       (both point to DeviceExtension but represent different roles) */
    extFdo = (PI2CHID_FDO)DeviceObject->DeviceExtension;
    extPt  = (PI2CHID_PT_DEVEXT)DeviceObject->DeviceExtension;

    switch (code) {

    /* === DEVICE DESCRIPTOR (buffered out) ================================= */
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
    {
        PHID_DESCRIPTOR outDesc;
        ULONG outLen;

        outDesc = (PHID_DESCRIPTOR)Irp->AssociatedIrp.SystemBuffer;
        outLen  = isl->Parameters.DeviceIoControl.OutputBufferLength;

        if (extFdo->Self == DeviceObject) {
            if (outDesc == NULL || outLen < sizeof(HID_DESCRIPTOR)) {
                status = STATUS_BUFFER_TOO_SMALL;
                info   = 0;
                break;
            }
            if (extFdo->HidStatic.HidDesc.bLength == 0) {
                status = STATUS_INVALID_DEVICE_STATE;
                info   = 0;
                break;
            }
            RtlCopyMemory(outDesc, &extFdo->HidStatic.HidDesc, sizeof(HID_DESCRIPTOR));
            info   = sizeof(HID_DESCRIPTOR);
            status = STATUS_SUCCESS;
        } else {
            if (outDesc == NULL || outLen < sizeof(HID_DESCRIPTOR)) {
                status = STATUS_BUFFER_TOO_SMALL;
                info   = 0;
                break;
            }
            RtlCopyMemory(outDesc, &extPt->HidDesc, sizeof(HID_DESCRIPTOR));
            info   = sizeof(HID_DESCRIPTOR);
            status = STATUS_SUCCESS;
        }
        break;
    }

    /* === REPORT DESCRIPTOR (buffered out) ================================= */
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
    {
        PUCHAR outBuf;
        ULONG  outLen;

        outBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
        outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;

        if (outBuf == NULL) {
            status = STATUS_INVALID_USER_BUFFER;
            info   = 0;
            break;
        }

        if (extFdo->Self == DeviceObject) {
            if (extFdo->HidStatic.ReportDescLength == 0 ||
                outLen < extFdo->HidStatic.ReportDescLength) {
                status = STATUS_BUFFER_TOO_SMALL;
                info   = 0;
                break;
            }
            RtlCopyMemory(outBuf,
                          extFdo->HidStatic.ReportDesc,
                          extFdo->HidStatic.ReportDescLength);
            info   = extFdo->HidStatic.ReportDescLength;
            status = STATUS_SUCCESS;
        } else {
            if (extPt->ReportDescLen == 0 ||
                outLen < extPt->ReportDescLen) {
                status = STATUS_BUFFER_TOO_SMALL;
                info   = 0;
                break;
            }
            RtlCopyMemory(outBuf,
                          extPt->ReportDesc,
                          extPt->ReportDescLen);
            info   = extPt->ReportDescLen;
            status = STATUS_SUCCESS;
        }
        break;
    }

    /* === COLLECTION INFORMATION (buffered out) ============================= */
    case IOCTL_HID_GET_COLLECTION_INFORMATION:
    {
        PHID_COLLECTION_INFORMATION ci;
        ULONG outLen;

        ci     = (PHID_COLLECTION_INFORMATION)Irp->AssociatedIrp.SystemBuffer;
        outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;

        if (extFdo->Self != DeviceObject) {
            /* Only meaningful for the FDO in this stack */
            status = STATUS_INVALID_DEVICE_REQUEST;
            info   = 0;
            break;
        }

        if (ci == NULL || outLen < sizeof(HID_COLLECTION_INFORMATION)) {
            status = STATUS_BUFFER_TOO_SMALL;
            info   = 0;
            break;
        }

        RtlZeroMemory(ci, sizeof(*ci));
        ci->DescriptorSize = extFdo->HidStatic.ReportDescLength;
        ci->Polled         = FALSE;
        ci->VendorID       = extFdo->VendorId;
        ci->ProductID      = extFdo->ProductId;
        ci->VersionNumber  = extFdo->VersionNumber;

        info   = sizeof(HID_COLLECTION_INFORMATION);
        status = STATUS_SUCCESS;
        break;
    }

    /* === DEVICE ATTRIBUTES (buffered out) ================================= */
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
    {
        PHID_DEVICE_ATTRIBUTES attrs;
        ULONG outLen;

        attrs  = (PHID_DEVICE_ATTRIBUTES)Irp->AssociatedIrp.SystemBuffer;
        outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;

        if (attrs == NULL || outLen < sizeof(HID_DEVICE_ATTRIBUTES)) {
            status = STATUS_BUFFER_TOO_SMALL;
            info   = 0;
            break;
        }

        RtlZeroMemory(attrs, sizeof(*attrs));
        attrs->Size = sizeof(HID_DEVICE_ATTRIBUTES);

        if (extFdo->Self == DeviceObject) {
            attrs->VendorID      = extFdo->VendorId;
            attrs->ProductID     = extFdo->ProductId;
            attrs->VersionNumber = extFdo->VersionNumber;
        } else {
            /* PT emulation attributes */
            attrs->VendorID      = 0xFFFF;
            attrs->ProductID     = 0x0001;
            attrs->VersionNumber = 0x0001;
        }

        info   = sizeof(HID_DEVICE_ATTRIBUTES);
        status = STATUS_SUCCESS;
        break;
    }

    /* === GET STRING (buffered out) ======================================== */
    case IOCTL_HID_GET_STRING:
    {
        PWSTR outStr;
        ULONG outLen;
        USHORT id;
        const WCHAR* src;
        ULONG bytes;

        outStr = (PWSTR)Irp->AssociatedIrp.SystemBuffer;
        outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
        /* Type3InputBuffer carries the string ID for HID GET_STRING requests */
        id     = (USHORT)(ULONG_PTR)isl->Parameters.DeviceIoControl.Type3InputBuffer;

        src   = NULL;
        bytes = 0;

        if (outStr == NULL || outLen == 0) {
            status = STATUS_BUFFER_TOO_SMALL;
            info   = 0;
            break;
        }

        if (extPt->Self == DeviceObject) {
            static const WCHAR Mfr[]  = L"PT Emulation Vendor";
            static const WCHAR Prod[] = L"Precision Touchpad (XP Emulation)";
            static const WCHAR Ser[]  = L"0001";

            switch (id) {
            case HID_STRING_ID_IMANUFACTURER: src = Mfr;  bytes = sizeof(Mfr);  break;
            case HID_STRING_ID_IPRODUCT:      src = Prod; bytes = sizeof(Prod); break;
            case HID_STRING_ID_ISERIALNUMBER: src = Ser;  bytes = sizeof(Ser);  break;
            default: status = STATUS_INVALID_PARAMETER;    info = 0;            break;
            }

            if (NT_SUCCESS(status)) {
                if (outLen < bytes) {
                    status = STATUS_BUFFER_TOO_SMALL;
                    info   = 0;
                } else {
                    RtlCopyMemory(outStr, src, bytes);
                    info   = bytes;
                    status = STATUS_SUCCESS;
                }
            }
        } else {
            /* FDO path: if needed, provide FDO-specific strings; else not supported */
            status = STATUS_NOT_SUPPORTED;
            info   = 0;
        }
        break;
    }

    /* === SET FEATURE (buffered in) ======================================== */
    case IOCTL_HID_SET_FEATURE:
    {
        PUCHAR inBuf;
        ULONG  inLen;

        inBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
        inLen = isl->Parameters.DeviceIoControl.InputBufferLength;

        if (inBuf == NULL || inLen == 0U) {
            status = STATUS_BUFFER_TOO_SMALL;
            info   = 0;
            break;
        }

        if (extFdo->Self == DeviceObject) {
            /* Not yet implemented for real HID-over-I²C */
            status = STATUS_NOT_IMPLEMENTED;
            info   = 0;
        } else {
            if (inLen < sizeof(extPt->FeatureBuffer)) {
                status = STATUS_BUFFER_TOO_SMALL;
                info   = 0;
                break;
            }
            RtlCopyMemory(extPt->FeatureBuffer, inBuf, sizeof(extPt->FeatureBuffer));
            info   = sizeof(extPt->FeatureBuffer);
            status = STATUS_SUCCESS;
        }
        break;
    }

    default:
        /* For unrecognized IOCTLs, just pass down to lower driver safely.
           Ensure extFdo->LowerDevice is valid and avoid completing twice. */
        if (extFdo != NULL && extFdo->LowerDevice != NULL) {
            IoSkipCurrentIrpStackLocation(Irp);
            return IoCallDriver(extFdo->LowerDevice, Irp);
        } else {
            status = STATUS_INVALID_DEVICE_REQUEST;
            info   = 0;
        }
        break;
    }

    /* Complete IRP for handled cases (single completion point) */
    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}


/* ---------------------------------------------------------------------------
 * Read queue completion helpers
 * --------------------------------------------------------------------------- */

VOID
I2CHID_CompleteQueuedReads(PI2CHID_FDO ext, PUCHAR data, ULONG len)
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;

    KeAcquireSpinLock(&ext->ReadQueueLock, &oldIrql);

    while (!IsListEmpty(&ext->ReadQueue)) {
        PIRP irp;
        PIO_STACK_LOCATION isl;
        ULONG outLen;
        ULONG toCopy;

        entry = RemoveHeadList(&ext->ReadQueue);
        irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);

        isl = IoGetCurrentIrpStackLocation(irp);
        outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
        toCopy = (len < outLen) ? len : outLen;

        if (toCopy != 0 && irp->UserBuffer != NULL) {
            RtlCopyMemory(irp->UserBuffer, data, toCopy);
        }

        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = toCopy;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }

    KeReleaseSpinLock(&ext->ReadQueueLock, oldIrql);
}

/* ---------------------------------------------------------------------------
 * Interrupt DPC — runs at DISPATCH_LEVEL after ISR
 * --------------------------------------------------------------------------- */
VOID
I2CHID_InterruptDpc(
    KDPC* Dpc,
    PVOID DeferredContext,
    PVOID Arg1,
    PVOID Arg2
    )
{
    PI2CHID_FDO ext;
    UCHAR inBuf[sizeof(I2CHID_REPORT)];
    BOOLEAN haveData;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    ext = (PI2CHID_FDO)DeferredContext;
    if (ext == NULL || ext->Removed || !ext->Started) {
        return;
    }

    /* Clear the pending flag so ISR can queue again */
    if (InterlockedExchange(&ext->PendingInputFlag, 0) == 0) {
        return; /* spurious DPC */
    }

    RtlZeroMemory(inBuf, sizeof(inBuf));

    /* Perform the actual I²C read of the input register */
    haveData = I2CHID_ReadInput(ext, inBuf, sizeof(inBuf));
    if (!haveData) {
        return;
    }

    /* Complete all queued HID read IRPs with the raw input data */
    I2CHID_CompleteQueuedReads(ext, inBuf, sizeof(inBuf));
}

/* Interrupt Service Routine — schedules input DPC */
BOOLEAN
I2CHID_InterruptServiceRoutine(
    PKINTERRUPT Interrupt,
    PVOID ServiceContext
    )
{
    PI2CHID_FDO ext;
    LONG prev;

    UNREFERENCED_PARAMETER(Interrupt);

    ext = (PI2CHID_FDO)ServiceContext;
    if (ext == NULL || ext->Removed || !ext->Started) {
        return FALSE;
    }

    /* Gate DPC queuing with a flag */
    prev = InterlockedExchange(&ext->PendingInputFlag, 1);
    if (prev == 0) {
        KeInsertQueueDpc(&ext->InterruptDpc, NULL, NULL);
    }

    return TRUE; /* we handled the interrupt */
}

/* ---------------------------------------------------------------------------
 * ReadInput — fetch a HID input report from the device via I²C
 * Hardened: validates parameters, retries on transient errors, ETW logging.
 * --------------------------------------------------------------------------- */
BOOLEAN
I2CHID_ReadInput(
    PI2CHID_FDO ext,
    PUCHAR          buf,
    ULONG           bufLen
    )
{
    NTSTATUS        status;
    UCHAR           rawBuf[64];      /* raw device data buffer */
    I2C_READ_PARAMS params;
    I2CHID_REPORT   rpt;
    int             tries;

    /* Basic parameter validation */
    if (ext == NULL || buf == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "I2CHID_ReadInput invalid parameters ext=%p buf=%p len=%lu",
                    ext, buf, bufLen);
        return FALSE;
    }
    if (bufLen < sizeof(I2CHID_REPORT)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "I2CHID_ReadInput buffer too small len=%lu need=%u",
                    bufLen, (unsigned)sizeof(I2CHID_REPORT));
        return FALSE;
    }

    /* Prepare I²C read parameters: input register is usually at 0x00 */
    RtlZeroMemory(&params, sizeof(params));
    params.Address7Bit = ext->I2cAddr7Bit;
    params.Prefix[0]   = 0x00;     /* input register */
    params.PrefixLen   = 1;
    params.ReadLen     = sizeof(rawBuf);

    /* Attempt read with limited retries */
    RtlZeroMemory(rawBuf, sizeof(rawBuf));
    status = STATUS_UNSUCCESSFUL;
    for (tries = 0; tries < 3; tries++) {
        status = I2CHID_IoctlRead(ext, &params, rawBuf, sizeof(rawBuf));
        if (NT_SUCCESS(status)) {
            break;
        }
        KeStallExecutionProcessor(1000); /* 1ms backoff */
    }
    if (!NT_SUCCESS(status)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_TRANSFER,
                    "I2CHID_ReadInput I2C read failed addr=0x%02X status=0x%08lx",
                    params.Address7Bit, status);
        return FALSE;
    }

    /* Parse the raw buffer into our report structure */
    RtlZeroMemory(&rpt, sizeof(rpt));
    I2CHID_ParseRawPnp0c50(rawBuf, sizeof(rawBuf), &rpt);

    /* Copy the parsed report into the caller’s buffer */
    RtlCopyMemory(buf, &rpt, sizeof(rpt));

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_TRANSFER,
                "I2CHID_ReadInput success ReportId=%lu Timestamp=%I64d",
                rpt.ReportId, rpt.Timestamp.QuadPart);

    return TRUE;
}

