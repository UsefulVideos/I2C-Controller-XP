/* -----------------------------------------------------------------------
   uartctrl_ioctl.c – IOCTL dispatch for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl.h"
#include "uartctrl_ext.h"
#include "uartctrl_ioctl.h"
#include "uartctrl_hw.h"

/* -----------------------------------------------------------------------
 * UARTCTRL_DispatchIoctl – handle DeviceIoControl / InternalDeviceControl
 * ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_DispatchIoctl(
    PDEVICE_OBJECT DevObj,
    PIRP           Irp
    )
{
    PUARTCTRL_FDO       ext;
    PIO_STACK_LOCATION  isl;
    NTSTATUS            status;
    ULONG_PTR           info;

    ext   = (PUARTCTRL_FDO)DevObj->DeviceExtension;
    isl   = IoGetCurrentIrpStackLocation(Irp);
    status = STATUS_INVALID_DEVICE_REQUEST;
    info   = 0;

    UartCtrl_Log("Ioctl: code=0x%08lx\n",
                 isl->Parameters.DeviceIoControl.IoControlCode);

    switch (isl->Parameters.DeviceIoControl.IoControlCode) {

    case IOCTL_UART_OPEN:
    {
        UartCtrl_Log("Ioctl: IOCTL_UART_OPEN\n");

        status = UARTCTRL_ExtResetHardware(ext);
        if (!NT_SUCCESS(status)) {
            UartCtrl_Log("Ioctl: OPEN reset failed (0x%08lx)\n", status);
            break;
        }

        UartEnableFifo(ext, FCR_TRIG_8);
        UartSetLineControl(ext,
                           ext->Config.DataBits,
                           ext->Config.StopBits,
                           ext->Config.Parity);

        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_UART_CLOSE:
    {
        UartCtrl_Log("Ioctl: IOCTL_UART_CLOSE\n");

        UartDisableInterrupts(ext);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_UART_SET_CONFIG:
    {
        PUARTCTRL_CONFIG cfg;

        UartCtrl_Log("Ioctl: IOCTL_UART_SET_CONFIG\n");

        if (isl->Parameters.DeviceIoControl.InputBufferLength <
            sizeof(UARTCTRL_CONFIG)) {

            UartCtrl_Log("Ioctl: SET_CONFIG buffer too small (%lu)\n",
                         isl->Parameters.DeviceIoControl.InputBufferLength);
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        cfg = (PUARTCTRL_CONFIG)Irp->AssociatedIrp.SystemBuffer;
        ext->Config = *cfg;

        UartSetLineControl(ext,
                           cfg->DataBits,
                           cfg->StopBits,
                           cfg->Parity);
        UartSetBaud(ext, ext->ClockHz, cfg->BaudRate);

        UartCtrl_Log("Ioctl: SET_CONFIG baud=%u data=%u stop=%u parity=%u\n",
                     cfg->BaudRate,
                     cfg->DataBits,
                     cfg->StopBits,
                     cfg->Parity);

        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_UART_GET_CONFIG:
    {
        UartCtrl_Log("Ioctl: IOCTL_UART_GET_CONFIG\n");

        if (isl->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(UARTCTRL_CONFIG)) {

            UartCtrl_Log("Ioctl: GET_CONFIG buffer too small (%lu)\n",
                         isl->Parameters.DeviceIoControl.OutputBufferLength);
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        *(PUARTCTRL_CONFIG)Irp->AssociatedIrp.SystemBuffer = ext->Config;
        info   = sizeof(UARTCTRL_CONFIG);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_UART_WRITE:
    {
        PUCHAR buf;
        ULONG  inLen;
        KIRQL  irql;
        ULONG  i;

        UartCtrl_Log("Ioctl: IOCTL_UART_WRITE\n");

        buf   = (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress,
                                                     NormalPagePriority);
        inLen = isl->Parameters.DeviceIoControl.InputBufferLength;

        if (buf == NULL || inLen == 0) {
            UartCtrl_Log("Ioctl: WRITE invalid buffer/length\n");
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        KeAcquireSpinLock(&ext->TxLock, &irql);
        for (i = 0; i < inLen; i++) {
            if (!RingPut(ext->TxBuf,
                         ext->TxSize,
                         &ext->TxHead,
                         &ext->TxTail,
                         buf[i])) {
                break; /* buffer full */
            }
        }
        KeReleaseSpinLock(&ext->TxLock, irql);

        UartCtrl_Log("Ioctl: WRITE queued %lu bytes\n", i);

        /* Kick TX if THR empty */
        if (UartReadLineStatus(ext) & LSR_THRE) {
            UCHAR v;
            while (RingGet(ext->TxBuf,
                           ext->TxSize,
                           &ext->TxHead,
                           &ext->TxTail,
                           &v)) {

                UartWriteByte(ext, v);
                if (!(UartReadLineStatus(ext) & LSR_THRE)) {
                    break;
                }
            }
        }

        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_UART_READ:
    {
        PUCHAR buf;
        ULONG  outLen;
        ULONG  copied;
        KIRQL  irql;

        UartCtrl_Log("Ioctl: IOCTL_UART_READ\n");

        buf    = (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress,
                                                      NormalPagePriority);
        outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;
        copied = 0;

        if (buf == NULL || outLen == 0) {
            UartCtrl_Log("Ioctl: READ invalid buffer/length\n");
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        KeAcquireSpinLock(&ext->RxLock, &irql);
        while (copied < outLen) {
            UCHAR v;
            if (!RingGet(ext->RxBuf,
                         ext->RxSize,
                         &ext->RxHead,
                         &ext->RxTail,
                         &v)) {
                break;
            }
            buf[copied++] = v;
        }
        KeReleaseSpinLock(&ext->RxLock, irql);

        if (copied > 0) {
            UartCtrl_Log("Ioctl: READ returned %lu bytes\n", copied);
            info   = copied;
            status = STATUS_SUCCESS;
        } else {
            KIRQL qirql;

            UartCtrl_Log("Ioctl: READ no data, queuing IRP %p\n", Irp);

            IoMarkIrpPending(Irp);
            KeAcquireSpinLock(&ext->ReadQueueLock, &qirql);
            InsertTailList(&ext->ReadQueue, &Irp->Tail.Overlay.ListEntry);
            KeReleaseSpinLock(&ext->ReadQueueLock, qirql);

            return STATUS_PENDING;
        }
        break;
    }

    case IOCTL_UART_GET_STATUS:
    {
        PUARTCTRL_STATUS st;

        UartCtrl_Log("Ioctl: IOCTL_UART_GET_STATUS\n");

        if (isl->Parameters.DeviceIoControl.OutputBufferLength <
            sizeof(UARTCTRL_STATUS)) {

            UartCtrl_Log("Ioctl: GET_STATUS buffer too small (%lu)\n",
                         isl->Parameters.DeviceIoControl.OutputBufferLength);
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        st = (PUARTCTRL_STATUS)Irp->AssociatedIrp.SystemBuffer;

        st->RxBytesAvailable =
            RingAvail(ext->RxBuf,
                      ext->RxSize,
                      ext->RxHead,
                      ext->RxTail);

        st->TxBytesFree =
            ext->TxSize -
            RingAvail(ext->TxBuf,
                      ext->TxSize,
                      ext->TxHead,
                      ext->TxTail);

        st->Errors = ext->RxErrors | ext->TxErrors;
        st->Flags  = 0; /* optional modem flags */

        info   = sizeof(UARTCTRL_STATUS);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_UART_SET_FLOW_CONTROL:
    {
        UartCtrl_Log("Ioctl: IOCTL_UART_SET_FLOW_CONTROL (stub)\n");
        /* Future: program RTS/CTS or XON/XOFF */
        status = STATUS_SUCCESS;
        break;
    }

    default:
    {
        UartCtrl_Log("Ioctl: unsupported code 0x%08lx\n",
                     isl->Parameters.DeviceIoControl.IoControlCode);
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    }

    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = info;

    if (status != STATUS_PENDING) {
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    UartCtrl_Log("Ioctl: done, status=0x%08lx, info=%lu\n",
                 status, (ULONG)info);

    return status;
}
