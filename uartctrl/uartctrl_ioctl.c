/* -----------------------------------------------------------------------
   uartctrl_ioctl.c – IOCTL dispatch for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl.h"
#include "uartctrl_ext.h"
#include "uartctrl_ioctl.h"
#include "uartctrl_hw.h"

/* Dispatch routine for DeviceIoControl and InternalDeviceControl */
NTSTATUS
UARTCTRL_DispatchIoctl(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PUARTCTRL_DEVEXT ext = (PUARTCTRL_DEVEXT)DevObj->DeviceExtension;
    PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    switch (isl->Parameters.DeviceIoControl.IoControlCode) {

    case IOCTL_UART_OPEN:
    {
        // Reset hardware and enable interrupts
        status = UARTCTRL_ExtResetHardware(ext);
        if (!NT_SUCCESS(status)) {
            break;
        }

        UartEnableFifo(ext, FCR_TRIG_8);
        UartSetLineControl(ext, ext->Config.DataBits, ext->Config.StopBits, ext->Config.Parity);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_UART_CLOSE:
    {
        // Disable interrupts
        UartDisableInterrupts(ext);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_UART_SET_CONFIG:
    {
        PUARTCTRL_CONFIG cfg;

        if (isl->Parameters.DeviceIoControl.InputBufferLength < sizeof(UARTCTRL_CONFIG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        cfg = (PUARTCTRL_CONFIG)Irp->AssociatedIrp.SystemBuffer;
        ext->Config = *cfg;

        // Apply to hardware
        UartSetLineControl(ext, cfg->DataBits, cfg->StopBits, cfg->Parity);

        // Note: ext->ClockHz must be initialized during StartDevice
        UartSetBaud(ext, ext->ClockHz, cfg->BaudRate);

        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_UART_GET_CONFIG:
    {
        if (isl->Parameters.DeviceIoControl.OutputBufferLength < sizeof(UARTCTRL_CONFIG)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        *(PUARTCTRL_CONFIG)Irp->AssociatedIrp.SystemBuffer = ext->Config;
        info = sizeof(UARTCTRL_CONFIG);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_UART_WRITE:
    {
        PUCHAR buf;
        ULONG inLen;
        KIRQL irql;
        ULONG i;

        buf = (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
        inLen = isl->Parameters.DeviceIoControl.InputBufferLength;

        if (!buf || inLen == 0) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        KeAcquireSpinLock(&ext->TxLock, &irql);
        for (i = 0; i < inLen; i++) {
            if (!RingPut(ext->TxBuf, ext->TxSize, &ext->TxHead, &ext->TxTail, buf[i])) {
                break; // buffer full
            }
        }
        KeReleaseSpinLock(&ext->TxLock, irql);

        // Kick TX if THR empty
        if (UartReadLineStatus(ext) & LSR_THRE) {
            UCHAR v;
            while (RingGet(ext->TxBuf, ext->TxSize, &ext->TxHead, &ext->TxTail, &v)) {
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
        ULONG outLen;
        ULONG copied = 0;
        KIRQL irql;

        buf = (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
        outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;

        if (!buf || outLen == 0) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        KeAcquireSpinLock(&ext->RxLock, &irql);
        while (copied < outLen) {
            UCHAR v;
            if (!RingGet(ext->RxBuf, ext->RxSize, &ext->RxHead, &ext->RxTail, &v)) {
                break;
            }
            buf[copied++] = v;
        }
        KeReleaseSpinLock(&ext->RxLock, irql);

        if (copied > 0) {
            info = copied;
            status = STATUS_SUCCESS;
        } else {
            KIRQL qirql;

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

        if (isl->Parameters.DeviceIoControl.OutputBufferLength < sizeof(UARTCTRL_STATUS)) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        st = (PUARTCTRL_STATUS)Irp->AssociatedIrp.SystemBuffer;
        st->RxBytesAvailable = RingAvail(ext->RxBuf, ext->RxSize, ext->RxHead, ext->RxTail);
        st->TxBytesFree      = ext->TxSize - RingAvail(ext->TxBuf, ext->TxSize, ext->TxHead, ext->TxTail);
        st->Errors           = ext->RxErrors | ext->TxErrors;
        st->Flags            = 0; // Optional: modem signals if exposed

        info = sizeof(UARTCTRL_STATUS);
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_UART_SET_FLOW_CONTROL:
    {
        // Program RTS/CTS or XON/XOFF as needed (future work)
        status = STATUS_SUCCESS;
        break;
    }

    default:
    {
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    if (status != STATUS_PENDING) {
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }
    return status;
}
