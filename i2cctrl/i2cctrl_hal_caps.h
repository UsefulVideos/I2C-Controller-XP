/* i2cctrl_hal_caps.h */

#ifndef I2CCTRL_HAL_CAPS_H
#define I2CCTRL_HAL_CAPS_H

#include <ntddk.h>   /* for ULONG, BOOLEAN */

/* ---------------------------------------------------------------------------
   Hardware-neutral capabilities
   --------------------------------------------------------------------------- */
typedef struct _I2C_HW_CAPS {
    /* FIFO depths reported by hardware */
    ULONG   TxFifoDepth;       /* TX FIFO depth in entries */
    ULONG   RxFifoDepth;       /* RX FIFO depth in entries */

    /* Feature flags */
    BOOLEAN SupportsRestart;           /* Controller can issue RESTART */
    BOOLEAN SupportsStopBitInDataCmd;  /* STOP bit can be encoded in DATA_CMD */
    BOOLEAN HasSeparateIntrClearRegs;  /* Separate registers for interrupt clear */
    BOOLEAN HasDedicatedIntrMask;      /* Has INT_MASK register */
    BOOLEAN HasRawIntrStatus;          /* Has raw interrupt status register */

    /* Addressing capabilities */
    BOOLEAN Supports10BitAddr;         /* Supports 10-bit addressing */
    BOOLEAN SupportsSlaveMode;         /* Can act as slave device */

    /* Arbitration / error handling */
    BOOLEAN DetectsArbitrationLost;    /* Can detect arbitration lost */
    BOOLEAN DetectsAddressNack;        /* Can detect address NACK */
    BOOLEAN DetectsDataNack;           /* Can detect data NACK */

    /* Speed capabilities */
    BOOLEAN SupportsStandard100k;      /* Standard-mode (100 kHz) */
    BOOLEAN SupportsFast400k;          /* Fast-mode (400 kHz) */
    BOOLEAN SupportsHigh3_4M;          /* High-speed mode (3.4 MHz) */

    /* Maximum achievable bus speed in Hz */
    ULONG   MaxSpeedHz;

    /* Input clock frequency if known (0 if not reported) */
    ULONG   InputClockHz;
} I2C_HW_CAPS, *PI2C_HW_CAPS;

#endif /* I2CCTRL_HAL_CAPS_H */
