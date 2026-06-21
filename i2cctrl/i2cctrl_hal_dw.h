#ifndef I2CCTRL_HAL_DW_H
#define I2CCTRL_HAL_DW_H

#include <ntddk.h>
#include "i2cctrl_hal_ops.h"
#include "i2cctrl_hal_caps.h"

/*
 * Synopsys DesignWare / Intel Serial IO I2C backend
 * Hardware register layout + bit definitions
 * (moved out of i2cctrl_hal_dw.c)
 */

/* Interrupt/status bits used by higher layers */
#define INTEL_STAT_TX_EMPTY_BIT    0x00000004U
#define INTEL_STAT_RX_FULL_BIT     0x00000008U
#define INTEL_STAT_RX_NOT_EMPTY    0x00000010U
#define INTEL_STAT_ARB_LOST_BIT    0x00000100U
#define INTEL_STAT_STOP_DET_BIT    0x00000020U
#define INTEL_STAT_TX_ABRT_BIT     0x00000040U

/* DW I2C register offsets */
#define INTEL_REG_CON              0x00U
#define INTEL_REG_TAR              0x04U
#define INTEL_REG_DATA_CMD         0x08U
#define INTEL_REG_SS_SCL_HCNT      0x0CU
#define INTEL_REG_INTR_STATUS      0x10U
#define INTEL_REG_INTR_MASK        0x14U
#define INTEL_REG_CLR_INTR         0x18U
#define INTEL_REG_CLR_TX_ABRT      0x1CU
#define INTEL_REG_CLR_STOP_DET     0x20U
#define INTEL_REG_TXFLR            0x24U
#define INTEL_REG_RXFLR            0x28U

/* Control register bits */
#define INTEL_CON_ENABLE_BIT       0x00000001U
#define INTEL_CON_MASTER_MODE      0x00000040U
#define INTEL_CON_RESTART_EN       0x00000020U

/*
 * Exported backend ops + capabilities
 */
extern I2C_HW_OPS  DwI2cOps;
extern I2C_HW_CAPS DwI2cCaps;

#endif /* I2CCTRL_HAL_DW_H */
