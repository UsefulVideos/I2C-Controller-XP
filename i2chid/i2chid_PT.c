#include <ntddk.h>
#include <hidport.h>        // HIDCLASS miniport interfaces (XP/2003)
#include "..\i2cctrl\i2cctrl_ioctl.h"  // Your controller IOCTLs (adjust include path)
#include "..\i2cctrl\i2cctrl_ext.h"    // Controller/device contracts if needed
#include "I2CHID_i2cctrl.h"
#include "I2CHID_hid.h"

// -------------------------------
// Forward declarations
// -------------------------------
DRIVER_UNLOAD   I2CHID_PT_Unload;
NTSTATUS        I2CHID_PT_DispatchPass(PDEVICE_OBJECT, PIRP);

NTSTATUS I2CHID_PT_InitDescriptors(PI2CHID_PT_DEVEXT);
NTSTATUS I2CHID_PT_ReadRawSample(PI2CHID_PT_DEVEXT, PT_RAW_SAMPLE*);
	
UCHAR
I2CHID_PT_TapButtons(PI2CHID_PT_DEVEXT, const PT_RAW_SAMPLE*);

VOID
I2CHID_PT_Unload(
    PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
}

// -------------------------------
// Pass-through defaults
// -------------------------------
NTSTATUS
I2CHID_PT_DispatchPass(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    PI2CHID_PT_DEVEXT dx = (PI2CHID_PT_DEVEXT)DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(dx->LowerDevice, Irp);
}

// -------------------------------
// Helpers
// -------------------------------
#ifndef HID_USB_VERSION
#define HID_USB_VERSION 0x0111   /* HID spec version 1.11 */
#endif

NTSTATUS
I2CHID_PT_InitDescriptors(
    PI2CHID_PT_DEVEXT dev
    )
{
    /* Must run at PASSIVE_LEVEL */
    I2CHID_REQUIRE_PASSIVE();
    PAGED_CODE();

    if (dev == NULL) {
        I2CHID_Log("I2CHID_PT_InitDescriptors: NULL device extension\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* Clear HID descriptor structure */
    RtlZeroMemory(&dev->HidDesc, sizeof(dev->HidDesc));

    /* Initialize HID descriptor header */
    dev->HidDesc.bLength         = (UCHAR)sizeof(HID_DESCRIPTOR);
    dev->HidDesc.bDescriptorType = HID_HID_DESCRIPTOR_TYPE;
    dev->HidDesc.bcdHID          = HID_USB_VERSION;   /* HID spec version */
    dev->HidDesc.bCountry        = 0;
    dev->HidDesc.bNumDescriptors = 1;

    dev->HidDesc.DescriptorList[0].bReportType   = HID_REPORT_DESCRIPTOR_TYPE;
    dev->HidDesc.DescriptorList[0].wReportLength = (USHORT)g_HidReportDescSize;

    /* Use unified descriptor and exported size */
    dev->ReportDesc    = (PUCHAR)g_HidReportDesc;
    dev->ReportDescLen = (USHORT)g_HidReportDescSize;

    KdPrint(("I2CHID_PT_InitDescriptors: initialized HID descriptor, report length=%lu\n",
         (ULONG)dev->ReportDescLen));

    return STATUS_SUCCESS;
}

VOID
I2CHID_PT_Complete(
    PIRP Irp,
    NTSTATUS Status,
    ULONG_PTR Information
    )
{
    if (Irp == NULL) {
        I2CHID_Log("I2CHID_PT_Complete: NULL IRP\n");
        return;
    }

    /* Update IRP status fields */
    Irp->IoStatus.Status      = Status;
    Irp->IoStatus.Information = Information;

    /* Safe completion at PASSIVE_LEVEL or DISPATCH_LEVEL */
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    KdPrint(("I2CHID_PT_Complete: IRP %p completed with status=0x%08X, info=%lu\n",
         Irp, (unsigned int)Status, (ULONG)Information));
}

// Read a raw PT sample from your I2C controller.
// You must implement IOCTL_GET_PT_SAMPLE in i2cctrl.sys to fill PT_RAW_SAMPLE.
NTSTATUS
I2CHID_PT_ReadRawSample(
    PI2CHID_PT_DEVEXT dx,
    PT_RAW_SAMPLE* Sample
    )
{
    NTSTATUS status;
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;

    if (!Sample) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Sample, sizeof(*Sample));

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
        IOCTL_GET_PT_SAMPLE,
        dx->LowerDevice,
        &dx->I2cAddress, sizeof(dx->I2cAddress),  // Input: target address
        Sample, sizeof(*Sample),                  // Output: raw contacts
        FALSE,
        &event,
        &iosb
        );

    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;

    status = IoCallDriver(dx->LowerDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    return status;
}

//
// Send a HID keyboard report with modifier state.
// - modifierBits: 8‑bit mask (bit0 = Left Ctrl, bit1 = Left Shift, bit2 = Left Alt, bit3 = Left GUI, etc.)
// - Report format: [Modifiers][Reserved][KeyCodes…] (standard HID boot keyboard report)
//
VOID
I2CHID_SendKeyboardModifier(
    PI2CHID_PT_DEVEXT dev,
    UCHAR modifierBits
    )
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    PIRP Irp;
    PUCHAR out;

    KeAcquireSpinLock(&dev->ReadQueueLock, &oldIrql);
    while (!IsListEmpty(&dev->ReadQueue)) {
        entry = RemoveHeadList(&dev->ReadQueue);
        Irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);

        out = (PUCHAR)Irp->UserBuffer;
        // Keyboard report is 8 bytes: modifiers, reserved, 6 key slots
        if (out && IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.OutputBufferLength >= 8) {
            out[0] = modifierBits;  // Modifier byte
            out[1] = 0x00;          // Reserved
            out[2] = 0x00;          // Keycode 1
            out[3] = 0x00;          // Keycode 2
            out[4] = 0x00;          // Keycode 3
            out[5] = 0x00;          // Keycode 4
            out[6] = 0x00;          // Keycode 5
            out[7] = 0x00;          // Keycode 6
            I2CHID_PT_Complete(Irp, STATUS_SUCCESS, 8);
        } else {
            I2CHID_PT_Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
    }
    KeReleaseSpinLock(&dev->ReadQueueLock, oldIrql);
}

//
// Send a HID mouse report with buttons, relative motion, vertical wheel, and optional horizontal wheel.
// - buttons: bitmask (bit0=Left, bit1=Right, bit2=Middle)
// - dx, dy: relative X/Y motion (-127..127)
// - wheelV: vertical wheel delta (-127..127)
// - wheelH: horizontal wheel delta (-127..127) [optional, set 0 if unused]
// Notes:
// • Values are clamped to HID limits to avoid overflow.
// • Report format matches Mouse Application in g_HidReportDesc.
// • XP/2003 HID stack consumes vertical wheel; horizontal wheel is ignored unless descriptor is extended.
//
VOID
I2CHID_SendMouseReport(
    PI2CHID_PT_DEVEXT dev,
    UCHAR buttons,
    CHAR dx,
    CHAR dy,
    CHAR wheelV,
    CHAR wheelH
    )
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    PIRP Irp;
    PUCHAR out;
    CHAR clampedDx, clampedDy, clampedWheelV, clampedWheelH;

    /* Clamp motion and wheel values to HID range (-127..127) */
    if (dx < -127) dx = -127; if (dx > 127) dx = 127;
    if (dy < -127) dy = -127; if (dy > 127) dy = 127;
    if (wheelV < -127) wheelV = -127; if (wheelV > 127) wheelV = 127;
    if (wheelH < -127) wheelH = -127; if (wheelH > 127) wheelH = 127;

    clampedDx     = dx;
    clampedDy     = dy;
    clampedWheelV = wheelV;
    clampedWheelH = wheelH;

    KeAcquireSpinLock(&dev->ReadQueueLock, &oldIrql);
    while (!IsListEmpty(&dev->ReadQueue)) {
        entry = RemoveHeadList(&dev->ReadQueue);
        Irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);

        out = (PUCHAR)Irp->UserBuffer;
        /* Mouse report is 5 bytes: buttons, X, Y, vertical wheel, horizontal wheel */
        if (out && IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.OutputBufferLength >= 5) {
            out[0] = buttons;            /* Button bitmask */
            out[1] = (UCHAR)clampedDx;   /* Relative X */
            out[2] = (UCHAR)clampedDy;   /* Relative Y */
            out[3] = (UCHAR)clampedWheelV; /* Vertical wheel delta */
            out[4] = (UCHAR)clampedWheelH; /* Horizontal wheel delta */
            I2CHID_PT_Complete(Irp, STATUS_SUCCESS, 5);
        } else {
            I2CHID_PT_Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
    }
    KeReleaseSpinLock(&dev->ReadQueueLock, oldIrql);
}


// Map a raw PT sample to touchpad-like HID report fields, with integer-only pinch/zoom.
// Adds backwards compatibility: pinchZoom is translated into Ctrl+wheel events using a proper HID keyboard report.
// C89-compliant, no floating point; uses centroid motion, two-finger scroll (vertical + horizontal), and pinch/zoom (squared distance).
VOID
I2CHID_PT_EmulateTouchpad(
    PI2CHID_PT_DEVEXT dev,
    const PT_RAW_SAMPLE* s,
    UCHAR* buttons,
    CHAR* dxRel,
    CHAR* dyRel,
    CHAR* wheelVRel,
    CHAR* pinchZoomRel
    )
{
    UCHAR btn;
    CHAR rx, ry, whV, whH, pz;
    LONG sumX, sumY;
    ULONG active, i;
    LONG cx, cy;
    LONG dxAbs, dyAbs;
    LONG dyAvg, dxAvg;
    LONG dxDist, dyDist, distSq, deltaSq;

    /* Outputs init */
    btn = 0;
    rx = 0;
    ry = 0;
    whV = 0;
    whH = 0;
    pz = 0;

    /* Accumulators init */
    sumX = 0;
    sumY = 0;
    active = 0;
    cx = 0;
    cy = 0;
    dxAbs = 0;
    dyAbs = 0;
    dyAvg = 0;
    dxAvg = 0;
    dxDist = 0;
    dyDist = 0;
    distSq = 0;
    deltaSq = 0;

    /* Map taps/physical zones to button flags */
    btn = I2CHID_PT_TapButtons(dev, s);

    /* Centroid motion of all active contacts */
    if (s->ContactCount > 0) {
        for (i = 0; i < s->ContactCount; i++) {
            if (s->Contacts[i].Flags & 0x01) {
                sumX += (LONG)s->Contacts[i].X;
                sumY += (LONG)s->Contacts[i].Y;
                active++;
            }
        }

        if (active > 0) {
            cx = sumX / (LONG)active;
            cy = sumY / (LONG)active;

            if (dev->LastPrimaryDown) {
                dxAbs = cx - (LONG)dev->lastX;
                dyAbs = cy - (LONG)dev->lastY;

                /* Sensitivity scaling; safeguard against division by zero */
                if (dev->Sensitivity <= 0) {
                    dev->Sensitivity = 1;
                }
                dxAbs /= dev->Sensitivity;
                dyAbs /= dev->Sensitivity;

                if (dxAbs < -127) dxAbs = -127; if (dxAbs > 127) dxAbs = 127;
                if (dyAbs < -127) dyAbs = -127; if (dyAbs > 127) dyAbs = 127;

                rx = (CHAR)dxAbs;
                ry = (CHAR)dyAbs;
            }

            dev->lastX = (USHORT)cx;
            dev->lastY = (USHORT)cy;
            dev->LastPrimaryDown = TRUE;
        } else {
            dev->LastPrimaryDown = FALSE;
        }
    } else {
        dev->LastPrimaryDown = FALSE;
    }

    /* Two-finger scroll and integer-only pinch/zoom (squared distance) */
    if (s->ContactCount >= 2 &&
        (s->Contacts[0].Flags & 0x01) &&
        (s->Contacts[1].Flags & 0x01)) {

        /* Two-finger vertical scroll (average Y vs last centroid Y) */
        dyAvg = ((LONG)s->Contacts[0].Y + (LONG)s->Contacts[1].Y) / 2 - (LONG)dev->lastY;
        if (dev->Sensitivity <= 0) {
            dev->Sensitivity = 1;
        }
        dyAvg /= (dev->Sensitivity * 2); /* slower wheel for smoother scroll */
        if (dyAvg < -127) dyAvg = -127; if (dyAvg > 127) dyAvg = 127;
        whV = (CHAR)dyAvg;

        /* Two-finger horizontal scroll (average X vs last centroid X) */
        dxAvg = ((LONG)s->Contacts[0].X + (LONG)s->Contacts[1].X) / 2 - (LONG)dev->lastX;
        dxAvg /= (dev->Sensitivity * 2);
        if (dxAvg < -127) dxAvg = -127; if (dxAvg > 127) dxAvg = 127;
        whH = (CHAR)dxAvg;

        /* Pinch/zoom: squared Euclidean distance; avoids floating point */
        dxDist = (LONG)s->Contacts[0].X - (LONG)s->Contacts[1].X;
        dyDist = (LONG)s->Contacts[0].Y - (LONG)s->Contacts[1].Y;
        distSq = dxDist * dxDist + dyDist * dyDist;

        if (dev->LastPinchDistSq != 0) {
            deltaSq = distSq - dev->LastPinchDistSq;

            /* Scale and clamp; sign conveys pinch (-) vs spread (+) */
            deltaSq /= dev->Sensitivity;
            if (deltaSq < -127) deltaSq = -127; if (deltaSq > 127) deltaSq = 127;

            pz = (CHAR)deltaSq;
        }
        dev->LastPinchDistSq = distSq;
    } else {
        dev->LastPinchDistSq = 0;
    }

    /* Backwards compatibility: if pinchZoom is nonzero, translate into Ctrl+wheel via keyboard report */
    if (pz != 0) {
        whV += pz;          /* reuse vertical wheel axis for zoom ticks */

        /* Emit HID keyboard report: Ctrl down */
        I2CHID_SendKeyboardModifier(dev, 0x01); /* Left Ctrl bit set */

        /* Emit HID mouse report with wheel movement */
        I2CHID_SendMouseReport(dev, btn, rx, ry, whV, whH);

        /* Emit HID keyboard report: Ctrl up */
        I2CHID_SendKeyboardModifier(dev, 0x00);

        pz = 0;            /* clear pinchZoom field */
    }

    *buttons      = btn;
    *dxRel        = rx;
    *dyRel        = ry;
    *wheelVRel    = whV;
    *pinchZoomRel = pz;
}


// Enhanced tap detection:
// - Single-finger short press-release → left click
// - Two-finger short press-release   → right click
// - Three-finger short press-release → middle click
// - Four-finger short press-release  → Ctrl modifier (for zoom compatibility)
// - Five-finger short press-release  → Windows/GUI modifier (future expansion)
// Timing gate ~100ms, tune against perf counter frequency.
UCHAR
I2CHID_PT_TapButtons(
    PI2CHID_PT_DEVEXT dev,
    const PT_RAW_SAMPLE* s
    )
{
    UCHAR btn;
    LARGE_INTEGER now;
    BOOLEAN anyDown;
    UCHAR downCount;
    UCHAR i;

    btn = 0;
    KeQueryPerformanceCounter(&now);

    anyDown = FALSE;
    downCount = 0;

    for (i = 0; i < s->ContactCount; i++) {
        if (s->Contacts[i].Flags & 0x01) {
            anyDown = TRUE;
            downCount++;
        }
    }

    if (anyDown) {
        dev->LastDownTime = now;
        dev->LastDownContacts = downCount;
        return 0; // hold until release to emit click
    }

    if (dev->LastDownContacts != 0) {
        LONGLONG dt;

        dt = (now.QuadPart - dev->LastDownTime.QuadPart);
        // Very rough timing gate (~100ms). You can tune against the perf counter frequency.
        if (dt > 0 && dt < 500000) {
            switch (dev->LastDownContacts) {
            case 1: btn = 0x01; break; // left
            case 2: btn = 0x02; break; // right
            case 3: btn = 0x04; break; // middle
            case 4:
                // Four-finger tap → emit Ctrl modifier via keyboard report
                I2CHID_SendKeyboardModifier(dev, 0x01); /* Ctrl down */
                I2CHID_SendKeyboardModifier(dev, 0x00); /* Ctrl up */
                btn = 0x00; // no mouse button, just modifier
                break;
            case 5:
                // Five-finger tap → emit Windows/GUI modifier via keyboard report
                I2CHID_SendKeyboardModifier(dev, 0x08); /* Left GUI (Windows key) down */
                I2CHID_SendKeyboardModifier(dev, 0x00); /* release */
                btn = 0x00; // no mouse button, just modifier
                break;
            default: break;
            }
        }
        dev->LastDownContacts = 0;
    }

    return btn;
}


VOID I2CHID_PT_CompleteQueuedReads(PI2CHID_PT_DEVEXT dev, const PT_RAW_SAMPLE* sample)
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    PIRP Irp;
    PUCHAR out;
    UCHAR buttons;
    CHAR relX, relY, wheelV, wheelH, pinchZoom;

    /* Initialize outputs */
    buttons   = 0;
    relX      = 0;
    relY      = 0;
    wheelV    = 0;
    wheelH    = 0;
    pinchZoom = 0;

    /* Emulate touchpad gestures: motion, scroll, pinch/zoom */
    I2CHID_PT_EmulateTouchpad(dev, sample, &buttons, &relX, &relY, &wheelV, &pinchZoom);

    /* Backwards compatibility: translate pinchZoom into Ctrl+wheel via keyboard report */
    if (pinchZoom != 0) {
        wheelV += pinchZoom;   /* reuse vertical wheel axis for zoom ticks */

        /* Emit HID keyboard report: Ctrl down */
        I2CHID_SendKeyboardModifier(dev, 0x01); /* Left Ctrl bit set */

        /* Emit HID mouse report with wheel movement */
        I2CHID_SendMouseReport(dev, buttons, relX, relY, wheelV, wheelH);

        /* Emit HID keyboard report: Ctrl up */
        I2CHID_SendKeyboardModifier(dev, 0x00);

        pinchZoom = 0;        /* clear pinchZoom field so legacy apps ignore it */
    }

    KeAcquireSpinLock(&dev->ReadQueueLock, &oldIrql);
    while (!IsListEmpty(&dev->ReadQueue)) {
        entry = RemoveHeadList(&dev->ReadQueue);
        Irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);

        out = (PUCHAR)Irp->UserBuffer;
        if (out && IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.OutputBufferLength >= 6) {
            /* Write 6 bytes: buttons, relX, relY, wheelV, wheelH, pinchZoom */
            out[0] = buttons;
            out[1] = relX;
            out[2] = relY;
            out[3] = wheelV;
            out[4] = wheelH;
            out[5] = pinchZoom;
            I2CHID_PT_Complete(Irp, STATUS_SUCCESS, 6);
        } else {
            I2CHID_PT_Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
    }
    KeReleaseSpinLock(&dev->ReadQueueLock, oldIrql);
}
