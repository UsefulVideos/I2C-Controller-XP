/* -----------------------------------------------------------------------
   uartctrl_power.c – Power management dispatch for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl.h"
#include "uartctrl_ext.h"
#include "uartctrl_hw.h"

/* -----------------------------------------------------------------------
 * UARTCTRL_DispatchPower – handle power IRPs (XP/2003-safe)
 * ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_DispatchPower(
    PDEVICE_OBJECT DevObj,
    PIRP           Irp
    )
{
    PUARTCTRL_FDO      ext;
    PIO_STACK_LOCATION isl;

    ext = (PUARTCTRL_FDO)DevObj->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    /* XP/2003 requirement */
    PoStartNextPowerIrp(Irp);

    UartCtrl_Log("Power: minor=0x%02X\n", isl->MinorFunction);

    switch (isl->MinorFunction) {

    /* -------------------------------------------------------------
     * QUERY_POWER – always allow
     * ------------------------------------------------------------- */
    case IRP_MN_QUERY_POWER:
        UartCtrl_Log("Power: IRP_MN_QUERY_POWER\n");
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    /* -------------------------------------------------------------
     * SET_POWER – handle D0/Dx transitions
     * ------------------------------------------------------------- */
    case IRP_MN_SET_POWER:
    {
        DEVICE_POWER_STATE newState =
            isl->Parameters.Power.State.DeviceState;

        UartCtrl_Log("Power: IRP_MN_SET_POWER -> D%u\n", newState);

        if (newState == PowerDeviceD0) {

            UartCtrl_Log("Power: entering D0, reinitializing UART\n");

            /* Re-enable UART hardware after resume */
            UartEnableFifo(ext, FCR_TRIG_8);
            UartSetLineControl(ext,
                               ext->Config.DataBits,
                               ext->Config.StopBits,
                               ext->Config.Parity);
            UartSetBaud(ext, ext->ClockHz, ext->Config.BaudRate);
            UartSetModemControl(ext, MCR_RTS | MCR_OUT2);
            UartEnableInterrupts(ext,
                                 IER_RDA | IER_THRE | IER_RLS);

        } else {

            UartCtrl_Log("Power: entering Dx, disabling interrupts\n");

            /* Suspend: disable interrupts */
            UartDisableInterrupts(ext);
        }

        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    }

    /* -------------------------------------------------------------
     * WAIT_WAKE – optional, unsupported
     * ------------------------------------------------------------- */
    case IRP_MN_WAIT_WAKE:
        UartCtrl_Log("Power: IRP_MN_WAIT_WAKE (not supported)\n");
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        break;

    /* -------------------------------------------------------------
     * Default – pass through
     * ------------------------------------------------------------- */
    default:
        UartCtrl_Log("Power: passing minor 0x%02X down\n",
                     isl->MinorFunction);
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    }

    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(ext->LowerDevice, Irp);
}
