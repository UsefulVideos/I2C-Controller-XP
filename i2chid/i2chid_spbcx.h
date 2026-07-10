/* I2CHID_spbcx.h
 * HID‑over‑I²C façade: IOCTLs only.
 * C89 compliant.
 */

#ifndef _I2CHID_SPBCX_H_
#define _I2CHID_SPBCX_H_

#include <ntddk.h>
#include "I2CHID_EXT.h"
#include "i2chid.h"         /* I2C_READ_PARAMS / I2C_WRITE_PARAMS */
/* deliberately NOT including I2CHID_hid.h to avoid circular include */

/* Bring in bus IOCTL contract types (I2CCTRL_TARGET, IOCTL codes, etc.) */
#include "..\i2cctrl\i2cctrl_spbcx.h"   /* I2CCTRL_TARGET and helper prototypes */

/* ---------------------------------------------------------------------------
   Forward declaration of bus driver context
   --------------------------------------------------------------------------- */
struct _I2CCTRL_FDO;
typedef struct _I2CCTRL_FDO I2CCTRL_FDO, *PI2CCTRL_FDO;

/* ---------------------------------------------------------------------------
   HID façade IOCTLs
   --------------------------------------------------------------------------- */
#define IOCTL_I2CHID_SET_TARGET   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CHID_READ_REPORT  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CHID_WRITE_REPORT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ---------------------------------------------------------------------------
   Function prototypes for façade IOCTL handlers (mirror bus prototypes)
   --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_IoctlSetTarget(
    PI2CCTRL_FDO Dx,
    PI2CCTRL_TARGET         Tgt,
    PVOID                   InBuf,
    ULONG                   InLen
    );

/* HID‑side façade IOCTLs */
NTSTATUS
I2CHID_IoctlSetTarget(
    PI2CHID_FDO Ext,
    PVOID           InBuf,
    ULONG           InLen
    );

NTSTATUS
I2CHID_IoctlReadReport(
    PI2CHID_FDO Ext,
    PVOID           OutBuf,
    ULONG           OutLen
    );

NTSTATUS
I2CHID_IoctlWriteReport(
    PI2CHID_FDO Ext,
    PVOID           InBuf,
    ULONG           InLen
    );


#endif /* _I2CHID_SPBCX_H_ */
