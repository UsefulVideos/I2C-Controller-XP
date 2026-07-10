/*  I2CHID_hw_shim.c (C89 compliant)
    - Interrupt-driven touchpad-like input engine for XP/2003
    - Uses IoConnectInterrupt to hook ISR/DPC
    - DPC issues IOCTL_I2C_READ to controller to fetch input packets
    - Builds a 2-contact multitouch HID input payload (touchpad emulation)
      and completes IOCTL_HID_READ_REPORT requests
*/
#include <ntddk.h>
#include "I2CHID_EXT.h"   /* PI2CHID_FDO */
#include "i2chid.h"       /* I2CHID_REPORT, IOCTL defs (if shared) */
#include "I2CHID_hid.h"   /* ISR/DPC prototypes, input helpers */
#include "I2CHID_hw_shim.h"

#ifndef CTL_CODE
#include <devioctl.h>
#endif

/* === ISR/DPC plumbing === */

/* -----------------------------------------------------------------------
   Gesture analysis helper (feature-complete, C89-compliant)
   - Tap-to-click (single finger within TapTimeMs and TapDistance)
   - Two-finger vertical scroll (accumulate and emit wheel ticks via mouse emulation)
   - Pinch-to-zoom (two-finger squared-distance delta, accumulates ZoomScale ticks)
   - Two-finger rotation (signed angle via cross-product, accumulates RotateScale ticks)
   - Three-finger swipe (centroid delta vs SwipeScale; placeholder hooks)
   Uses ext->Gest: lastX[], lastY[], accumScroll, accumZoom, accumRotate, lastTickMs
   Uses ext->Cfg : TapTimeMs, TapDistance, ScrollScale, ZoomScale, PalmThreshold, RotateScale, SwipeScale
   ----------------------------------------------------------------------- */
static VOID
I2cHw_AnalyzeGestures(
    PI2CHID_FDO ext,
    const USHORT x[MT_MAX_CONTACTS],
    const USHORT y[MT_MAX_CONTACTS],
    UCHAR count
    )
{
    ULONG nowMs;
    UCHAR i;
    USHORT dx1;
    USHORT dy1;
    BOOLEAN isTap;
    USHORT tapDist;
    ULONG tapTime;
    LONG scrollAccumDelta;
    LONG zoomPairCur;
    LONG zoomPairLast;
    UCHAR j;
    UCHAR k;
    LONG bestDistSq;
    LONG dx;
    LONG dy;
    LONG ldx;
    LONG ldy;
    LONG dsum;
    USHORT dyAbs;
    UCHAR m;
    LONG rotNum;
    LONG rotDen;
    LONG rotSign;
    LONG cx;
    LONG cy;
    LONG lcx;
    LONG lcy;
    LONG vx;
    LONG vy;

    if (ext == NULL) {
        return;
    }

    /* Convert interrupt time (100ns units) to ~milliseconds */
    nowMs = (ULONG)(KeQueryInterruptTime() / 10000UL);

    tapDist = (USHORT)ext->Cfg.TapDistance;
    tapTime = (ULONG)ext->Cfg.TapTimeMs;

    /* Tap-to-click: single contact, minimal movement in allowed time */
    isTap = FALSE;
    if (count == 1U) {
        dx1 = (USHORT)((x[0] > ext->Gest.lastX[0]) ? (x[0] - ext->Gest.lastX[0]) : (ext->Gest.lastX[0] - x[0]));
        dy1 = (USHORT)((y[0] > ext->Gest.lastY[0]) ? (y[0] - ext->Gest.lastY[0]) : (ext->Gest.lastY[0] - y[0]));

        if ((ULONG)(nowMs - ext->Gest.lastTickMs) <= tapTime &&
            dx1 <= tapDist &&
            dy1 <= tapDist) {
            isTap = TRUE;
        }
    }

    if (isTap) {
        /* Mouse emulation: press and release */
        I2cHw_QueueMouseReport(ext, 0x01, 0, 0);
        I2cHw_QueueMouseReport(ext, 0x00, 0, 0);
    }

    /* Two-finger scroll: accumulate signed Y movements, ignore large jumps (palm) */
    scrollAccumDelta = 0;
    if (count >= 2U) {
        dsum = 0;
        for (i = 0U; i < count && i < MT_MAX_CONTACTS; i++) {
            dyAbs = (USHORT)((y[i] > ext->Gest.lastY[i]) ? (y[i] - ext->Gest.lastY[i]) : (ext->Gest.lastY[i] - y[i]));
            if (dyAbs < (USHORT)ext->Cfg.PalmThreshold) {
                if (y[i] > ext->Gest.lastY[i]) {
                    dsum += 1;   /* moving down */
                } else if (y[i] < ext->Gest.lastY[i]) {
                    dsum -= 1;   /* moving up */
                }
            }
        }
        scrollAccumDelta = dsum;
        ext->Gest.accumScroll += scrollAccumDelta;

        while (ext->Gest.accumScroll >= (LONG)ext->Cfg.ScrollScale) {
            /* Scroll down (positive wheel by convention) */
            I2cHw_QueueMouseReport(ext, 0x00, 0, 1);
            ext->Gest.accumScroll -= (LONG)ext->Cfg.ScrollScale;
        }
        while (ext->Gest.accumScroll <= -(LONG)ext->Cfg.ScrollScale) {
            /* Scroll up (negative wheel) */
            I2cHw_QueueMouseReport(ext, 0x00, 0, -1);
            ext->Gest.accumScroll += (LONG)ext->Cfg.ScrollScale;
        }
    }

    /* Pinch-to-zoom: choose closest pair, compare squared distance change.
       Rotation: cross-product sign between last and current vectors for the same pair. */
    if (count >= 2U) {
        bestDistSq = 0x7FFFFFFFL;
        j = 0U;
        k = 1U;

        for (i = 0U; i < count && i < MT_MAX_CONTACTS; i++) {
            for (m = (UCHAR)(i + 1U); m < count && m < MT_MAX_CONTACTS; m++) {
                LONG tx;
                LONG ty;
                LONG s;
                tx = (LONG)x[i] - (LONG)x[m];
                ty = (LONG)y[i] - (LONG)y[m];
                s = (tx * tx) + (ty * ty);
                if (s < bestDistSq) {
                    bestDistSq = s;
                    j = i;
                    k = m;
                }
            }
        }

        ldx = (LONG)ext->Gest.lastX[j] - (LONG)ext->Gest.lastX[k];
        ldy = (LONG)ext->Gest.lastY[j] - (LONG)ext->Gest.lastY[k];
        zoomPairLast = (ldx * ldx) + (ldy * ldy);

        dx = (LONG)x[j] - (LONG)x[k];
        dy = (LONG)y[j] - (LONG)y[k];
        zoomPairCur = (dx * dx) + (dy * dy);

        if (zoomPairCur > zoomPairLast) {
            /* Fingers moving apart => zoom out (+1) */
            ext->Gest.accumZoom += 1;
        } else if (zoomPairCur < zoomPairLast) {
            /* Fingers moving closer => zoom in (-1) */
            ext->Gest.accumZoom -= 1;
        }

        while (ext->Gest.accumZoom >= (LONG)ext->Cfg.ZoomScale) {
            /* Emit one zoom-out tick (mapping left to application; no mouse event by default) */
            ext->Gest.accumZoom -= (LONG)ext->Cfg.ZoomScale;
        }
        while (ext->Gest.accumZoom <= -(LONG)ext->Cfg.ZoomScale) {
            /* Emit one zoom-in tick */
            ext->Gest.accumZoom += (LONG)ext->Cfg.ZoomScale;
        }

        /* Rotation via cross product (z-component): sign-only accumulation */
        rotNum = (ldx * dy) - (ldy * dx);
        rotDen = zoomPairLast + zoomPairCur;
        rotSign = 0;
        if (rotDen != 0) {
            if (rotNum > 0) {
                rotSign = 1;  /* clockwise */
            } else if (rotNum < 0) {
                rotSign = -1; /* counter-clockwise */
            }
        }
        ext->Gest.accumRotate += rotSign;

        while (ext->Gest.accumRotate >= (LONG)ext->Cfg.RotateScale) {
            /* Rotation clockwise tick (hook: vendor-defined event or ignore on XP) */
            ext->Gest.accumRotate -= (LONG)ext->Cfg.RotateScale;
        }
        while (ext->Gest.accumRotate <= -(LONG)ext->Cfg.RotateScale) {
            /* Rotation counter-clockwise tick */
            ext->Gest.accumRotate += (LONG)ext->Cfg.RotateScale;
        }
    }

    /* Three-finger swipe: centroid movement threshold */
    if (count >= 3U) {
        cx = 0;
        cy = 0;
        lcx = 0;
        lcy = 0;

        for (i = 0U; i < count && i < MT_MAX_CONTACTS; i++) {
            cx += (LONG)x[i];
            cy += (LONG)y[i];
            lcx += (LONG)ext->Gest.lastX[i];
            lcy += (LONG)ext->Gest.lastY[i];
        }

        cx /= (LONG)count;
        cy /= (LONG)count;
        lcx /= (LONG)count;
        lcy /= (LONG)count;

        vx = cx - lcx;
        vy = cy - lcy;

        /* Placeholders: define actual actions as needed */
        if (vx >= (LONG)ext->Cfg.SwipeScale) {
            /* Right swipe */
        } else if (vx <= -(LONG)ext->Cfg.SwipeScale) {
            /* Left swipe */
        }
        if (vy >= (LONG)ext->Cfg.SwipeScale) {
            /* Down swipe */
        } else if (vy <= -(LONG)ext->Cfg.SwipeScale) {
            /* Up swipe */
        }
    }

    /* Update last positions and time stamp */
    for (i = 0U; i < MT_MAX_CONTACTS; i++) {
        ext->Gest.lastX[i] = x[i];
        ext->Gest.lastY[i] = y[i];
    }
    ext->Gest.lastTickMs = nowMs;
}



/* -----------------------------------------------------------------------
   DPC routine:
   - Invoked from ISR to service pending input
   - Reads latest packet from I²C controller (device‑specific decode)
   - Populates up to MT_MAX_CONTACTS arrays (X, Y, ID, flags)
   - Builds a multitouch HID report buffer
   - Completes one pending HID read IRP with the report
   ----------------------------------------------------------------------- */
static VOID
I2cHw_PollDpc(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArg1,
    PVOID SystemArg2
    )
{
    PI2CHID_FDO ext;
    USHORT ax[MT_MAX_CONTACTS];
    USHORT ay[MT_MAX_CONTACTS];
    UCHAR  aid[MT_MAX_CONTACTS];
    UCHAR  aflags[MT_MAX_CONTACTS];
    UCHAR  cnt;
    UCHAR  i;
    UCHAR  mt[MT_REPORT_SIZE];
    UCHAR  packet[I2C_INPUT_PACKET_MAX];
    ULONG  readLen;
    NTSTATUS status;
    USHORT x1;
    USHORT y1;
    USHORT x2;
    USHORT y2;
    UCHAR  flags;
    BOOLEAN tip1;
    BOOLEAN tip2;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(SystemArg1);
    UNREFERENCED_PARAMETER(SystemArg2);

    ext = (PI2CHID_FDO)DeferredContext;
    if (ext == NULL) {
        return;
    }

    RtlZeroMemory(ax, sizeof(ax));
    RtlZeroMemory(ay, sizeof(ay));
    RtlZeroMemory(aid, sizeof(aid));
    RtlZeroMemory(aflags, sizeof(aflags));
    RtlZeroMemory(mt, sizeof(mt));
    RtlZeroMemory(packet, sizeof(packet));

    cnt = 0;
    readLen = 0;

    /* Fetch latest input packet from the controller */
    status = I2cHw_ReadInputPacket(ext, packet, &readLen);
    if (!NT_SUCCESS(status) || readLen == 0) {
        I2cHw_QueueMouseReport(ext, 0, 0, 0);
        return;
    }

    /* Device-specific parsing populates x1,y1,x2,y2,tip1,tip2, etc. */
    x1 = (USHORT)(packet[0] | ((USHORT)packet[1] << 8));
    y1 = (USHORT)(packet[2] | ((USHORT)packet[3] << 8));
    flags = packet[4];

    tip1 = (flags & 0x01) ? TRUE : FALSE;
    tip2 = (flags & 0x02) ? TRUE : FALSE;

    if (tip1) {
        ax[cnt]     = x1;
        ay[cnt]     = y1;
        aid[cnt]    = 0;
        aflags[cnt] = (UCHAR)(0x01 | 0x02 | 0x04); /* Tip + InRange + Primary */
        cnt++;
    }
    if (tip2) {
        if (readLen >= 9) {
            x2 = (USHORT)(packet[5] | ((USHORT)packet[6] << 8));
            y2 = (USHORT)(packet[7] | ((USHORT)packet[8] << 8));
        } else {
            x2 = x1;
            y2 = y1;
        }
        ax[cnt]     = x2;
        ay[cnt]     = y2;
        aid[cnt]    = 1;
        aflags[cnt] = (UCHAR)(0x01 | 0x02);        /* Tip + InRange */
        cnt++;
    }

    /* Additional contacts (up to MT_MAX_CONTACTS) can be parsed similarly */

    I2cHw_BuildMultitouchReport5(mt, cnt, ax, ay, aid, aflags);
    I2cHw_CompleteOneRead(ext, mt, (ULONG)MT_REPORT_SIZE);
}


/* === Controller I/O helpers (XP/2003 style) === */

/* Robust synchronous read of the current input packet from the I2C controller
   - Allocates a METHOD_BUFFERED descriptor
   - Issues IOCTL_I2C_READ with retries
   - Copies data into caller buffer if valid
*/
static NTSTATUS
I2cHw_ReadInputPacket(
    PI2CHID_FDO ext,
    PUCHAR buf,
    PULONG bytesRead
    )
{
    NTSTATUS        status;
    PIRP            irp;
    IO_STATUS_BLOCK iosb;
    KEVENT          evt;
    I2CCTRL_RW*     rw;
    ULONG           allocSize;
    UCHAR           addr;
    ULONG           readLen;
    PDEVICE_OBJECT  ctrl;
    ULONG           attempts;
    ULONG           backoffUs;
    ULONG           got;
    ULONG           copyLen;

    status    = STATUS_SUCCESS;
    irp       = NULL;
    rw        = NULL;
    allocSize = 0;
    addr      = 0;
    readLen   = 0;
    ctrl      = NULL;
    attempts  = 0;
    backoffUs = 200;
    got       = 0;
    copyLen   = 0;

    if (ext == NULL || buf == NULL || bytesRead == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *bytesRead = 0;

    ctrl = ext->Dpi.ControllerDevice;
    addr = ext->Dpi.I2cAddr7Bit;
    readLen = I2C_INPUT_PACKET_MAX;

    if (ctrl == NULL || addr == 0) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    allocSize = sizeof(I2CCTRL_RW) + readLen - 1;
    rw = (I2CCTRL_RW*)ExAllocatePoolWithTag(NonPagedPool, allocSize, 'RW2I');
    if (rw == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(rw, allocSize);
    rw->Address = addr;
    rw->Length  = (USHORT)readLen;

    while (attempts < 3) {
        KeInitializeEvent(&evt, NotificationEvent, FALSE);

        irp = IoBuildDeviceIoControlRequest(
                  IOCTL_I2C_READ,
                  ctrl,
                  rw,
                  allocSize,
                  rw->Data,
                  readLen,
                  FALSE,
                  &evt,
                  &iosb
              );
        if (irp == NULL) {
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        status = IoCallDriver(ctrl, irp);
        if (status == STATUS_PENDING) {
            KeWaitForSingleObject(&evt, Executive, KernelMode, FALSE, NULL);
            status = iosb.Status;
        }

        if (NT_SUCCESS(status)) {
            got = (ULONG)iosb.Information;
            if (got >= 5) {
                copyLen = (got < readLen) ? got : readLen;
                RtlCopyMemory(buf, rw->Data, copyLen);
                *bytesRead = copyLen;
                break;
            } else {
                status = STATUS_DEVICE_DATA_ERROR;
            }
        }

        attempts++;
        KeStallExecutionProcessor(backoffUs);
        if (backoffUs < 2000) {
            backoffUs *= 2;
        }
    }

    ExFreePoolWithTag(rw, 'RW2I');
    return status;
}

/* === HID completion/report builders === */

/* Complete exactly one pending HID read IRP with provided data */
static VOID
I2cHw_CompleteOneRead(
    PI2CHID_FDO ext,
    PUCHAR data,
    ULONG len
    )
{
    KIRQL oldIrql;
    PIRP irp;
    PIO_STACK_LOCATION isl;
    ULONG outLen;
    ULONG toCopy;

    oldIrql = 0;
    irp = NULL;
    isl = NULL;
    outLen = 0;
    toCopy = 0;

    if (ext == NULL || data == NULL || len == 0) {
        return;
    }

    KeAcquireSpinLock(&ext->ReadQueueLock, &oldIrql);
    if (!IsListEmpty(&ext->ReadQueue)) {
        PLIST_ENTRY entry;
        entry = RemoveHeadList(&ext->ReadQueue);
        irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
    }
    KeReleaseSpinLock(&ext->ReadQueueLock, oldIrql);

    if (irp == NULL) {
        return;
    }

    isl = IoGetCurrentIrpStackLocation(irp);

    /* HIDCLASS typically uses DeviceIoControl with output buffer length */
    outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
    toCopy = (len < outLen) ? len : outLen;

    if (toCopy != 0 && irp->UserBuffer != NULL) {
        RtlCopyMemory(irp->UserBuffer, data, toCopy);
    }

    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = toCopy;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

/* Helper: synthesize a 3-byte mouse report for basic UX */
static VOID
I2cHw_QueueMouseReport(
    PI2CHID_FDO ext,
    UCHAR btnMask,
    CHAR dx,
    CHAR dy
    )
{
    UCHAR report[MOUSE_REPORT_SIZE];

    report[0] = btnMask;
    report[1] = (UCHAR)dx;
    report[2] = (UCHAR)dy;

    I2cHw_CompleteOneRead(ext, report, (ULONG)sizeof(report));
}

/* -----------------------------------------------------------------------
   Build a multitouch HID report supporting up to 5 contacts
   Layout: Byte0 = ContactCount, then 6 bytes per contact:
     Flags, Xlo, Xhi, Ylo, Yhi, ID
   ----------------------------------------------------------------------- */
static VOID
I2cHw_BuildMultitouchReport5(
    PUCHAR buf,
    UCHAR contactCount,
    const USHORT x[MT_MAX_CONTACTS],
    const USHORT y[MT_MAX_CONTACTS],
    const UCHAR  id[MT_MAX_CONTACTS],
    const UCHAR  flags[MT_MAX_CONTACTS] /* bits: 0 Tip, 1 InRange, 2 Primary */
    )
{
    UCHAR i;
    UCHAR c;
    UCHAR off;

    RtlZeroMemory(buf, MT_REPORT_SIZE);

    c = contactCount;
    if (c > MT_MAX_CONTACTS) {
        c = MT_MAX_CONTACTS;
    }

    buf[0] = c;

    for (i = 0; i < c; i++) {
        off = (UCHAR)(1 + (i * MT_CONTACT_BYTES));
        buf[off + 0] = flags[i];
        buf[off + 1] = (UCHAR)(x[i] & 0xFF);
        buf[off + 2] = (UCHAR)((x[i] >> 8) & 0xFF);
        buf[off + 3] = (UCHAR)(y[i] & 0xFF);
        buf[off + 4] = (UCHAR)((y[i] >> 8) & 0xFF);
        buf[off + 5] = id[i];
    }
}

/* === Public entry points (interrupt-driven) === */

NTSTATUS
I2cHw_ConnectInterrupt(PI2CHID_FDO ext)
{
    NTSTATUS status;

    status = STATUS_SUCCESS;

    if (ext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (!ext->InterruptConnected) {
        status = IoConnectInterrupt(
            &ext->InterruptObject,              /* OUT handle */
            I2CHID_InterruptServiceRoutine,     /* ISR */
            ext,                                /* ServiceContext */
            NULL,                               /* SpinLock (optional) */
            ext->InterruptVector,               /* Vector */
            ext->InterruptIrql,                 /* Irql */
            ext->InterruptIrql,                 /* SynchronizeIrql */
            ext->InterruptMode,                 /* LevelSensitive or Latched */
            ext->InterruptSharable,             /* ShareVector */
            ext->InterruptAffinity,             /* ProcessorEnableMask */
            FALSE                               /* FloatingSave */
        );

        if (NT_SUCCESS(status)) {
            /* Bind DPC target to our poller */
            KeInitializeDpc(&ext->InterruptDpc, I2cHw_PollDpc, ext);
            ext->InterruptConnected = TRUE;
        }
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
I2cHw_DisconnectInterrupt(PI2CHID_FDO ext)
{
    if (ext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (ext->InterruptConnected && ext->InterruptObject != NULL) {
        IoDisconnectInterrupt(ext->InterruptObject);
        ext->InterruptObject    = NULL;
        ext->InterruptConnected = FALSE;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
I2cHw_EnableController(PI2CHID_FDO ext)
{
    if (ext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* Nothing special here — device generates interrupts automatically */
    return STATUS_SUCCESS;
}

NTSTATUS
I2cHw_DeviceResume(PI2CHID_FDO ext)
{
    if (ext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* If needed, re-enable interrupt here */
    return STATUS_SUCCESS;
}

NTSTATUS
I2cHw_DeviceSuspend(PI2CHID_FDO ext)
{
    if (ext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* Optionally disconnect or mask interrupt */
    return STATUS_SUCCESS;
}

NTSTATUS
I2cHw_DeviceIdle(PI2CHID_FDO ext)
{
    if (ext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* Optionally mask interrupt while idle */
    return STATUS_SUCCESS;
}

NTSTATUS
I2cHw_EnableWake(PI2CHID_FDO ext, BOOLEAN enable)
{
    if (ext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    ext->WakeEnabled = (enable ? TRUE : FALSE);
    return STATUS_SUCCESS;
}

NTSTATUS
I2cHw_ReprogramDescriptor(PI2CHID_FDO ext)
{
    if (ext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    /* No polling in interrupt mode; nothing to restart */
    return STATUS_SUCCESS;
}
