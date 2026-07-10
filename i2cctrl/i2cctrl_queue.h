/* i2cctrl_Queue->h
 * Queue management for SMBus transactions in the I2C controller driver.
 * C89 compliant, XP/2003 build environment.
 */

#ifndef _I2cCtrl_QUEUE_H_
#define _I2cCtrl_QUEUE_H_

#include <ntddk.h>
#include "i2cctrl_ext.h"
#include "i2cctrl_hw.h"
#include "i2cctrl_bsod.h"

/* ---------------------------------------------------------------------------
   Public prototypes for queue lifecycle
   --------------------------------------------------------------------------- */

/* Initialize queue structures (called in StartDevice) */
VOID
I2cCtrl_InitQueue(
    PI2CCTRL_FDO Dx
    );

/* Flush and cancel all queued requests (called in Stop/Remove) */
VOID
I2cCtrl_FlushQueue(
    PI2CCTRL_FDO Dx
    );

#endif /* _I2cCtrl_QUEUE_H_ */
