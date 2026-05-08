/* i2cctrl_bsod.c
 * Advanced BSOD workaround helpers with WinDBG-friendly diagnostics
 * Target: Windows XP x86/x64, C89 compliant
 */

#include <ntddk.h>
#include "i2cctrl_bsod.h"
#include "i2cctrl_ext.h"
#include "i2cctrl_etw.h"
#include "i2cctrl_etw.tmh"
#include "i2cctrl_hw.h"

/* ---------------------------------------------------------------------------
   Safe IRP completion (defends against NULL and standardizes status/info)
   --------------------------------------------------------------------------- */
VOID
I2cCtrl_SafeCompleteIrp(
    PIRP Irp,
    NTSTATUS Status
    )
{
    KIRQL irql;

    /* XP-safe: IoCompleteRequest is safe at DISPATCH_LEVEL, but guard NULL */
    if (Irp) {
        irql = KeGetCurrentIrql();
        Irp->IoStatus.Status = Status;
        Irp->IoStatus.Information = 0;
        /* Avoid APC-level issues: never block here */
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        if (irql > DISPATCH_LEVEL) {
            KdPrint(("I2CCTRL: SafeCompleteIrp: completed at IRQL %lu\n", (ULONG)irql));
        }
    }
    else {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL, "SafeCompleteIrp: NULL IRP", 0, 0);
        KdPrint(("I2CCTRL: SafeCompleteIrp: NULL IRP\n"));
    }
}

/* ---------------------------------------------------------------------------
   Spinlock wrappers (trace IRQL constraints)
   --------------------------------------------------------------------------- */
VOID
I2cCtrl_AcquireIoLock(
    PI2CCTRL_FDO Dx,
    PKIRQL OldIrql
    )
{
    I2CCTRL_REQUIRE_PTR(Dx);
    I2CCTRL_REQUIRE_PTR(OldIrql);

    /* XP-safe: must be <= DISPATCH_LEVEL when acquiring spinlocks */
    I2CCTRL_REQUIRE_DISPATCH_OR_LOWER();

    KeAcquireSpinLock(&Dx->IoLock, OldIrql);
}

VOID
I2cCtrl_ReleaseIoLock(
    PI2CCTRL_FDO Dx,
    KIRQL OldIrql
    )
{
    I2CCTRL_REQUIRE_PTR(Dx);
    KeReleaseSpinLock(&Dx->IoLock, OldIrql);
}

/* ---------------------------------------------------------------------------
   Unicode free (defend against double free and stale fields)
   --------------------------------------------------------------------------- */
VOID
I2cCtrl_SafeFreeUnicode(
    PUNICODE_STRING Str
    )
{
    /* XP-safe: can run at PASSIVE_LEVEL only (RtlFreeUnicodeString is pageable) */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, 1, "I2CCTRL[%lx]: " "SafeFreeUnicode: wrong IRQL %lu" "\n", (0x00000200), (ULONG)KeGetCurrentIrql());
    }

    if (Str && Str->Buffer) {
        RtlFreeUnicodeString(Str);
        Str->Buffer = NULL;
        Str->Length = 0;
        Str->MaximumLength = 0;
    } else {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, 2, "I2CCTRL[%lx]: " "SafeFreeUnicode: skip (NULL or already freed)" "\n", (0x00000200), 0);
    }
}

/* ---------------------------------------------------------------------------
   Safe Unicode duplicate (pool-tagged, NPP; returns NULL on failure)
   --------------------------------------------------------------------------- */
PUNICODE_STRING
I2cCtrl_SafeDuplicateUnicode(
    PCUNICODE_STRING Src,
    ULONG Tag
    )
{
    PUNICODE_STRING dup;

    if (!Src || !Src->Buffer || Src->Length == 0) {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_SNAPSHOT,
                    "SafeDuplicateUnicode: skip (NULL/empty)", 0, 0);
        return NULL;
    }

    /* XP-safe: allocate in NonPagedPool to avoid page faults later */
    dup = (PUNICODE_STRING)I2cCtrl_Alloc(NonPagedPool, sizeof(UNICODE_STRING), Tag);
    if (!dup) return NULL;

    dup->Length = Src->Length;
    dup->MaximumLength = Src->Length + sizeof(WCHAR);
    dup->Buffer = (PWCHAR)I2cCtrl_Alloc(NonPagedPool, dup->MaximumLength, Tag);
    if (!dup->Buffer) {
        I2cCtrl_Free(dup, Tag);
        return NULL;
    }

    RtlCopyMemory(dup->Buffer, Src->Buffer, Src->Length);
    dup->Buffer[dup->Length / sizeof(WCHAR)] = L'\0';
    return dup;
}

/* ---------------------------------------------------------------------------
   Safe child PDO deletion stub (ACPI‑safe, XP/2003‑safe)
   - DO NOT delete the PDO here.
   - DO NOT free ACPI‑visible strings.
   - DO NOT close ACPI handles.
   - Only mark the PDO as Removed; real deletion happens in IRP_MN_REMOVE_DEVICE.
   --------------------------------------------------------------------------- */
VOID
I2cCtrl_SafeDeleteChild(
    PDEVICE_OBJECT Pdo
    )
{
    PI2CCTRL_PDO pdoExt;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_SNAPSHOT,
                    "SafeDeleteChild: wrong IRQL %lu",
                    (ULONG)KeGetCurrentIrql());
    }

    if (Pdo == NULL) {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_SNAPSHOT,
                    "SafeDeleteChild: NULL PDO");
        return;
    }

    pdoExt = (PI2CCTRL_PDO)Pdo->DeviceExtension;
    if (pdoExt == NULL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_SNAPSHOT,
                    "SafeDeleteChild: no extension for PDO %p", Pdo);
        return;
    }

    //
    // ACPI‑safe behavior:
    // Mark the PDO as removed, but DO NOT delete it.
    // ACPI may still reference the namespace node for 30-120 seconds.
    //
    pdoExt->Removed = TRUE;

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_SNAPSHOT,
                "SafeDeleteChild: marked PDO %p as removed (no delete)", Pdo);
}

/* ---------------------------------------------------------------------------
   Tagged allocation helpers (WinDBG pool tracking friendly)
   --------------------------------------------------------------------------- */
PVOID
I2cCtrl_Alloc(
    POOL_TYPE Type,
    SIZE_T Size,
    ULONG Tag
    )
{
    PVOID p;

    /* XP-safe sanity: NonPagedPool for IRQL > PASSIVE_LEVEL */
    if (KeGetCurrentIrql() > PASSIVE_LEVEL && Type != NonPagedPool) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_INIT,
                    "Alloc: forcing NonPagedPool at IRQL %lu", (ULONG)KeGetCurrentIrql());
        Type = NonPagedPool;
    }

    p = ExAllocatePoolWithTag(Type, Size, Tag);
    if (!p) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT,
                    "Alloc fail: size=%lu tag=%08lx", (ULONG)Size, Tag);
    } else {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_INIT,
                    "Alloc ok: ptr=%p size=%lu tag=%08lx", p, (ULONG)Size, Tag);
    }
    return p;
}

VOID
I2cCtrl_Free(
    PVOID Ptr,
    ULONG Tag
    )
{
    if (Ptr) {
        ExFreePoolWithTag(Ptr, Tag);
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_INIT,
                    "Free ok: ptr=%p tag=%08lx", Ptr, Tag);
    } else {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_INIT,
                    "Free NULL: tag=%08lx", Tag);
    }
}

/* ---------------------------------------------------------------------------
   Guarded user copy (from user → kernel), with probe + SEH
   --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_SafeCopyFromUser(
    PVOID KernelDest,
    PVOID UserSrc,
    SIZE_T Bytes
    )
{
    NTSTATUS st = STATUS_SUCCESS;
    KPROCESSOR_MODE mode;

    if (!KernelDest || !UserSrc || Bytes == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* XP-safe: only probe if previous mode is UserMode; else copy directly */
    mode = ExGetPreviousMode();

    __try {
        if (mode == UserMode) {
            ProbeForRead(UserSrc, Bytes, sizeof(UCHAR));
        }
        RtlCopyMemory(KernelDest, UserSrc, Bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = GetExceptionCode();
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                    "SafeCopyFromUser: exception 0x%08lx", st);
    }
    return st;
}

/* ---------------------------------------------------------------------------
   Guarded user copy (kernel → user), with probe + SEH
   --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_SafeCopyToUser(
    VOID* UserDest,
    const VOID* KernelSrc,
    SIZE_T Bytes
    )
{
    NTSTATUS st = STATUS_SUCCESS;
    KPROCESSOR_MODE mode;

    if (!UserDest || !KernelSrc || Bytes == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* XP-safe: only probe if previous mode is UserMode; else copy directly */
    mode = ExGetPreviousMode();

    __try {
        if (mode == UserMode) {
            ProbeForWrite(UserDest, Bytes, sizeof(UCHAR));
        }
        RtlCopyMemory(UserDest, KernelSrc, Bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = GetExceptionCode();
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                    "SafeCopyToUser: exception 0x%08lx", st);
    }
    return st;
}

/* ---------------------------------------------------------------------------
   Safe MDL probe+lock for user buffers (read/write), with cleanup on failure
   --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_SafeProbeAndLockMdl(
    PMDL* OutMdl,
    PVOID UserBuffer,
    SIZE_T Length,
    BOOLEAN ForWrite
    )
{
    PMDL mdl;
    KPROCESSOR_MODE mode;

    if (!OutMdl || !UserBuffer || Length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* XP-safe: this must run at PASSIVE_LEVEL */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                    "SafeProbeAndLockMdl: wrong IRQL %lu", (ULONG)KeGetCurrentIrql());
        return STATUS_INVALID_DEVICE_STATE;
    }

    *OutMdl = NULL;

    mdl = IoAllocateMdl(UserBuffer, (ULONG)Length, FALSE, FALSE, NULL);
    if (!mdl) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    mode = ExGetPreviousMode();

    __try {
        MmProbeAndLockPages(mdl,
                            (mode == UserMode) ? UserMode : KernelMode,
                            ForWrite ? IoWriteAccess : IoReadAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        NTSTATUS code = GetExceptionCode();
        IoFreeMdl(mdl);
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                    "SafeProbeAndLockMdl: exception 0x%08lx", code);
        return code;
    }

    *OutMdl = mdl;
    return STATUS_SUCCESS;
}

VOID
I2cCtrl_SafeUnlockMdl(
    PMDL Mdl
    )
{
    /* XP-safe: MDL ops are safe at PASSIVE/DISPATCH; guard NULL */
    if (Mdl) {
        __try {
            MmUnlockPages(Mdl);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            NTSTATUS code = GetExceptionCode();
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_IOCTL,
                        "SafeUnlockMdl: exception 0x%08lx", code);
        }
        IoFreeMdl(Mdl);
    }
}

/* ---------------------------------------------------------------------------
   Safe ref/deref helpers (thin guards with tracing)
   --------------------------------------------------------------------------- */
VOID
I2cCtrl_SafeDeref(
    PVOID Object,
    PVOID (*DecRef)(PVOID)
    )
{
    if (Object && DecRef) {
        /* XP-safe: avoid exceptions from custom deref */
        __try {
            (VOID)DecRef(Object);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            NTSTATUS code = GetExceptionCode();
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_INIT,
                        "SafeDeref: exception 0x%08lx", code);
        }
    } else {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_INIT,
                    "SafeDeref: skip (NULL)", 0);
    }
}

BOOLEAN
I2cCtrl_SafeRefDevice(
    PDEVICE_OBJECT DeviceObject
    )
{
    if (DeviceObject) {
        /* ObReferenceObject is nonpageable; safe at any IRQL */
        ObReferenceObject(DeviceObject);
        return TRUE;
    }
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_SNAPSHOT,
                "SafeRefDevice: NULL DeviceObject", 0, 0);
    return FALSE;
}

VOID
I2cCtrl_SafeDerefDevice(
    PDEVICE_OBJECT DeviceObject
    )
{
    if (DeviceObject) {
        ObDereferenceObject(DeviceObject);
    } else {
        TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_SNAPSHOT,
                    "SafeDerefDevice: NULL DeviceObject", 0, 0);
    }
}

/* ---------------------------------------------------------------------------
   SafeControllerReset - XP/2003 BSOD-safe, C89-compliant
   Purpose:
     - Defensive stop/start of controller with trace and failure flagging
     - Quiesce hardware, stall briefly, re-enable
     - Verify bus idle via STOP_DET and clear interrupts
     - Flag hardware failure if MMIO guard tripped
   --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_SafeControllerReset(
    PI2CCTRL_FDO fdoExt,
    ULONG        Tag
    )
{
    NTSTATUS       st;
    I2C_HW_STATUS  hwst;

    /* C89 init */
    st = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (fdoExt == NULL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ControllerReset called with NULL context");
        return STATUS_INVALID_PARAMETER;
    }

    /* XP-safe: allow at DISPATCH or lower, never at HIGH_LEVEL */
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ControllerReset bad IRQL %lu", (ULONG)KeGetCurrentIrql());
        return STATUS_INVALID_DEVICE_STATE;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_BUS,
                "ControllerReset begin");

    /* Best-effort quiesce: disable, short stall (hard-capped), re-enable */
    if (fdoExt->Ops != NULL && fdoExt->Ops->Enable != NULL) {
        (VOID)fdoExt->Ops->Enable(fdoExt, FALSE);  // disable
        KeStallExecutionProcessor(10); /* conservative 10µs stall */
        (VOID)fdoExt->Ops->Enable(fdoExt, TRUE);
    }

    /* Optional marker allocation to aid postmortem correlation */
    {
        PVOID marker = I2cCtrl_Alloc(NonPagedPool, 16U, Tag);
        if (marker != NULL) {
            RtlZeroMemory(marker, 16U);
            I2cCtrl_Free(marker, Tag);
        }
    }

    /* Verify bus idle via STOP_DET; clear if set */
    if (fdoExt->Ops != NULL && fdoExt->Ops->GetStatus != NULL) {
        st = fdoExt->Ops->GetStatus(fdoExt, &hwst);
        if (NT_SUCCESS(st) && hwst.StopDetected) {
            if (fdoExt->Ops->AckInterrupts != NULL) {
                fdoExt->Ops->AckInterrupts(fdoExt, hwst.RawIntr);
            }
        } else {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_BUS,
                        "ControllerReset STOP not detected; continuing");
        }
    }

    /* Flag hardware failure if MMIO guard tripped in safe accessors */
    if (fdoExt->HardwareFailure) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS,
                    "ControllerReset detected HardwareFailure");
        st = STATUS_DEVICE_HARDWARE_ERROR;
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_BUS,
                "ControllerReset end (st=0x%08lx)", st);
    return st;
}
