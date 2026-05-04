/* -----------------------------------------------------------------------
   uartctrl_power.c – Power management dispatch for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl.h"
#include "uartctrl_ext.h"
#include "uartctrl_hw.h"

/* -----------------------------------------------------------------------
   Power Dispatch
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_DispatchPower(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PUARTCTRL_DEVEXT ext;
    PIO_STACK_LOCATION isl;

    ext = (PUARTCTRL_DEVEXT)DevObj->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    PoStartNextPowerIrp(Irp);

    switch (isl->MinorFunction) {

    case IRP_MN_QUERY_POWER:
        /* Allow all queries; could add checks if hardware cannot support certain states */
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_SET_POWER:
        /* Handle transitions to D0/Dx */
        if (isl->Parameters.Power.State.DeviceState == PowerDeviceD0) {
            /* Re‑enable UART hardware after resume */
            UartEnableFifo(ext, FCR_TRIG_8);
            UartSetLineControl(ext,
                               ext->Config.DataBits,
                               ext->Config.StopBits,
                               ext->Config.Parity);
            UartSetBaud(ext, ext->ClockHz, ext->Config.BaudRate);
            UartSetModemControl(ext, MCR_RTS | MCR_OUT2);
            UartEnableInterrupts(ext, IER_RDA | IER_THRE | IER_RLS);
        } else {
            /* Transitioning to low‑power state: disable interrupts */
            UartDisableInterrupts(ext);
        }
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_WAIT_WAKE:
        /* Optional: support wake‑on‑RX if hardware allows */
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        break;

    default:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(ext->LowerDevice, Irp);
}
