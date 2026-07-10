#include <ntddk.h>
#include "i2cctrl_ioctl.h"        // IOCTL_GET_PT_SAMPLE
#include "i2cctrl_ext.h"          // I2CCTRL_DEVEXT
#include "..\i2chid\I2CHID_EXT.h" // PT_RAW_SAMPLE definition
#include "i2cctrl_zw.h"

// Utility: complete an IRP
VOID
I2CCTRL_CompleteIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Info)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

NTSTATUS
I2cCtrl_Transfer(PDEVICE_OBJECT DevObj,
                 HANDLE ControllerHandle,
                 USHORT SlaveAddress,
                 ULONG  Direction,
                 PUCHAR Buffer,
                 ULONG  Length,
                 PULONG BytesTransferred)
{
    NTSTATUS         status;
    IO_STATUS_BLOCK  ioStatus;
    I2CCTRL_TRANSFER packet;

    /* C89: declare and initialize variables at start */
    status  = STATUS_SUCCESS;
    RtlZeroMemory(&ioStatus, sizeof(ioStatus));
    RtlZeroMemory(&packet, sizeof(packet));

    if (ControllerHandle == NULL || Buffer == NULL || Length == 0U) {
        if (BytesTransferred) {
            *BytesTransferred = 0;
        }
        return STATUS_INVALID_PARAMETER;
    }

    /* Populate packet */
    packet.NumMessages   = 1;            /* single message transfer */
    packet.SlaveAddress  = SlaveAddress;
    packet.Direction     = Direction;
    packet.Buffer        = Buffer;
    packet.Length        = Length;
    packet.BytesReturned = 0;

    /* Use duplicate dispatcher instead of ZwDeviceIoControlFile */
    status = I2cCtrl_ControlFile(
				 DevObj,                 /* PDEVICE_OBJECT */
                 ControllerHandle,       /* FileHandle */
                 NULL,                   /* Event */
                 NULL,                   /* APC routine */
                 NULL,                   /* APC context */
                 &ioStatus,              /* IO_STATUS_BLOCK */
                 IOCTL_I2cCtrl_TRANSFER, /* IoControlCode */
                 &packet, sizeof(packet),/* InputBuffer + length */
                 &packet, sizeof(packet) /* OutputBuffer + length */
             );

    if (NT_SUCCESS(status)) {
        if (BytesTransferred) {
            *BytesTransferred = packet.BytesReturned;
        }
    } else {
        if (BytesTransferred) {
            *BytesTransferred = 0;
        }
    }

    return status;
}

NTSTATUS
I2CCTRL_FillSample(PI2CHID_PT_DEVEXT ext,
                   PT_RAW_SAMPLE* out)
{
    KIRQL   oldIrql;
    NTSTATUS status;
    ULONG   bytesRead;
    USHORT  inputReg;

    oldIrql   = PASSIVE_LEVEL;
    status    = STATUS_SUCCESS;
    bytesRead = 0U;
    inputReg  = (USHORT)ext->FdoExt->InputRegisterAddress;

    if (ext == NULL || out == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&ext->SampleLock, &oldIrql);

    RtlZeroMemory(out, sizeof(*out));

    /* HID-over-I²C sequence */
    status = I2cCtrl_Transfer(
                 ext->FdoExt->Self,             /* PDEVICE_OBJECT DevObj */
                 ext->ControllerHandle,         /* HANDLE */
                 (USHORT)ext->I2cAddress,       /* USHORT */
                 I2C_DIRECTION_WRITE,           /* ULONG */
                 (PUCHAR)&inputReg,             /* PUCHAR */
                 sizeof(inputReg),              /* ULONG */
                 &bytesRead);                   /* PULONG */

    if (NT_SUCCESS(status)) {
        status = I2cCtrl_Transfer(
                     ext->FdoExt->Self,
                     ext->ControllerHandle,
                     (USHORT)ext->I2cAddress,
                     I2C_DIRECTION_READ,
                     (PUCHAR)out,
                     sizeof(*out),
                     &bytesRead);
    }

    if (NT_SUCCESS(status) && bytesRead >= sizeof(PT_RAW_SAMPLE)) {
        ext->SampleValid = TRUE;
    } else {
        ext->SampleValid = FALSE;
    }

    KeReleaseSpinLock(&ext->SampleLock, oldIrql);
    return status;
}



// ----------------------------------------------------------------------
// HID helper: serve IOCTL_GET_PT_SAMPLE
// ----------------------------------------------------------------------
NTSTATUS
I2CCTRL_GetPtSampleForHid(
    PI2CHID_PT_DEVEXT ext,
    PT_RAW_SAMPLE* outSample
    )
{
    if (!ext || !outSample) {
        return STATUS_INVALID_PARAMETER;
    }

    // Delegate to the common fill routine
    return I2CCTRL_FillSample(ext, outSample);
}


