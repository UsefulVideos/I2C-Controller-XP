#include <ntddk.h>
#include "i2cctrl_wdm_compat.h"
#include "i2cctrl_backport.h"

/* ---------------- Object shells ---------------- */

struct _I2CCTRL_WDM_DEVICE {
    PDEVICE_OBJECT Fdo;
    PDEVICE_OBJECT Pdo;
};

struct _I2CCTRL_WDM_TIMER {
    KTIMER Timer;
    KDPC   Dpc;
    I2CCTRL_WDM_TIMER_CB Callback;
    PVOID Context;
};

struct _I2CCTRL_WDM_WORKITEM {
    PIO_WORKITEM WorkItem;
    I2CCTRL_WDM_WORK_CB Callback;
    PVOID Context;
    PDEVICE_OBJECT DeviceObject;
};

struct _I2CCTRL_WDM_QUEUE {
    LIST_ENTRY IrpList;
    PKSPIN_LOCK Lock;
    I2CCTRL_WDM_QUEUE_DISPATCH Dispatch;
    BOOLEAN Started;
};

/* ---------------- Device ---------------- */

NTSTATUS
I2CCTRL_WdmDeviceCreate(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PDEVICE_OBJECT  Pdo,
    OUT PI2CCTRL_WDM_DEVICE *DeviceOut
    )
{
    NTSTATUS status;
    PI2CCTRL_WDM_DEVICE dev;
    UNICODE_STRING name;
    PDEVICE_OBJECT fdo;

    if (DriverObject == NULL || DeviceOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlInitUnicodeString(&name, L"\\Device\\I2CCTRL_WDM");
    status = IoCreateDevice(
        DriverObject,
        sizeof(*dev),
        &name,
        FILE_DEVICE_UNKNOWN,
        0U,
        FALSE,
        &fdo
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    dev = (PI2CCTRL_WDM_DEVICE)ExAllocatePoolWithTag(NonPagedPool, sizeof(*dev), 'dCI2');
    if (dev == NULL) {
        IoDeleteDevice(fdo);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    dev->Fdo = fdo;
    dev->Pdo = Pdo;

    if (Pdo != NULL) {
        (void)IoAttachDeviceToDeviceStack(fdo, Pdo);
    }

    *DeviceOut = dev;
    return STATUS_SUCCESS;
}

/* ---------------- Spin lock ---------------- */

NTSTATUS
I2CCTRL_WdmSpinLockCreate(OUT PKSPIN_LOCK *LockOut)
{
    PKSPIN_LOCK lock;
    if (LockOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    lock = (PKSPIN_LOCK)ExAllocatePoolWithTag(NonPagedPool, sizeof(KSPIN_LOCK), 'lCI2');
    if (lock == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    I2CCTRL_INIT_LOCK(lock);
    *LockOut = lock;
    return STATUS_SUCCESS;
}

KIRQL
I2CCTRL_WdmSpinLockAcquire(IN PKSPIN_LOCK Lock)
{
    KIRQL oldIrql;
    if (Lock == NULL) {
        return PASSIVE_LEVEL;
    }
    KeAcquireSpinLock(Lock, &oldIrql);
    return oldIrql;
}

VOID
I2CCTRL_WdmSpinLockRelease(IN PKSPIN_LOCK Lock, IN KIRQL OldIrql)
{
    if (Lock == NULL) {
        return;
    }
    KeReleaseSpinLock(Lock, OldIrql);
}

/* ---------------- Timer ---------------- */

static VOID
I2CCTRL_WdmTimerDpc(IN PKDPC Dpc, IN PVOID Context, IN PVOID Arg1, IN PVOID Arg2)
{
    PI2CCTRL_WDM_TIMER t;
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    t = (PI2CCTRL_WDM_TIMER)Context;
    if (t != NULL && t->Callback != NULL) {
        t->Callback(t->Context);
    }
}

NTSTATUS
I2CCTRL_WdmTimerCreate(OUT PI2CCTRL_WDM_TIMER *TimerOut,
                       IN  I2CCTRL_WDM_TIMER_CB Callback,
                       IN  PVOID Context)
{
    PI2CCTRL_WDM_TIMER t;
    if (TimerOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    t = (PI2CCTRL_WDM_TIMER)ExAllocatePoolWithTag(NonPagedPool, sizeof(*t), 'tCI2');
    if (t == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeTimer(&t->Timer);
    KeInitializeDpc(&t->Dpc, I2CCTRL_WdmTimerDpc, (PVOID)t);
    t->Callback = Callback;
    t->Context  = Context;
    *TimerOut = t;
    return STATUS_SUCCESS;
}

BOOLEAN
I2CCTRL_WdmTimerStart(IN PI2CCTRL_WDM_TIMER Timer,
                      IN LARGE_INTEGER DueTime,
                      IN ULONG PeriodMs)
{
    if (Timer == NULL) {
        return FALSE;
    }
    return KeSetTimerEx(&Timer->Timer, DueTime, PeriodMs, &Timer->Dpc);
}

BOOLEAN
I2CCTRL_WdmTimerStop(IN PI2CCTRL_WDM_TIMER Timer)
{
    if (Timer == NULL) {
        return FALSE;
    }
    return KeCancelTimer(&Timer->Timer);
}

/* ---------------- Work item (IoQueueWorkItem) ---------------- */

static VOID
I2CCTRL_WdmWorkThunk(IN PDEVICE_OBJECT DeviceObject, IN PVOID Context)
{
    PI2CCTRL_WDM_WORKITEM w;
    UNREFERENCED_PARAMETER(DeviceObject);

    w = (PI2CCTRL_WDM_WORKITEM)Context;
    if (w != NULL && w->Callback != NULL) {
        w->Callback(w->Context);
    }

    if (w != NULL) {
        if (w->WorkItem != NULL) {
            IoFreeWorkItem(w->WorkItem);
        }
        ExFreePool(w);
    }
}

NTSTATUS
I2CCTRL_WdmWorkItemCreate(
    OUT PI2CCTRL_WDM_WORKITEM *WorkOut,
    IN  PDEVICE_OBJECT DeviceObject,
    IN  I2CCTRL_WDM_WORK_CB Callback,
    IN  PVOID Context
    )
{
    PI2CCTRL_WDM_WORKITEM w;
    PIO_WORKITEM wi;
    KIRQL irql;

    /* Defensive checks */
    if (WorkOut == NULL || DeviceObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* IoAllocateWorkItem must be called at PASSIVE_LEVEL */
    irql = KeGetCurrentIrql();
    if (irql != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    /* Allocate wrapper structure from nonpaged pool */
    w = (PI2CCTRL_WDM_WORKITEM)ExAllocatePoolWithTag(NonPagedPool,
                                                     sizeof(I2CCTRL_WDM_WORKITEM),
                                                     'wCI2');
    if (w == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Allocate the actual I/O work item */
    wi = IoAllocateWorkItem(DeviceObject);
    if (wi == NULL) {
        ExFreePool(w);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Initialize fields */
    w->WorkItem     = wi;
    w->Callback     = Callback;
    w->Context      = Context;
    w->DeviceObject = DeviceObject;

    *WorkOut = w;
    return STATUS_SUCCESS;
}

BOOLEAN
I2CCTRL_WdmWorkItemEnqueue(IN PI2CCTRL_WDM_WORKITEM Work)
{
    KIRQL irql;

    /* Defensive checks */
    if (Work == NULL) {
        return FALSE;
    }
    if (Work->WorkItem == NULL || Work->DeviceObject == NULL) {
        return FALSE;
    }

    /* IoQueueWorkItem must be called at PASSIVE_LEVEL */
    irql = KeGetCurrentIrql();
    if (irql != PASSIVE_LEVEL) {
        return FALSE;
    }

    IoQueueWorkItem(Work->WorkItem,
                    I2CCTRL_WdmWorkThunk,
                    DelayedWorkQueue,
                    (PVOID)Work);

    return TRUE;
}


/* ---------------- Memory ---------------- */

PVOID
I2CCTRL_WdmMemoryAlloc(IN POOL_TYPE Pool, IN SIZE_T Bytes, IN ULONG Tag)
{
    if (Bytes == 0U) {
        return NULL;
    }
    if (KeGetCurrentIrql() == DISPATCH_LEVEL &&
        (Pool == PagedPool || Pool == PagedPoolCacheAligned)) {
        return NULL;
    }
    return ExAllocatePoolWithTag(Pool, Bytes, Tag);
}

VOID
I2CCTRL_WdmMemoryFree(IN PVOID Ptr)
{
    if (Ptr != NULL) {
        ExFreePool(Ptr);
    }
}

/* ---------------- Queue (cancel-safe skeleton) ---------------- */

NTSTATUS
I2CCTRL_WdmQueueCreate(OUT PI2CCTRL_WDM_QUEUE *QueueOut,
                       IN  I2CCTRL_WDM_QUEUE_DISPATCH Dispatch)
{
    PI2CCTRL_WDM_QUEUE q;
    NTSTATUS status;
    PKSPIN_LOCK lock;

    if (QueueOut == NULL || Dispatch == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = I2CCTRL_WdmSpinLockCreate(&lock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    q = (PI2CCTRL_WDM_QUEUE)ExAllocatePoolWithTag(NonPagedPool, sizeof(*q), 'qCI2');
    if (q == NULL) {
        ExFreePool(lock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    InitializeListHead(&q->IrpList);
    q->Lock = lock;
    q->Dispatch = Dispatch;
    q->Started = FALSE;

    *QueueOut = q;
    return STATUS_SUCCESS;
}

VOID
I2CCTRL_WdmQueueStart(IN PI2CCTRL_WDM_QUEUE Queue)
{
    KIRQL old;
    if (Queue == NULL) {
        return;
    }
    old = I2CCTRL_WdmSpinLockAcquire(Queue->Lock);
    Queue->Started = TRUE;
    I2CCTRL_WdmSpinLockRelease(Queue->Lock, old);
}

VOID
I2CCTRL_WdmQueueStop(IN PI2CCTRL_WDM_QUEUE Queue)
{
    KIRQL old;
    if (Queue == NULL) {
        return;
    }
    old = I2CCTRL_WdmSpinLockAcquire(Queue->Lock);
    Queue->Started = FALSE;
    I2CCTRL_WdmSpinLockRelease(Queue->Lock, old);
}

VOID
I2CCTRL_WdmQueueDrain(IN PI2CCTRL_WDM_QUEUE Queue)
{
    PLIST_ENTRY e;
    PLIST_ENTRY head;
    KIRQL old;

    if (Queue == NULL) {
        return;
    }

    old = I2CCTRL_WdmSpinLockAcquire(Queue->Lock);
    head = &Queue->IrpList;
    e = head->Flink;

    while (e != head) {
        PIRP irp;
        irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        e = e->Flink;
        RemoveEntryList(&irp->Tail.Overlay.ListEntry);

        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0UL;

        I2CCTRL_WdmSpinLockRelease(Queue->Lock, old);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        old = I2CCTRL_WdmSpinLockAcquire(Queue->Lock);
    }

    I2CCTRL_WdmSpinLockRelease(Queue->Lock, old);
}

NTSTATUS
I2CCTRL_WdmQueueSubmit(IN PI2CCTRL_WDM_QUEUE Queue, IN PIRP Irp)
{
    KIRQL old;

    if (Queue == NULL || Irp == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    old = I2CCTRL_WdmSpinLockAcquire(Queue->Lock);

    if (!Queue->Started) {
        InsertTailList(&Queue->IrpList, &Irp->Tail.Overlay.ListEntry);
        I2CCTRL_WdmSpinLockRelease(Queue->Lock, old);
        return STATUS_PENDING;
    }

    I2CCTRL_WdmSpinLockRelease(Queue->Lock, old);
    return Queue->Dispatch(Queue, Irp);
}

/* ---------------- Interrupt ---------------- */

NTSTATUS
I2CCTRL_WdmInterruptCreate(
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
    )
{
    if (InterruptOut == NULL || ServiceRoutine == NULL || SpinLock == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    return IoConnectInterrupt(
        InterruptOut,
        ServiceRoutine,
        ServiceContext,
        SpinLock,
        Vector,
        Irql,
        SyncIrql,
        Mode,
        Share,
        Affinity,
        FALSE
    );
}

/* ---------------- Helpers ---------------- */

PDEVICE_OBJECT
I2CCTRL_WdmGetDeviceObject(IN PI2CCTRL_WDM_DEVICE Device)
{
    if (Device == NULL) {
        return NULL;
    }
    return Device->Fdo;
}


/* -----------------------------------------------------------------------
 * Backport: WdfRequestComplete -> I2CCTRL_WdmRequestComplete
 * ----------------------------------------------------------------------- */
VOID
I2CCTRL_WdmRequestComplete(
    IN PIRP Irp,
    IN NTSTATUS Status
    )
{
    if (Irp == NULL) {
        return;
    }
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0UL;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/* -----------------------------------------------------------------------
 * Backport: KeDelayExecutionThread -> I2CCTRL_WdmDelayExecution
 * ----------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_WdmDelayExecution(
    IN KPROCESSOR_MODE WaitMode,
    IN BOOLEAN Alertable,
    IN PLARGE_INTEGER Interval
    )
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    return KeDelayExecutionThread(WaitMode, Alertable, Interval);
}

/* -----------------------------------------------------------------------
 * Backport: Event helpers
 * ----------------------------------------------------------------------- */
VOID
I2CCTRL_WdmEventCreate(OUT PKEVENT Event, IN EVENT_TYPE Type, IN BOOLEAN State)
{
    KeInitializeEvent(Event, Type, State);
}

VOID
I2CCTRL_WdmEventSet(IN PKEVENT Event)
{
    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
}

VOID
I2CCTRL_WdmEventReset(IN PKEVENT Event)
{
    KeResetEvent(Event);
}