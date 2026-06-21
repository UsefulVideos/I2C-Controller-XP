/* i2cctrl_hal_ops.h */

#ifndef I2CCTRL_HAL_OPS_H
#define I2CCTRL_HAL_OPS_H

#include <ntddk.h>           /* NTSTATUS, PCM_RESOURCE_LIST, PIRP */
#include "i2cctrl_hal_caps.h" /* brings in I2C_HW_CAPS definition */

/* ---------------------------------------------------------------------------
   Forward declarations
   --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_FDO I2CCTRL_FDO;
typedef I2CCTRL_FDO* PI2CCTRL_FDO;

typedef struct _I2C_HW_STATUS I2C_HW_STATUS;

struct _PT_RAW_SAMPLE;
typedef struct _PT_RAW_SAMPLE PT_RAW_SAMPLE;


/* Speed selection */
typedef enum _I2C_SPEED_MODE {
    /* Canonical names */
    I2C_SPEED_STANDARD = 0,   /* Standard-mode (100 kHz) */
    I2C_SPEED_FAST     = 1,   /* Fast-mode (400 kHz) */
    I2C_SPEED_HIGH     = 2,   /* High-speed mode (3.4 MHz) */

    /* Legacy/alternate names for compatibility */
    I2cSpeedStandard100k = I2C_SPEED_STANDARD,
    I2cSpeedFast400k     = I2C_SPEED_FAST,
    I2cSpeedHigh3_4M     = I2C_SPEED_HIGH
} I2C_SPEED_MODE;

typedef NTSTATUS
(*PFN_I2C_BLOCK_WRITE)(
    PI2CCTRL_FDO devctx,
    UCHAR        slaveAddr,
    ULONG        reg,
    PUCHAR       buf,
    ULONG        len,
    PULONG       bytesDone
);

typedef NTSTATUS
(*PFN_I2C_BLOCK_READ)(
    PI2CCTRL_FDO devctx,
    UCHAR        slaveAddr,
    ULONG        reg,
    PUCHAR       buf,
    ULONG        len,
    PULONG       bytesDone
);


/* ---------------------------------------------------------------------------
   HAL ops: the only functions upper layers may call
   --------------------------------------------------------------------------- */
typedef struct _I2C_HW_OPS {
    NTSTATUS (*MapResources)(PI2CCTRL_FDO ctx, PCM_RESOURCE_LIST translated);
    VOID     (*UnmapResources)(PI2CCTRL_FDO ctx);

    NTSTATUS (*Enable)(PI2CCTRL_FDO ctx, BOOLEAN on);
    NTSTATUS (*SetTarget7bit)(PI2CCTRL_FDO ctx, UCHAR addr7);

    NTSTATUS (*SetSpeed)(PI2CCTRL_FDO ctx, I2C_SPEED_MODE speed);
    NTSTATUS (*SetBusSpeedHz)(PI2CCTRL_FDO ctx, ULONG speedHz);

    NTSTATUS (*IssueWriteByte)(PI2CCTRL_FDO ctx, UCHAR byte);
    NTSTATUS (*IssueReadToken)(PI2CCTRL_FDO ctx);
    NTSTATUS (*ReadRxByte)(PI2CCTRL_FDO ctx, UCHAR* out);
    NTSTATUS (*ReadRxByteSafe)(PI2CCTRL_FDO ctx, UCHAR* out);

    NTSTATUS (*PrimeWrite)(PI2CCTRL_FDO ctx, const UCHAR* buf, ULONG len, ULONG* pushed);
    NTSTATUS (*PrimeReadTokens)(PI2CCTRL_FDO ctx, ULONG count, ULONG* queued);

    NTSTATUS (*GetStatus)(PI2CCTRL_FDO ctx, I2C_HW_STATUS* st);
    VOID     (*AckInterrupts)(PI2CCTRL_FDO ctx, ULONG hwBits);

    VOID     (*MaskInterrupts)(PI2CCTRL_FDO ctx, ULONG hwMask);
    VOID     (*UnmaskInterrupts)(PI2CCTRL_FDO ctx, ULONG hwMask);

    VOID     (*DrainRxBounded)(PI2CCTRL_FDO ctx);
    VOID     (*FlushTxBounded)(PI2CCTRL_FDO ctx);

    NTSTATUS (*EmitStopIfNeeded)(PI2CCTRL_FDO ctx);
    NTSTATUS (*EmitRestartIfNeeded)(PI2CCTRL_FDO ctx);

    ULONG    (*GetRawIntr)(PI2CCTRL_FDO ctx);
    BOOLEAN  (*IsArbitrationLost)(PI2CCTRL_FDO ctx, ULONG snapshot);
    NTSTATUS (*WriteTxByte)(PI2CCTRL_FDO ctx, UCHAR byte);

    /* Capabilities block */
    I2C_HW_CAPS Caps;

    /* FIFO management */
    NTSTATUS (*QuiesceFifos)(PI2CCTRL_FDO fdoExt);
    NTSTATUS (*DrainRxFifo)(PI2CCTRL_FDO fdoExt);
    NTSTATUS (*FlushTxFifo)(PI2CCTRL_FDO fdoExt);

    /* Optional raw register access */
    NTSTATUS (*ReadRegister32)(PI2CCTRL_FDO fdoExt, ULONG offset, ULONG* outValue);
    NTSTATUS (*WriteRegister32)(PI2CCTRL_FDO fdoExt, ULONG offset, ULONG value);

    /* Resource queries */
    NTSTATUS (*QueryLegacyBusInfo)(PI2CCTRL_FDO fdoExt, PVOID* busInfo);
    NTSTATUS (*FilterResourceRequirements)(PI2CCTRL_FDO fdoExt, PIRP Irp);
    NTSTATUS (*QueryResourceRequirements)(PI2CCTRL_FDO fdoExt, PIRP Irp);
    NTSTATUS (*QueryResources)(PI2CCTRL_FDO fdoExt, PIRP Irp);

    /* Controller helpers */
    NTSTATUS (*ConfigureController)(PI2CCTRL_FDO ctx,
                                    BOOLEAN masterMode,
                                    BOOLEAN restartEnable,
                                    BOOLEAN slaveDisable);

    NTSTATUS (*EmitReadRequest)(PI2CCTRL_FDO ctx);
	VOID     (*ReadTxDiscard)(PI2CCTRL_FDO);
    NTSTATUS (*QueryTouchSample)(PI2CCTRL_FDO ctx, PT_RAW_SAMPLE* outSample);

    VOID (*EnableWakeSource)(PI2CCTRL_FDO devctx, BOOLEAN enable);
    PFN_I2C_BLOCK_WRITE IssueBlockWrite;
    PFN_I2C_BLOCK_READ  IssueBlockRead;
} I2C_HW_OPS, *PI2C_HW_OPS;

/* Global backend ops (implemented in a .c file) */
extern I2C_HW_OPS  DwI2cOps;
extern I2C_HW_CAPS DwI2cCaps;
#endif /* I2CCTRL_HAL_OPS_H */
