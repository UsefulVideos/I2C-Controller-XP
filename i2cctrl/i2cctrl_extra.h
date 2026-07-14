/* i2cctrl_extra.h
 *
 * Additional definitions for probe results and sequence helpers.
 * Not to be confused with i2cctrl_ext.h.
 */

#ifndef _I2CCTRL_EXTRA_H_
#define _I2CCTRL_EXTRA_H_

#include <ntddk.h>

/* Forward declaration so the compiler knows this is a type */
typedef struct _I2C_TRANSFER_CONTEXT I2C_TRANSFER_CONTEXT;

/* ---------------------------------------------------------------------------
   Probe result structure for IOCTL_PROBE
   --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_PROBE_RESULT {
    UCHAR   Address7Bit;  /* probed address (7-bit) */
    BOOLEAN Found;        /* TRUE if device acknowledged */
    UCHAR   TenBit;       /* 0 = 7-bit, 1 = 10-bit */
    UCHAR   Reserved;     /* alignment/padding */
} I2CCTRL_PROBE_RESULT, *PI2CCTRL_PROBE_RESULT;

/* ---------------------------------------------------------------------------
   Helper to return number of bytes written in last transfer
   --------------------------------------------------------------------------- */
ULONG
I2cCtrl_GetTransferBytesWritten(
    I2C_TRANSFER_CONTEXT *xc
    );

/* ---------------------------------------------------------------------------
   Helper to return number of bytes written in last sequence
   --------------------------------------------------------------------------- */
ULONG
I2cCtrl_GetSequenceBytesWritten(
    I2C_TRANSFER_CONTEXT *xc
    );

#endif /* _I2CCTRL_EXTRA_H_ */
