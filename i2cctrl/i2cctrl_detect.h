#ifndef _I2CCTRL_DETECT_H_
#define _I2CCTRL_DETECT_H_

#include <ntddk.h>
#include "i2cctrl_ext.h"   /* brings in I2CCTRL_FDO / PDO types */
#include "i2cctrl_hw.h"    /* central register/bit definitions */

/* ---------------------------------------------------------------------------
   HID-over-I²C descriptor v1.0 layout (per HID over I²C spec 1.0)
   --------------------------------------------------------------------------- */
typedef struct _HID_I2C_DESCRIPTOR_V10 {
    USHORT wHIDDescLength;
    USHORT bcdVersion;
    USHORT wReportDescLength;
    USHORT wReportDescRegister;
    USHORT wInputRegister;
    USHORT wMaxInputLength;
    USHORT wOutputRegister;
    USHORT wMaxOutputLength;
    USHORT wCommandRegister;
    USHORT wDataRegister;
    USHORT wVendorID;
    USHORT wProductID;
    USHORT wVersionID;
} HID_I2C_DESCRIPTOR_V10, *PHID_I2C_DESCRIPTOR_V10;

/* ---------------------------------------------------------------------------
   Result of detection
   --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_DETECT_RESULT {
    BOOLEAN Found;
    UCHAR   Address;        /* 7-bit I²C address */
    USHORT  HidDescLength;  /* length of HID descriptor */
    USHORT  VendorID;
    USHORT  ProductID;
    USHORT  VersionID;
	BOOLEAN Present;
	BOOLEAN IsTouchpad;
    /* Future expansion: could add ReportDescLength, etc. */
} I2CCTRL_DETECT_RESULT, *PI2CCTRL_DETECT_RESULT;

/* ---------------------------------------------------------------------------
   Public entry points
   --------------------------------------------------------------------------- */

/* Probe bus for HID-over-I²C touchpad and fill result */
NTSTATUS
I2cCtrl_DetectTouchpad(
    PI2CCTRL_FDO dx,
    PI2CCTRL_DETECT_RESULT result
    );

/* Read and validate HID-over-I²C descriptor at given slave address */
NTSTATUS
I2cCtrl_ReadAndValidateHidDescriptor(
    PI2CCTRL_FDO          dx,        /* bus/device extension */
    UCHAR                 addr,      /* 7-bit I²C address of HID device */
    PUCHAR                outBuf,    /* caller-supplied buffer for raw bytes */
    ULONG                 outBufLen, /* length of buffer in bytes */
    PHID_I2C_DESCRIPTOR_V10 parsed   /* parsed descriptor structure */
    );

/* ---------------------------------------------------------------------------
   Internal helper
   --------------------------------------------------------------------------- */

/* Low-level read from HID register */
NTSTATUS
I2cCtrl_ReadHidRegister(
    PI2CCTRL_FDO dx,
    UCHAR        addr,
    UCHAR        reg,
    PUCHAR       buffer,
    ULONG        length,
    ULONG        timeoutUs
    );

/* ---------------------------------------------------------------------------
   HID-over-I²C helpers (prototypes only)
   --------------------------------------------------------------------------- */

NTSTATUS
I2cCtrl_ReadHidDescriptor(
    PI2CCTRL_FDO     fdo,
    PI2CCTRL_PDO hidpdo,
    PHID_DESCRIPTOR  outDesc
    );

NTSTATUS
I2cCtrl_ReadReportDescriptor(
    PI2CCTRL_FDO     fdo,
    PI2CCTRL_PDO hidpdo,
    PUCHAR           buf,
    ULONG            len
    );

// ---------------------------------------------------------------------------
// Validate HID descriptor bytes and parse fields (HID-I²C v1.0, HAL-neutral, XP-safe)
// ---------------------------------------------------------------------------
BOOLEAN
ParseHidDescriptorV10(
    const UCHAR*              buf,
    ULONG                     len,
    PHID_I2C_DESCRIPTOR_V10   out
    );

#endif /* _I2CCTRL_DETECT_H_ */
