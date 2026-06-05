/* -----------------------------------------------------------------------
   uartctrl_pt.c – pass-through dispatch for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl.h"
#include "uartctrl_ext.h"

/* -----------------------------------------------------------------------
 * UARTCTRL_PT_DispatchPass – default pass‑through handler
 * ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_PT_DispatchPass(
    PDEVICE_OBJECT DevObj,
    PIRP           Irp
    )
{
    PIO_STACK_LOCATION isl;

    UNREFERENCED_PARAMETER(DevObj);

    isl = IoGetCurrentIrpStackLocation(Irp);

    /* Safe to log: always PASSIVE_LEVEL */
    UartCtrl_Log("PT: major=0x%02X minor=0x%02X\n",
                 isl->MajorFunction,
                 isl->MinorFunction);

    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}
