/* i2chid_spbcx.c
 * HID‑over‑I²C façade implementation.
 * C89 compliant.
 */

#include <ntddk.h>
#include "i2chid_spbcx.h"
#include "i2chid_hid.h"
#include "../i2cctrl/i2cctrl_ioctl.h"  /* shared IOCTL contract only */
#include "../i2cctrl/i2cctrl_ext.h"    /* shared structs like I2CCTRL_FDO */

/* Bus‑side IOCTLs (called by HID shim, bridge to controller PDO) */
NTSTATUS
I2cCtrl_IoctlSetTarget(
    PI2CCTRL_FDO devctx,
    PI2CCTRL_TARGET         Tgt,
    PVOID                   InBuf,
    ULONG                   InLen
    )
{
    UNREFERENCED_PARAMETER(devctx);
    UNREFERENCED_PARAMETER(Tgt);
    UNREFERENCED_PARAMETER(InBuf);
    UNREFERENCED_PARAMETER(InLen);

    /* i2chid.sys should not set bus state directly.
       Send the target to the bus PDO via IOCTL. */
    return STATUS_NOT_SUPPORTED;
}

/* ---------------------------------------------------------------------------
   HID‑side IOCTLs (called by HID class driver, bridge down to controller)
   --------------------------------------------------------------------------- */
NTSTATUS
I2CHID_IoctlSetTarget(
    PI2CHID_FDO Ext,
    PVOID           InBuf,
    ULONG           InLen
    )
{
    I2CCTRL_TARGET tgt;
    NTSTATUS status;
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;

    if (Ext == NULL || Ext->ParentFdo == NULL || InBuf == NULL || InLen < sizeof(UCHAR)) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&tgt, sizeof(tgt));
    tgt.Address = *((PUCHAR)InBuf);
    tgt.Flags   = 0;
    tgt.Bound   = TRUE;

    Ext->I2cAddr7Bit = (UCHAR)tgt.Address;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
              IOCTL_SET_TARGET,
              Ext->ParentFdo,
              &tgt, sizeof(tgt),
              &tgt, sizeof(tgt),
              FALSE,
              &event,
              &iosb
          );
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(Ext->ParentFdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    return status;
}

NTSTATUS
I2CHID_IoctlReadReport(
    PI2CHID_FDO Ext,
    PVOID           OutBuf,
    ULONG           OutLen
    )
{
    NTSTATUS status;
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;

    if (Ext == NULL || Ext->ParentFdo == NULL || OutBuf == NULL || OutLen == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
              IOCTL_TRANSFER,
              Ext->ParentFdo,
              OutBuf, OutLen,
              OutBuf, OutLen,
              FALSE,
              &event,
              &iosb
          );
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(Ext->ParentFdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    return status;
}

NTSTATUS
I2CHID_IoctlWriteReport(
    PI2CHID_FDO Ext,
    PVOID           InBuf,
    ULONG           InLen
    )
{
    NTSTATUS status;
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;

    if (Ext == NULL || Ext->ParentFdo == NULL || InBuf == NULL || InLen == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
              IOCTL_TRANSFER,
              Ext->ParentFdo,
              InBuf, InLen,
              InBuf, InLen,
              FALSE,
              &event,
              &iosb
          );
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(Ext->ParentFdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }
    return status;
}
