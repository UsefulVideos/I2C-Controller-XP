/* -----------------------------------------------------------------------
   uartctrl_pt.c – pass-through dispatch for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl.h"
#include "uartctrl_ext.h"

/* -----------------------------------------------------------------------
   Pass-through dispatch routine
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_PT_DispatchPass(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PUARTCTRL_DEVEXT ext = (PUARTCTRL_DEVEXT)DevObj->DeviceExtension;
    UNREFERENCED_PARAMETER(ext);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;
}
