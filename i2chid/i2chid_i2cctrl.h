#pragma once

#include <ntddk.h>
#include "..\i2cctrl\i2cctrl_ioctl.h"
#include "I2CHID_EXT.h"

// ----------------------------------------------------------------------
// Glue layer between i2chid.sys and i2cctrl.sys
// Provides helpers to open/close the controller and issue IOCTLs
// ----------------------------------------------------------------------

// Open a handle to the I2C controller device (\Device\I2CCTRL)
NTSTATUS
I2CHID_I2cCtrl_Open(
    PHANDLE Handle
    );

// Close a previously opened controller handle
VOID
I2CHID_I2cCtrl_Close(
    HANDLE Handle
    );

// Fetch a raw touchpad sample via IOCTL_GET_PT_SAMPLE
NTSTATUS
I2CHID_I2cCtrl_GetSample(
    HANDLE Handle,
    PT_RAW_SAMPLE* Sample
    );

// Generic helper to send any IOCTL to the controller
NTSTATUS
I2CHID_I2cCtrl_Ioctl(
    HANDLE Handle,
    ULONG IoctlCode,
    PVOID InBuf,
    ULONG InLen,
    PVOID OutBuf,
    ULONG OutLen
    );
