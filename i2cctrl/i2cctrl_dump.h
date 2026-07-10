/* i2cctrl_dump.h
 * Diagnostic snapshot and crash/dump helpers for i2cctrl.sys
 * C89 compliant, XP-BSOD-safe and WinDDK-compiler-safe
 */

#ifndef _I2cCtrl_DUMP_H_
#define _I2cCtrl_DUMP_H_

#include <ntddk.h>
#include "i2cctrl_hw.h"

/* Optional SAL compatibility (no-ops on XP/older WDKs) */
#include "i2cctrl_comp_sal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bugcheck code and versioning (ULONG-sized constants, XP-safe) */
#define I2CCTRL_BUGCHECK_CODE      0xE2C1C2C3UL
#define I2CCTRL_DUMP_SNAPSHOT_VER  0x00010001UL

/* Forward declaration to avoid circular dependencies */
typedef struct _I2CCTRL_FDO I2CCTRL_FDO, *PI2CCTRL_FDO;

/* ---------- Snapshot structure (genericized) ----------
 * Plain integral types, no bitfields.
 * Field names avoid collision with register-offset macros.
 * All pointers stored as PVOID for WinDDK compatibility.
 */
typedef struct _I2cCtrl_DUMP_SNAPSHOT {
    ULONG         Version;        /* snapshot version */
    ULONG         Flags;          /* reserved for future use */
    LARGE_INTEGER SystemTime;     /* system time when captured */
    KIRQL         Irql;           /* IRQL at capture */

    PVOID         Dx;             /* FDO context pointer (opaque) */

    /* Abstract bus context */
    ULONG         BusAddress;     /* saved target address */
    ULONG         BusSpeedHz;     /* saved bus speed in Hz */
    ULONG         TimingHighNs;   /* abstract high period in ns */
    ULONG         TimingLowNs;    /* abstract low period in ns */
    ULONG         SpeedMode;      /* current speed mode enum */
    ULONG         CurrentState;   /* device power state */

    /* Diagnostic metadata */
    ULONG         LastReason;     /* reason code for snapshot */
} I2CCTRL_DUMP_SNAPSHOT, *PI2CCTRL_DUMP_SNAPSHOT;

/* ---------------------------------------------------------------------------
   Global snapshot pointer (defined in i2cctrl_dump.c, extern here)
   --------------------------------------------------------------------------- */
extern PI2CCTRL_DUMP_SNAPSHOT gSnap;

/* ---------------------------------------------------------------------------
   Public APIs (XP-safe, SAL expands to no-ops on older WDKs)
   --------------------------------------------------------------------------- */
VOID
I2cCtrl_DumpInit(
    PI2CCTRL_FDO dx
    );

VOID
I2cCtrl_AllocSnapshotBuffer(VOID);

VOID
I2cCtrl_ForceCrash(
    PI2CCTRL_FDO dx,
    NTSTATUS reason
    );

VOID
I2cCtrl_DumpOnTimeout(
    PI2CCTRL_FDO dx,
    NTSTATUS reason
    );

NTSTATUS
I2cCtrl_HandleDumpIoctl(
    PI2CCTRL_FDO dx,
    PIRP Irp,
    PIO_STACK_LOCATION isl
    );

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _I2cCtrl_DUMP_H_ */
