/* i2cctrl_acpiex.h
 * Minimal ACPIEx-like façade for XP/2003, reusing SpbCx-style contracts.
 * C89 compliant
 */

#ifndef _I2CCTRL_ACPIEX_H_
#define _I2CCTRL_ACPIEX_H_

#include <ntddk.h>

#include "i2cctrl_ext.h"     /* MUST COME FIRST: defines _I2CCTRL_TARGET */
#include "i2cctrl_hw.h"      /* pool tags, IOCTL codes */
#include "i2cctrl_spbcx.h"   /* I2CCTRL_TARGET, I2CCTRL_TRANSFER, etc. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _I2CCTRL_ACPIEX_HANDLE {
    I2CCTRL_TARGET Target;   /* bound target state */
} I2CCTRL_ACPIEX_HANDLE, *PI2CCTRL_ACPIEX_HANDLE;

/* Public APIs */
VOID I2cCtrl_AcpiexInit(PI2CCTRL_FDO Dx);
NTSTATUS I2cCtrl_AcpiexCreate(PDEVICE_OBJECT Fdo, PIRP Irp, PIO_STACK_LOCATION Isl);
NTSTATUS I2cCtrl_AcpiexClose(PDEVICE_OBJECT Fdo, PIRP Irp, PIO_STACK_LOCATION Isl);
NTSTATUS I2cCtrl_AcpiexDeviceControl(PDEVICE_OBJECT Fdo, PIRP Irp, PIO_STACK_LOCATION Isl);

NTSTATUS I2cCtrl_AcpiexSetTarget(
    PI2CCTRL_FDO Dx,
    PI2CCTRL_ACPIEX_HANDLE H,
    I2CCTRL_TARGET In
);

NTSTATUS I2cCtrl_AcpiexTransfer(
    PI2CCTRL_FDO Dx,
    PI2CCTRL_ACPIEX_HANDLE H,
    I2CCTRL_XFER_DESC In,
    PUCHAR Buf,
    SIZE_T BufLen
);

NTSTATUS I2cCtrl_AcpiexSequence(
    PI2CCTRL_FDO Dx,
    PI2CCTRL_ACPIEX_HANDLE H,
    PI2CCTRL_SEQUENCE_HDR Seq,
    PUCHAR Buf,
    SIZE_T BufLen
);

NTSTATUS I2cCtrl_AcpiexProbe(
    PI2CCTRL_FDO Dx,
    PI2CCTRL_ACPIEX_HANDLE H,
    I2CCTRL_PROBE In,
    PULONG OutPresenceMask
);

#ifdef __cplusplus
}
#endif

#endif /* _I2CCTRL_ACPIEX_H_ */
