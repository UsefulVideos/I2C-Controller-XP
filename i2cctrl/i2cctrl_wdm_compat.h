#ifndef I2CCTRL_WDM_COMPAT_H
#define I2CCTRL_WDM_COMPAT_H

#include <ntddk.h>

/* Opaque handles to mimic KMDF object flavor */
typedef struct _I2cCtrl_WDM_DEVICE    I2CCTRL_WDM_DEVICE,    *PI2CCTRL_WDM_DEVICE;
typedef struct _I2cCtrl_WDM_QUEUE     I2CCTRL_WDM_QUEUE,     *PI2CCTRL_WDM_QUEUE;
typedef struct _I2cCtrl_WDM_TIMER     I2CCTRL_WDM_TIMER,     *PI2CCTRL_WDM_TIMER;
typedef struct _I2cCtrl_WDM_WORKITEM  I2CCTRL_WDM_WORKITEM,  *PI2CCTRL_WDM_WORKITEM;

/* ---------------- Device ---------------- */
NTSTATUS
I2CCTRL_WdmDeviceCreate(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PDEVICE_OBJECT  Pdo,
    OUT PI2CCTRL_WDM_DEVICE *DeviceOut
    );

/* ---------------- Spin lock ---------------- */
NTSTATUS I2CCTRL_WdmSpinLockCreate(OUT PKSPIN_LOCK *LockOut);
KIRQL    I2CCTRL_WdmSpinLockAcquire(IN PKSPIN_LOCK Lock);
VOID     I2CCTRL_WdmSpinLockRelease(IN PKSPIN_LOCK Lock, IN KIRQL OldIrql);

/* ---------------- Timer ---------------- */
typedef VOID (*I2CCTRL_WDM_TIMER_CB)(IN PVOID Context);

NTSTATUS I2CCTRL_WdmTimerCreate(OUT PI2CCTRL_WDM_TIMER *TimerOut,
                                IN  I2CCTRL_WDM_TIMER_CB Callback,
                                IN  PVOID Context);
BOOLEAN  I2CCTRL_WdmTimerStart(IN PI2CCTRL_WDM_TIMER Timer,
                               IN LARGE_INTEGER DueTime,
                               IN ULONG PeriodMs);
BOOLEAN  I2CCTRL_WdmTimerStop(IN PI2CCTRL_WDM_TIMER Timer);

/* ---------------- Work item (IoQueueWorkItem) ---------------- */
typedef VOID (*I2CCTRL_WDM_WORK_CB)(IN PVOID Context);

NTSTATUS I2CCTRL_WdmWorkItemCreate(OUT PI2CCTRL_WDM_WORKITEM *WorkOut,
                                   IN  PDEVICE_OBJECT DeviceObject,
                                   IN  I2CCTRL_WDM_WORK_CB Callback,
                                   IN  PVOID Context);
BOOLEAN  I2CCTRL_WdmWorkItemEnqueue(IN PI2CCTRL_WDM_WORKITEM Work);

/* ---------------- Memory ---------------- */
PVOID I2CCTRL_WdmMemoryAlloc(IN POOL_TYPE Pool, IN SIZE_T Bytes, IN ULONG Tag);
VOID  I2CCTRL_WdmMemoryFree(IN PVOID Ptr);

/* ---------------- Queue (cancel-safe IRP queue) ---------------- */
typedef NTSTATUS (*I2CCTRL_WDM_QUEUE_DISPATCH)(IN PI2CCTRL_WDM_QUEUE Queue, IN PIRP Irp);

NTSTATUS I2CCTRL_WdmQueueCreate(OUT PI2CCTRL_WDM_QUEUE *QueueOut,
                                IN  I2CCTRL_WDM_QUEUE_DISPATCH Dispatch);
VOID     I2CCTRL_WdmQueueStart(IN PI2CCTRL_WDM_QUEUE Queue);
VOID     I2CCTRL_WdmQueueStop(IN PI2CCTRL_WDM_QUEUE Queue);
VOID     I2CCTRL_WdmQueueDrain(IN PI2CCTRL_WDM_QUEUE Queue);
NTSTATUS I2CCTRL_WdmQueueSubmit(IN PI2CCTRL_WDM_QUEUE Queue, IN PIRP Irp);

/* ---------------- Interrupt ---------------- */
typedef VOID (*I2CCTRL_WDM_ISR)(IN PKINTERRUPT Interrupt, IN PVOID ServiceContext);

NTSTATUS I2CCTRL_WdmInterruptCreate(
    OUT PKINTERRUPT *InterruptOut,
    IN  PKSERVICE_ROUTINE ServiceRoutine,
    IN  PVOID ServiceContext,
    IN  PKSPIN_LOCK SpinLock,
    IN  ULONG Vector,
    IN  KIRQL Irql,
    IN  KIRQL SyncIrql,
    IN  KINTERRUPT_MODE Mode,
    IN  BOOLEAN Share,
    IN  KAFFINITY Affinity
    );

/* ---------------- Helpers ---------------- */
PDEVICE_OBJECT I2CCTRL_WdmGetDeviceObject(IN PI2CCTRL_WDM_DEVICE Device);

/* Provide local definitions if building with older headers */
#ifndef I2CCTRL_WDM_TIMER_TYPE_DEFINED
typedef enum _I2cCtrl_WDM_TIMER_TYPE {
    I2CCTRL_WdmNotificationTimer   = 0,
    I2CCTRL_WdmSynchronizationTimer = 1
} I2CCTRL_WDM_TIMER_TYPE;
#define I2CCTRL_WDM_TIMER_TYPE_DEFINED
#endif

/* -----------------------------------------------------------------------
 * I2CCTRL_WdmAddressProbeFlags
 * Flags controlling how address probing is performed.
 * ----------------------------------------------------------------------- */
typedef enum _I2cCtrl_WdmAddressProbeFlags {
    I2CCTRL_WdmProbeReadOnly     = 0x00, /* default: read probe only */
    I2CCTRL_WdmProbeWriteBack    = 0x01, /* write back same value after read */
    I2CCTRL_WdmProbeRangeEnds    = 0x02, /* also probe last byte of the range */
    I2CCTRL_WdmProbePerPage      = 0x04  /* probe one byte per page in range */
} I2CCTRL_WdmAddressProbeFlags;

/* Ensure PAGE_SIZE is defined for per-page probing */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096U
#endif

/* ================= Added XP-safe backport declarations (do not remove anything above) ============== */

/* Address validation helpers (XP-safe, no undocumented Mm* calls) */
BOOLEAN
I2CCTRL_WdmProbeAddressCompat(
    IN PVOID Address,
    IN SIZE_T Length,
    IN I2CCTRL_WdmAddressProbeFlags Flags
    );

BOOLEAN
I2CCTRL_WdmIsAddressValidCompat(
    IN PVOID Address
    );

/* Timer compatibility API (XP-safe wrappers) */
VOID
I2CCTRL_WdmTimerInitCompat(
    OUT PKTIMER Timer,
    IN I2CCTRL_WDM_TIMER_TYPE Type
    );

BOOLEAN
I2CCTRL_WdmTimerStartCompat(
    IN PKTIMER Timer,
    IN LARGE_INTEGER DueTime,
    IN ULONG Period,          /* milliseconds; 0 for one-shot */
    IN PKDPC DpcOptional      /* optional DPC; NULL allowed */
    );

BOOLEAN
I2CCTRL_WdmTimerCancelCompat(
    IN PKTIMER Timer
    );

BOOLEAN
I2CCTRL_WdmTimerIsSetCompat(
    IN PKTIMER Timer
    );

#endif /* I2CCTRL_WDM_COMPAT_H */
