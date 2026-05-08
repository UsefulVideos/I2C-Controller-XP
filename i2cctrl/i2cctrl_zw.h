#pragma once
#include <ntddk.h>   // for HANDLE, NTSTATUS, etc.

NTSTATUS
I2CctrlHw_WaitForIdle(PDEVICE_OBJECT DevObj,
                      ULONG TimeoutMs);

/*
 * Local duplicate of ZwDeviceIoControlFile, renamed to I2Cctrl_ControlFile.
 * This is a direct replacement, not a wrapper or a different semantic.
 */
NTSTATUS NTAPI I2Cctrl_ControlFile(
    PDEVICE_OBJECT DevObj,           /* added: device object for hardware helpers */
    HANDLE         FileHandle,
    HANDLE         Event OPTIONAL,
    PIO_APC_ROUTINE ApcRoutine OPTIONAL,
    PVOID          ApcContext OPTIONAL,
    PIO_STATUS_BLOCK IoStatusBlock,
    ULONG          IoControlCode,
    PVOID          InputBuffer OPTIONAL,
    ULONG          InputBufferLength,
    PVOID          OutputBuffer OPTIONAL,
    ULONG          OutputBufferLength
);
