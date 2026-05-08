/* i2cctrl_hal.h */

#ifndef I2CCTRL_HAL_H
#define I2CCTRL_HAL_H

#include <ntddk.h>   /* for NTSTATUS, PCM_RESOURCE_LIST, PIRP, etc. */

/* Bring in capabilities and ops definitions */
#include "i2cctrl_hal_caps.h"
#include "i2cctrl_hal_ops.h"

/* ---------------------------------------------------------------------------
   Forward declaration of your device context type.
   The full struct _I2CCTRL_FDO is defined elsewhere.
   --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_FDO I2CCTRL_FDO;
typedef I2CCTRL_FDO* PI2CCTRL_FDO;

/* ---------------------------------------------------------------------------
   Hardware-neutral status
   --------------------------------------------------------------------------- */
typedef struct _I2C_HW_STATUS {
    ULONG   RawIntr;        /* raw interrupt bits (hardware) */
    ULONG   StatusReg;      /* raw status register snapshot */

    BOOLEAN TxFifoNotFull;
    BOOLEAN TxFifoNotEmpty;
    BOOLEAN RxFifoNotEmpty;
    BOOLEAN StopDetected;
    BOOLEAN TxAborted;
    BOOLEAN ControllerActive; /* bus/controller activity flag */

    ULONG   TxFifoLevel;
    ULONG   RxFifoLevel;

    /* Universal abort/NACK context */
    ULONG   AbortBits;       /* backend-specific abort bitmask */
    BOOLEAN ArbLost;         /* arbitration lost */
    BOOLEAN AddressNack;     /* address NACK */
    BOOLEAN DataNack;        /* data NACK */

    /* Interrupt context */
    ULONG   PendingIntrBits; /* snapshot of pending interrupts */
    ULONG   StopBits;        /* mask for STOP_DET ack if needed */

    /* Optional timing counts (backend may populate) */
    ULONG SsSclHighCnt;      /* Standard/Fast mode SCL high period count */
    ULONG SsSclLowCnt;       /* Standard/Fast mode SCL low period count */
} I2C_HW_STATUS, *PI2C_HW_STATUS;


#endif /* I2CCTRL_HAL_H */
