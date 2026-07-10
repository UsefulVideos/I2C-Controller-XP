/* i2cctrl_i2c.h
 *
 * XP/2003-safe I²C hardware layer using I2CCTRL_FDO (no duplicate context).
 * - MMIO map/probe with <4GB physical check
 * - ACPI power-on (_PS0) helper
 * - Timing setup (conservative defaults + calculator)
 * - Polled/blocking transfers (write/read/write-read)
 * - ISR/DPC prototypes (explicit interrupt acknowledge model)
 * - Interrupt connect/disconnect helpers (INTx, level-sensitive)
 */

#pragma once

#include <ntddk.h>
#include <acpiioct.h>
#include "i2cctrl_spinlock_fix.h"
#include "i2cctrl_hw.h"
#include "i2cctrl_hal_ops.h"
#include "i2cctrl_log.h"

/* Forward declaration to allow using PI2CCTRL_FDO in prototypes
   without requiring the entire FDO definition here. */
typedef struct _I2CCTRL_FDO I2CCTRL_FDO, *PI2CCTRL_FDO;

/* Tunables (bounded waits) */
#define I2C_POLL_SPINS_MAX  20000  /* ~100ms with 5us stalls */
#define I2C_STALL_US        5

/* ACPI power-on helper (_PS0) */
NTSTATUS I2cCtrl_PowerOnTouchpad(PDEVICE_OBJECT Pdo);

/* ACPI power-off helper (_PS3) */
NTSTATUS I2cCtrl_PowerOffTouchpad(PDEVICE_OBJECT Pdo);

/* Timing calculator (optional): compute HCNT/LCNT from clock and target tHIGH/tLOW us */
VOID I2cHwComputeTimings(ULONG FclkHz, I2C_SPEED_MODE Speed, ULONG* Hcnt, ULONG* Lcnt);

/* MMIO mapping + init (conservative timing if calculator not used) */
NTSTATUS I2cHwMapAndInit(PI2CCTRL_FDO fdoExt,
                         PHYSICAL_ADDRESS BarPa,
                         ULONG BarLength,
                         I2C_SPEED_MODE Speed);

/* De-init + unmap */
VOID I2cHwUnmapAndDeinit(PI2CCTRL_FDO fdoExt);

/* Enable/disable core */
NTSTATUS I2cHwEnable(PI2CCTRL_FDO fdoExt, BOOLEAN Enable);

/* Configure speed (disables/enables core around timing/control writes) */
NTSTATUS I2cHwSetSpeed(PI2CCTRL_FDO fdoExt, I2C_SPEED_MODE Speed);

/* Polled/blocking transfers */
NTSTATUS I2cHwWrite(PI2CCTRL_FDO fdoExt, UCHAR Slave7, const UCHAR* Buf, ULONG Len, BOOLEAN SendStop);
NTSTATUS I2cHwRead(PI2CCTRL_FDO fdoExt, UCHAR Slave7, UCHAR* Buf, ULONG Len, BOOLEAN SendStop);
NTSTATUS I2cHwWriteReadRestart(PI2CCTRL_FDO fdoExt,
                               UCHAR Slave7,
                               const UCHAR* Wbuf, ULONG Wlen,
                               UCHAR* Rbuf, ULONG Rlen);

/* ISR/DPC prototypes (use explicit I2cCtrl_AckInterrupt to clear causes) */
VOID    I2cCtrl_DpcRoutine(KDPC* Dpc, PVOID DeferredContext, PVOID SystemArgument1, PVOID SystemArgument2);

/* Interrupt connect/disconnect helpers (INTx, level-sensitive) */
NTSTATUS I2cHwConnectInterrupt(PI2CCTRL_FDO fdoExt,
                               PDEVICE_OBJECT PhysicalDeviceObject,
                               KIRQL Irql,
                               ULONG Vector,
                               BOOLEAN ShareVector);
VOID     I2cHwDisconnectInterrupt(PI2CCTRL_FDO fdoExt);
