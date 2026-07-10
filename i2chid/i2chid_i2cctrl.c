#include <ntddk.h>
#include "..\i2cctrl\i2cctrl_ioctl.h"
#include "I2CHID_EXT.h"
#include "I2CHID_i2cctrl.h"

// Open a handle to the I2C controller device
NTSTATUS I2CHID_I2cCtrl_Open(PHANDLE Handle)
{
    UNICODE_STRING ctrlName;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    RtlInitUnicodeString(&ctrlName, L"\\Device\\I2CCTRL");
    InitializeObjectAttributes(&oa, &ctrlName, OBJ_KERNEL_HANDLE, NULL, NULL);

    return ZwCreateFile(
        Handle,
        GENERIC_READ | GENERIC_WRITE,
        &oa,
        &iosb,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        0,
        FILE_OPEN,
        0,
        NULL,
        0
    );
}

// Close controller handle
VOID I2CHID_I2cCtrl_Close(HANDLE Handle)
{
    if (Handle) ZwClose(Handle);
}

// Fetch a raw touchpad sample via IOCTL
NTSTATUS I2CHID_I2cCtrl_GetSample(HANDLE Handle, PT_RAW_SAMPLE* Sample)
{
    IO_STATUS_BLOCK iosb;
    return ZwDeviceIoControlFile(
        Handle,
        NULL, NULL, NULL,
        &iosb,
        IOCTL_GET_PT_SAMPLE,
        NULL, 0,
        Sample, sizeof(*Sample)
    );
}

// Generic helper to send any IOCTL to the controller
NTSTATUS I2CHID_I2cCtrl_Ioctl(
    HANDLE Handle,
    ULONG IoctlCode,
    PVOID InBuf,
    ULONG InLen,
    PVOID OutBuf,
    ULONG OutLen
    )
{
    IO_STATUS_BLOCK iosb;
    return ZwDeviceIoControlFile(
        Handle,
        NULL, NULL, NULL,
        &iosb,
        IoctlCode,
        InBuf, InLen,
        OutBuf, OutLen
    );
}
