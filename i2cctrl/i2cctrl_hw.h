/* i2cctrl_hw.h
 * Pure generic I2C hardware abstraction for i2cctrl.sys
 * C89 compliant, XP/2003-safe
 */

#ifndef _I2CCTRL_HW_H_
#define _I2CCTRL_HW_H_

#include <ntddk.h>
#include "i2cctrl_ext.h"   /* brings in SMBUS_REQUEST definition */
#include "i2cctrl_queue.h" /* brings in PI2CCTRL_QUEUE definition */

#ifndef STATUS_DEVICE_HARDWARE_ERROR
#define STATUS_DEVICE_HARDWARE_ERROR ((NTSTATUS)0xC0000368L)
#endif

/* ---------------------------------------------------------------------------
 * Registry knobs (policy/quirks)
 * --------------------------------------------------------------------------- */
#define I2CCTRL_REG_SELFTEST_ENABLE      L"SelfTestEnable"
#define I2CCTRL_REG_SELFTEST_SLAVE       L"SelfTestSlave"
#define I2CCTRL_REG_SELFTEST_DELAYMS     L"SelfTestDelayMs"
#define I2CCTRL_REG_FORCE_CRASH          L"ForceCrashOnError"

/* Multi-master policy */
#define I2CCTRL_REG_MULTIMASTER_ENABLE   L"MultiMasterEnable"
#define I2CCTRL_REG_ARB_BACKOFF_BASEUS   L"ArbBackoffBaseUs"
#define I2CCTRL_REG_ARB_BACKOFF_MAXUS    L"ArbBackoffMaxUs"
#define I2CCTRL_REG_ARB_BACKOFF_JITTERUS L"ArbBackoffJitterUs"
#define I2CCTRL_REG_ARB_MAX_RETRIES      L"ArbMaxRetries"

/* High-speed policy */
#define I2CCTRL_REG_HS_ENABLE            L"HighSpeedEnable"
#define I2CCTRL_REG_HS_HCNT              L"HsSclHighCnt"
#define I2CCTRL_REG_HS_LCNT              L"HsSclLowCnt"
#define I2CCTRL_REG_HS_MASTER_CODE       L"HsMasterCode"

/* ---------------------------------------------------------------------------
 * Pool tags
 * --------------------------------------------------------------------------- */
#define I2CCTRL_TAG_EXT    'xtcI'
#define I2CCTRL_TAG_SELF   'tcsI'
#define I2CCTRL_TAG_DUMP   'pmdI'
#define I2CCTRL_TAG_CTX    'CtxI'
#define TAG_CHILD          'dPcI'

/* ---------------------------------------------------------------------------
 * SMBus flags
 * --------------------------------------------------------------------------- */
#define SMBUS_FLAG_READ     0x01U
#define SMBUS_FLAG_STOP     0x02U
#define SMBUS_FLAG_RESTART  0x04U

/* ---------------------------------------------------------------------------
 * XP/2003 backport helpers
 * --------------------------------------------------------------------------- */
#ifndef IRP_MN_QUERY_PNP_DEVICE_STATE
#define IRP_MN_QUERY_PNP_DEVICE_STATE 0xFF
#endif
#ifndef PNP_DEVICE_WAKE_ENABLED
#define PNP_DEVICE_WAKE_ENABLED 0x00000004UL
#endif

#ifndef I2CCTRL_REQUIRE_PASSIVE
#define I2CCTRL_REQUIRE_PASSIVE()                                      \
    do {                                                               \
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) {                     \
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,            \
                        "Expected PASSIVE_LEVEL, got IRQL=%lu",        \
                        (ULONG)KeGetCurrentIrql());                    \
            ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);               \
        }                                                              \
    } while (0)
#endif


/* ---------------------------------------------------------------------------
 * Controller quirks bitmask flags (functional quirks)
 * --------------------------------------------------------------------------- */
#define QUIRK_NONE                     0x00000000U
#define QUIRK_NEEDS_RESET_WORKAROUND   0x00000001U  /* bit 0: requires reset workaround */
#define QUIRK_BROKEN_CLOCK_GATE        0x00000002U  /* bit 1: broken clock gating logic */
#define QUIRK_NO_DMA_SUPPORT           0x00000004U  /* bit 2: DMA engine not supported */
#define QUIRK_ACPI20                   0x00000008U  /* bit 3: Controller requires ACPI 2.0+ handling */
#define QUIRK_ACPI10                   0x00000010U  /* bit 4: Force ACPI 1.0b fallback (S3-only wake) */
#define QUIRK_SLOW_CLOCK               0x00000020U  /* bit 5: slow clock workaround */
#define QUIRK_NO_D1D2                  0x00000040U  /* bit 6: device does not support D1/D2 */

/* ---------------------------------------------------------------------------
 * BSOD-tweak-workaround quirks bitmask flags (secondary category)
 * --------------------------------------------------------------------------- */
#define BSOD_NONE                      0x00000000U
#define BSOD_FORCE_PIO                 0x00010000U  /* bit 16: force PIO path to avoid DMA-related BSODs */
#define BSOD_MASK_INTERRUPTS           0x00020000U  /* bit 17: mask interrupts aggressively to avoid race conditions */
#define BSOD_EXTRA_RESET               0x00040000U  /* bit 18: perform extra reset sequence to avoid stuck controller BSOD */
#define BSOD_DELAY_INIT                0x00080000U  /* bit 19: add artificial delay during init to avoid timing BSOD */




/* ---------------------------------------------------------------------------
 * HID-over-I2C constants
 * --------------------------------------------------------------------------- */
#define HID_I2C_GET_REPORT_DESCRIPTOR   0x01U

/* ---------------------------------------------------------------------------
 * Universal interrupt identifiers (HAL-generic)
 * --------------------------------------------------------------------------- */
#define I2C_INT_RX_UNDER       0x0001U
#define I2C_INT_RX_OVER        0x0002U
#define I2C_INT_RX_FULL        0x0004U
#define I2C_INT_TX_OVER        0x0008U
#define I2C_INT_TX_EMPTY       0x0010U
#define I2C_INT_RD_REQ         0x0020U
#define I2C_INT_TX_ABORT       0x0040U
#define I2C_INT_RX_DONE        0x0080U
#define I2C_INT_ACTIVITY       0x0100U
#define I2C_INT_STOP_DETECTED  0x0200U
#define I2C_INT_START_DETECTED 0x0400U
#define I2C_INT_GEN_CALL       0x0800U

/* Synchronous transfer IOCTL that the bus driver services */
#ifndef IOCTL_I2CCTRL_TRANSFER
#define IOCTL_I2CCTRL_TRANSFER \
    CTL_CODE(FILE_DEVICE_I2CCTRL, 0x999, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* SMBus request flags */
#define SMBUS_FLAG_CONTROLLER_ENABLED 0x80000000U


/* Transfer directions */
#ifndef I2C_DIRECTION_READ
#define I2C_DIRECTION_READ    0
#endif

#ifndef I2C_DIRECTION_WRITE
#define I2C_DIRECTION_WRITE   1
#endif

NTSTATUS I2CctrlHw_SetTarget(PDEVICE_OBJECT DevObj, USHORT slaveAddr, ULONG flags);
NTSTATUS I2CctrlHw_Write(PDEVICE_OBJECT DevObj, const UCHAR* buf, ULONG len, ULONG* bytesDone, ULONG flags);
NTSTATUS I2CctrlHw_Read(PDEVICE_OBJECT DevObj, UCHAR* buf, ULONG len, ULONG* bytesDone, ULONG flags);
VOID     I2CctrlHw_Flush(PDEVICE_OBJECT DevObj);


/* ---------------------------------------------------------------------------
 * ADDITIONS: Generic controller register map and extension (do not remove above)
 * --------------------------------------------------------------------------- */

/* Generic controller register offsets (replace with silicon-specific values) */
#ifndef I2C_REG_CONTROL
#define I2C_REG_CONTROL         0x00U
#endif
#ifndef I2C_REG_STATUS
#define I2C_REG_STATUS          0x04U
#endif
#ifndef I2C_REG_TARGET
#define I2C_REG_TARGET          0x08U
#endif
#ifndef I2C_REG_TX
#define I2C_REG_TX              0x0CU
#endif
#ifndef I2C_REG_RX
#define I2C_REG_RX              0x10U
#endif
#ifndef I2C_REG_CMD
#define I2C_REG_CMD             0x14U
#endif
#ifndef I2C_REG_CLK
#define I2C_REG_CLK             0x18U
#endif

/* CONTROL bits */
#ifndef I2C_CTRL_ENABLE
#define I2C_CTRL_ENABLE         0x00000001U
#endif

/* STATUS bits */
#ifndef I2C_STAT_BUSY
#define I2C_STAT_BUSY           0x00000001U
#endif
#ifndef I2C_STAT_TX_EMPTY
#define I2C_STAT_TX_EMPTY       0x00000002U
#endif
#ifndef I2C_STAT_RX_FULL
#define I2C_STAT_RX_FULL        0x00000004U
#endif
#ifndef I2C_STAT_NACK
#define I2C_STAT_NACK           0x00000008U
#endif
#ifndef I2C_STAT_ARB_LOST
#define I2C_STAT_ARB_LOST       0x00000010U
#endif
#ifndef I2C_STAT_CLK_STRETCH
#define I2C_STAT_CLK_STRETCH    0x00000020U
#endif
#ifndef I2C_STAT_ERROR_MASK
#define I2C_STAT_ERROR_MASK     (I2C_STAT_NACK | I2C_STAT_ARB_LOST)
#endif

/* CMD bits */
#ifndef I2C_CMD_START
#define I2C_CMD_START           0x00000001U
#endif
#ifndef I2C_CMD_STOP
#define I2C_CMD_STOP            0x00000002U
#endif
#ifndef I2C_CMD_READ
#define I2C_CMD_READ            0x00000004U
#endif
#ifndef I2C_CMD_WRITE
#define I2C_CMD_WRITE           0x00000008U
#endif

/* Default timeouts (ms) */
#ifndef I2CCTRL_IDLE_TIMEOUT_MS
#define I2CCTRL_IDLE_TIMEOUT_MS 50U
#endif
#ifndef I2CCTRL_XFER_TIMEOUT_MS
#define I2CCTRL_XFER_TIMEOUT_MS 200U
#endif

#define I2C_STATUS_ACTIVITY     0x00000001U

/* HID/touch extension: retrieve PT_RAW_SAMPLE via QueryTouchSample */
#define I2CCTRL_OPCODE_GET_PT_SAMPLE   0x30U

/* Abstract register offsets (example values, adjust to your HW spec) */
#define I2CCTRL_REG_STATUS     0x00U
#define I2CCTRL_REG_ERROR      0x04U
#define I2CCTRL_REG_CLKCTRL    0x08U

/* Clear masks (write-to-clear bits) */
#define I2CCTRL_STATUS_CLEAR_MASK   0xFFFFFFFFU
#define I2CCTRL_ERROR_CLEAR_MASK    0xFFFFFFFFU

/* Clock control masks */
#define I2CCTRL_CLK_DISABLE_MASK    0x00000001U
#define I2CCTRL_CLK_ENABLE_MASK     0x00000000U


#endif /* _I2CCTRL_HW_H_ */
