/* i2cctrl_backport.h */
#ifndef I2CCTRL_BACKPORT_H
#define I2CCTRL_BACKPORT_H

#include <ntddk.h>

NTSTATUS I2CCTRL_RtlInitUnicodeStringCompat(OUT PUNICODE_STRING dst, IN PCWSTR src);
PVOID    I2CCTRL_AllocatePoolPriorityCompat(IN POOL_TYPE pool, IN SIZE_T bytes, IN ULONG tag, IN EX_POOL_PRIORITY pri);

VOID
I2CCTRL_KeInitializeTimerCompat(
    OUT PKTIMER Timer,
    IN  TIMER_TYPE Type
    );

BOOLEAN  I2CCTRL_KeSetPeriodicTimerCompat(IN PKTIMER timer, IN LARGE_INTEGER due, IN ULONG periodMs, IN PKDPC dpc);
NTSTATUS I2CCTRL_RtlUnicodeStringToIntegerCompat(IN PUNICODE_STRING str, IN ULONG base, OUT PULONG value);
NTSTATUS I2CCTRL_IoConnectInterruptCompat(OUT PKINTERRUPT *InterruptObject,
                                          IN PKSERVICE_ROUTINE ServiceRoutine,
                                          IN PVOID ServiceContext,
                                          IN PKSPIN_LOCK SpinLock,
                                          IN ULONG Vector,
                                          IN KIRQL Irql,
                                          IN KIRQL SynchronizeIrql,
                                          IN KINTERRUPT_MODE InterruptMode,
                                          IN BOOLEAN ShareVector,
                                          IN KAFFINITY ProcessorEnableMask,
                                          IN PDEVICE_OBJECT PhysicalDeviceObject);
NTSTATUS I2CCTRL_KeDelayExecutionCompat(IN KPROCESSOR_MODE WaitMode, IN BOOLEAN Alertable, IN PLARGE_INTEGER Interval);
BOOLEAN  I2CCTRL_TryCompleteIrpOnce(IN PIRP Irp, IN NTSTATUS Status);

/* -----------------------------------------------------------------------
 * BACKPORT_IO_CONNECT_INTERRUPT_LINE_BASED_PARAMETERS
 * Unified struct for XP and newer kits.
 * Contains both the XP-defined members and the newer Vista+ members.
 * ----------------------------------------------------------------------- */
typedef struct _BACKPORT_IO_CONNECT_INTERRUPT_LINE_BASED_PARAMETERS {
    /* XP/2003 fields */
    PDEVICE_OBJECT    PhysicalDeviceObject;   /* optional PDO, may be NULL */
    PKSERVICE_ROUTINE ServiceRoutine;         /* ISR entry point */
    PVOID             ServiceContext;         /* ISR context */
    PKSPIN_LOCK       SpinLock;               /* ISR spinlock */
    PKINTERRUPT      *InterruptObject;        /* receives interrupt object */

    /* Vista+ fields (not present in XP headers, but useful to store) */
    ULONG             Vector;                 /* interrupt vector */
    KIRQL             Irql;                   /* interrupt IRQL */
    KIRQL             SynchronizeIrql;        /* sync IRQL */
    KINTERRUPT_MODE   InterruptMode;          /* LevelSensitive or Latched */
    BOOLEAN           ShareVector;            /* TRUE if sharable */
    KAFFINITY         ProcessorEnableMask;    /* affinity mask */
} BACKPORT_IO_CONNECT_INTERRUPT_LINE_BASED_PARAMETERS,
  *PBACKPORT_IO_CONNECT_INTERRUPT_LINE_BASED_PARAMETERS;

/* -----------------------------------------------------------------------
 * BACKPORT_EX_POOL_PRIORITY
 * XP/2003-compatible definition of EX_POOL_PRIORITY values
 * Provides newer constants for pool allocation priority semantics.
 * ----------------------------------------------------------------------- */
typedef enum _BACKPORT_EX_POOL_PRIORITY {
    /* Low priority allocations */
    BACKPORT_LowPoolPriority                        = 0,
    BACKPORT_LowPoolPrioritySpecialPoolOverrun      = 8,
    BACKPORT_LowPoolPrioritySpecialPoolUnderrun     = 9,

    /* Normal priority allocations */
    BACKPORT_NormalPoolPriority                     = 16,
    BACKPORT_NormalPoolPrioritySpecialPoolOverrun   = 24,
    BACKPORT_NormalPoolPrioritySpecialPoolUnderrun  = 25,

    /* High priority allocations */
    BACKPORT_HighPoolPriority                       = 32,
    BACKPORT_HighPoolPrioritySpecialPoolOverrun     = 40,
    BACKPORT_HighPoolPrioritySpecialPoolUnderrun    = 41
} BACKPORT_EX_POOL_PRIORITY;

NTSTATUS
I2CCTRL_IoConnectInterruptCompat(
    OUT PKINTERRUPT       *InterruptObject,
    IN  PKSERVICE_ROUTINE  ServiceRoutine,
    IN  PVOID              ServiceContext,
    IN  PKSPIN_LOCK        SpinLock,
    IN  ULONG              Vector,
    IN  KIRQL              Irql,
    IN  KIRQL              SynchronizeIrql,
    IN  KINTERRUPT_MODE    InterruptMode,
    IN  BOOLEAN            ShareVector,
    IN  KAFFINITY          ProcessorEnableMask,
    IN  PDEVICE_OBJECT     PhysicalDeviceObject /* optional, may be NULL */
    );

#endif /* I2CCTRL_BACKPORT_H */
