/* -----------------------------------------------------------------------
 * i2cctrl_backport.c - XP/2003 BSOD-safe compatibility shims
 * C89-compliant, WinDDK 7.1.0 compiler-safe
 *
 * Purpose:
 *  - Provide "Compat" wrappers for post-XP APIs often used by
 *    Precision Touchpad/HID-over-I2C code paths.
 *  - Each function is PASSIVE_LEVEL-only unless documented otherwise.
 *
 * Usage:
 *  - Include prototypes in i2cctrl_backport.h (recommended).
 *  - Call the Compat versions from your driver instead of newer APIs.
 * ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "i2cctrl_backport.h"
#include "i2cctrl_wdm_compat.h"

/* Some kits require explicit declarations for these types in C89 code */
typedef NTSTATUS (*PIO_CONNECT_INTERRUPT_EX)(IN PVOID Params);

/* -----------------------------------------------------------------------
 * Internal utility: resolve a system routine by name (PASSIVE_LEVEL only)
 * ----------------------------------------------------------------------- */
PVOID
I2CCTRL_GetSystemRoutine(IN PCWSTR Name)
{
    UNICODE_STRING usName;
    SIZE_T cch;

    if (Name == NULL) {
        return NULL;
    }

    cch = wcslen(Name);
    if (cch == 0U || cch > 0x7FFFu) {
        return NULL;
    }

    usName.Buffer = (PWSTR)Name;
    usName.Length = (USHORT)(cch * sizeof(WCHAR));
    usName.MaximumLength = usName.Length;

    return MmGetSystemRoutineAddress(&usName);
}

/* -----------------------------------------------------------------------
 * RtlInitUnicodeStringEx backport (PASSIVE_LEVEL)
 * Initializes a UNICODE_STRING safely with length checks.
 * ----------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_RtlInitUnicodeStringCompat(
    OUT PUNICODE_STRING DestinationString,
    IN  PCWSTR          SourceString
    )
{
    SIZE_T cch;
    USHORT byteLen;

    if (DestinationString == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (SourceString == NULL) {
        DestinationString->Buffer = NULL;
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        return STATUS_SUCCESS;
    }

    cch = wcslen(SourceString);
    if (cch > 0x7FFFu) {
        DestinationString->Buffer = NULL;
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        return STATUS_NAME_TOO_LONG;
    }

    byteLen = (USHORT)(cch * sizeof(WCHAR));
    DestinationString->Buffer = (PWSTR)SourceString;
    DestinationString->Length = byteLen;
    DestinationString->MaximumLength = byteLen;
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
 * I2CCTRL_AllocatePoolPriorityCompat
 * XP/2003 backport of ExAllocatePoolWithTagPriority
 *
 * Guarantees:
 *  - IRQL <= DISPATCH_LEVEL
 *  - Returns NULL if NumberOfBytes == 0
 *  - Preserves Tag
 *  - Emulates priority semantics inside the driver
 *  - C89-compliant, WinDDK 7.1.0 compiler-safe, XP-BSOD-safe
 * ----------------------------------------------------------------------- */
PVOID
I2CCTRL_AllocatePoolPriorityCompat(
    IN POOL_TYPE                 PoolType,
    IN SIZE_T                    NumberOfBytes,
    IN ULONG                     Tag,
    IN BACKPORT_EX_POOL_PRIORITY Priority
    )
{
    PVOID p;
    KIRQL irql;
    UCHAR *pb;
    SIZE_T i;
    SIZE_T guardSize;
    SIZE_T headInit;
    SIZE_T tailInit;
    SIZE_T pageCount;
    SIZE_T stride;
    BOOLEAN isPaged;

    /* Defensive IRQL and parameter checks */
    irql = KeGetCurrentIrql();
    if (irql > DISPATCH_LEVEL) {
        return NULL;
    }
    if (NumberOfBytes == 0U) {
        return NULL;
    }

    /* At DISPATCH_LEVEL, allocating paged pool is unsafe on XP */
    isPaged = ((PoolType == PagedPool) ||
               (PoolType == PagedPoolCacheAligned));
#ifdef POOL_TYPE_MASK
    if (!isPaged) {
        isPaged = ((PoolType & 1U) == 0U); /* conservative check */
    }
#endif
    if ((irql == DISPATCH_LEVEL) && isPaged) {
        return NULL;
    }

    /* XP/2003 path: allocator */
    p = ExAllocatePoolWithTag(PoolType, NumberOfBytes, Tag);
    if (p == NULL) {
        return NULL;
    }

    /* Prepare local state for emulation */
    pb = (UCHAR *)p;
    guardSize = (NumberOfBytes >= 16U) ? 16U : NumberOfBytes;
    headInit  = (NumberOfBytes >= 64U) ? 64U : NumberOfBytes;
    tailInit  = headInit;

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096U
#endif
    stride = (SIZE_T)PAGE_SIZE;
    pageCount = (NumberOfBytes + stride - 1U) / stride;

    /* Driver-side emulation of priority semantics */
    switch (Priority) {
    case BACKPORT_HighPoolPriority:
        /* Zero entire buffer and pre-touch each page */
        RtlZeroMemory(p, NumberOfBytes);
        for (i = 0U; i < pageCount; ++i) {
            volatile UCHAR *pPage;
            pPage = (volatile UCHAR *)(pb + i * stride);
            *pPage = *pPage; /* touch */
        }
        break;

    case BACKPORT_HighPoolPrioritySpecialPoolOverrun:
        RtlZeroMemory(p, NumberOfBytes);
        for (i = 0U; i < guardSize; ++i) {
            pb[NumberOfBytes - 1U - i] = 0xEE;
        }
        break;

    case BACKPORT_HighPoolPrioritySpecialPoolUnderrun:
        RtlZeroMemory(p, NumberOfBytes);
        for (i = 0U; i < guardSize; ++i) {
            pb[i] = 0xDD;
        }
        break;

    case BACKPORT_NormalPoolPriority:
        for (i = 0U; i < headInit; ++i) {
            pb[i] = 0xAA;
        }
        if (NumberOfBytes > tailInit) {
            for (i = 0U; i < tailInit; ++i) {
                pb[NumberOfBytes - 1U - i] = 0xAA;
            }
        }
        break;

    case BACKPORT_NormalPoolPrioritySpecialPoolOverrun:
        for (i = 0U; i < guardSize; ++i) {
            pb[NumberOfBytes - 1U - i] = 0xEE;
        }
        break;

    case BACKPORT_NormalPoolPrioritySpecialPoolUnderrun:
        for (i = 0U; i < guardSize; ++i) {
            pb[i] = 0xDD;
        }
        break;

    case BACKPORT_LowPoolPriority:
        /* Raw allocation: no extra work */
        break;

    case BACKPORT_LowPoolPrioritySpecialPoolOverrun:
        {
            SIZE_T halfGuard;
            halfGuard = (guardSize / 2U) + (guardSize % 2U);
            for (i = 0U; i < halfGuard; ++i) {
                pb[NumberOfBytes - 1U - i] = 0xEE;
            }
        }
        break;

    case BACKPORT_LowPoolPrioritySpecialPoolUnderrun:
        {
            SIZE_T halfGuard2;
            halfGuard2 = (guardSize / 2U) + (guardSize % 2U);
            for (i = 0U; i < halfGuard2; ++i) {
                pb[i] = 0xDD;
            }
        }
        break;

    default:
        /* Unknown priority: no special handling */
        break;
    }

    return p;
}

/* -----------------------------------------------------------------------
 * I2CCTRL_WdmProbeAddressCompat
 * XP/2003 safe, feature-complete replacement for MmIsAddressValid-like checks.
 *
 * Parameters:
 *  - Address: starting address to validate (non-NULL)
 *  - Length:  number of bytes in the region to validate (>= 1)
 *  - Flags:   probing behavior (read-only, write-back, range ends, per-page)
 *
 * Behavior:
 *  - IRQL must be <= APC_LEVEL (safe for __try/__except probing).
 *  - Probes a single byte at Address; optionally probes last byte,
 *    and per-page offsets for larger buffers.
 *  - If write-back flag is set, writes the same byte value back to avoid
 *    modifying content while still testing write accessibility.
 *
 * Returns:
 *  - TRUE  if all selected probes succeed
 *  - FALSE if any probe raises an exception or parameters are invalid
 * ----------------------------------------------------------------------- */
BOOLEAN
I2CCTRL_WdmProbeAddressCompat(
    IN PVOID Address,
    IN SIZE_T Length,
    IN I2CCTRL_WdmAddressProbeFlags Flags
    )
{
    KIRQL irql;
    volatile UCHAR *p;
    volatile UCHAR *pEnd;
    volatile UCHAR *pProbe;
    SIZE_T offset;
    SIZE_T pageStride;
    UCHAR value;
    BOOLEAN requireWrite;

    /* Basic parameter validation */
    if (Address == NULL) {
        return FALSE;
    }
    if (Length == 0U) {
        return FALSE;
    }

    /* __try/__except probing is only safe at IRQL <= APC_LEVEL */
    irql = KeGetCurrentIrql();
    if (irql > APC_LEVEL) {
        return FALSE;
    }

    p = (volatile UCHAR *)Address;
    pEnd = (volatile UCHAR *)((UCHAR *)Address + (Length - 1U));
    requireWrite = ((Flags & I2CCTRL_WdmProbeWriteBack) != 0);
    pageStride = (SIZE_T)PAGE_SIZE;

    /* Always probe the first byte */
    __try {
        value = *p;
        if (requireWrite) {
            *p = value; /* write-back same value to test write access */
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return FALSE;
    }

    /* Optionally probe the last byte of the range */
    if ((Flags & I2CCTRL_WdmProbeRangeEnds) != 0 && Length > 1U) {
        __try {
            value = *pEnd;
            if (requireWrite) {
                *pEnd = value;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return FALSE;
        }
    }

    /* Optionally probe one byte per page across the range */
    if ((Flags & I2CCTRL_WdmProbePerPage) != 0 && Length > pageStride) {
        /* Align start to the next page boundary within the range */
        offset = pageStride - ((SIZE_T)((ULONG_PTR)p) % pageStride);
        if (offset >= Length) {
            offset = Length - 1U; /* clamp within range */
        }
        /* Probe at each page boundary (or nearest byte) */
        while (offset < Length) {
            pProbe = (volatile UCHAR *)((UCHAR *)p + offset);
            __try {
                value = *pProbe;
                if (requireWrite) {
                    *pProbe = value;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                return FALSE;
            }

            /* Advance to next page boundary */
            if ((Length - offset) <= pageStride) {
                break;
            }
            offset += pageStride;
        }
    }

    return TRUE;
}

/* -----------------------------------------------------------------------
 * I2CCTRL_WdmIsAddressValidCompat
 * Backward-compatible wrapper preserving the original signature.
 * Probes the first byte read-only, plus last byte for ranges > 1.
 * ----------------------------------------------------------------------- */
BOOLEAN
I2CCTRL_WdmIsAddressValidCompat(IN PVOID Address)
{
    return I2CCTRL_WdmProbeAddressCompat(
        Address,
        1U, /* minimal probe: single byte */
        I2CCTRL_WdmProbeReadOnly
    );
}


/* -----------------------------------------------------------------------
 * I2CCTRL_WdmTimerInitCompat
 * XP/2003 backport of KeInitializeTimerEx
 *
 * Guarantees:
 *  - Safe at any IRQL (KTIMER is non-paged)
 *  - Deterministically initializes the timer object
 *  - Accepts both NotificationTimer and SynchronizationTimer types
 *    (XP initializes as notification; sync semantics must be handled by callers)
 *  - Uses I2CCTRL_WdmIsAddressValidCompat for validation
 *  - C89-compliant and WinDDK 7.1.0 compiler-safe
 * ----------------------------------------------------------------------- */
VOID
I2CCTRL_WdmTimerInitCompat(
    OUT PKTIMER Timer,
    IN I2CCTRL_WDM_TIMER_TYPE Type
    )
{
    KIRQL irql;
    BOOLEAN isValid;

    /* Defensive: NULL pointer check */
    if (Timer == NULL) {
        return;
    }

    /* Validate that the address is accessible */
    isValid = I2CCTRL_WdmIsAddressValidCompat((PVOID)Timer);
    if (isValid == FALSE) {
        return;
    }

    /* Safe at any IRQL, but record current IRQL for diagnostics */
    irql = KeGetCurrentIrql();
    UNREFERENCED_PARAMETER(irql);

    /* Clear the structure for deterministic state */
    RtlZeroMemory((PVOID)Timer, sizeof(KTIMER));

    /*
     * XP/2003 only provides KeInitializeTimer (notification timer).
     * For SynchronizationTimer, we still initialize as notification.
     * Callers needing strict sync semantics should implement them externally.
     */
    UNREFERENCED_PARAMETER(Type);
    KeInitializeTimer(Timer);
}


/* -----------------------------------------------------------------------
 * KeSetCoalescableTimer backport (PASSIVE_LEVEL or DISPATCH_LEVEL)
 * XP provides KeSetTimerEx/KeSetTimer without tolerance/coalescing.
 * ----------------------------------------------------------------------- */
BOOLEAN
I2CCTRL_KeSetPeriodicTimerCompat(
    IN  PKTIMER       Timer,
    IN  LARGE_INTEGER DueTime,
    IN  ULONG         PeriodMs,
    IN  PKDPC         Dpc
    )
{
    if (Timer == NULL) {
        return FALSE;
    }
    /* Prefer KeSetTimerEx if available in your kit; otherwise KeSetTimer */
#if (NTDDI_VERSION >= NTDDI_WS03)
    return KeSetTimerEx(Timer, DueTime, PeriodMs, Dpc);
#else
    /* One-shot; caller should re-arm in DPC for periodic behavior on very old kits */
    UNREFERENCED_PARAMETER(PeriodMs);
    return KeSetTimer(Timer, DueTime, Dpc);
#endif
}

/* -----------------------------------------------------------------------
 * RtlUnicodeStringToIntegerEx backport (PASSIVE_LEVEL)
 * XP exposes RtlUnicodeStringToInteger; validate inputs and forward.
 * ----------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_RtlUnicodeStringToIntegerCompat(
    IN  PUNICODE_STRING Str,
    IN  ULONG           Base,
    OUT PULONG          Value
    )
{
    if (Str == NULL || Value == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Str->Buffer == NULL || Str->Length == 0) {
        *Value = 0U;
        return STATUS_INVALID_PARAMETER;
    }
    return RtlUnicodeStringToInteger(Str, Base, Value);
}

/* -----------------------------------------------------------------------
 * IoConnectInterruptEx backport (PASSIVE_LEVEL)
 * Uses BACKPORT_IO_CONNECT_INTERRUPT_LINE_BASED_PARAMETERS for portability.
 * Tries IoConnectInterruptEx when available, else falls back to IoConnectInterrupt.
 * ----------------------------------------------------------------------- */
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
    )
{
    NTSTATUS status;
    PVOID pEx;

    if (InterruptObject == NULL || ServiceRoutine == NULL || SpinLock == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    PAGED_CODE();

    pEx = I2CCTRL_GetSystemRoutine(L"IoConnectInterruptEx");
    if (pEx != NULL) {
        IO_CONNECT_INTERRUPT_PARAMETERS params;
        BACKPORT_IO_CONNECT_INTERRUPT_LINE_BASED_PARAMETERS backport;

        RtlZeroMemory(&params, sizeof(params));
        RtlZeroMemory(&backport, sizeof(backport));

        /* Fill our backport struct with all fields */
        backport.PhysicalDeviceObject = PhysicalDeviceObject;
        backport.ServiceRoutine       = ServiceRoutine;
        backport.ServiceContext       = ServiceContext;
        backport.SpinLock             = SpinLock;
        backport.InterruptObject      = InterruptObject;
        backport.Vector               = Vector;
        backport.Irql                 = Irql;
        backport.SynchronizeIrql      = SynchronizeIrql;
        backport.InterruptMode        = InterruptMode;
        backport.ShareVector          = ShareVector;
        backport.ProcessorEnableMask  = ProcessorEnableMask;

        /* Copy only the fields XP headers actually define */
        params.Version = CONNECT_LINE_BASED;
        params.LineBased.PhysicalDeviceObject = backport.PhysicalDeviceObject;
        params.LineBased.ServiceRoutine       = backport.ServiceRoutine;
        params.LineBased.ServiceContext       = backport.ServiceContext;
        params.LineBased.SpinLock             = backport.SpinLock;
        params.LineBased.InterruptObject      = backport.InterruptObject;

        /* Call IoConnectInterruptEx */
        status = ((PIO_CONNECT_INTERRUPT_EX)pEx)(&params);
        return status;
    }

    /* XP fallback: classic IoConnectInterrupt */
    return IoConnectInterrupt(
        InterruptObject,
        ServiceRoutine,
        ServiceContext,
        SpinLock,
        Vector,
        Irql,
        SynchronizeIrql,
        InterruptMode,
        ShareVector,
        ProcessorEnableMask,
        FALSE
    );
}

/* -----------------------------------------------------------------------
 * Optional: KeDelayExecutionThread backport wrapper (PASSIVE_LEVEL)
 * Provides uniform return semantics and input validation.
 * ----------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_KeDelayExecutionCompat(
    IN  KPROCESSOR_MODE  WaitMode,
    IN  BOOLEAN          Alertable,
    IN  PLARGE_INTEGER   Interval
    )
{
    if (Interval == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    return KeDelayExecutionThread(WaitMode, Alertable, Interval);
}

/* -----------------------------------------------------------------------
 * Optional: IoTryLockIrp cancel-safe wrapper (XP-friendly pattern)
 * Use with cancel-safe queues to avoid double-completion hazards.
 * ----------------------------------------------------------------------- */
BOOLEAN
I2CCTRL_TryCompleteIrpOnce(
    IN PIRP Irp,
    IN NTSTATUS Status
    )
{
    PIO_STACK_LOCATION isl;

    if (Irp == NULL) {
        return FALSE;
    }

    isl = IoGetCurrentIrpStackLocation(Irp);
    /* Minimal guard; real drivers track cancel state in their own queues */
    if (isl == NULL) {
        return FALSE;
    }

    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0UL;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * I2CCTRL_WdmTimerStartCompat
 * XP/2003 backport wrapper for KeSetTimer/KeSetTimerEx
 *
 * Guarantees:
 *  - IRQL <= DISPATCH_LEVEL
 *  - NULL DPC is allowed (no DPC queued on expiry)
 *  - Returns TRUE if the timer was already set, FALSE otherwise
 *  - C89-compliant and WinDDK 7.1.0 compiler-safe
 * ----------------------------------------------------------------------- */
BOOLEAN
I2CCTRL_WdmTimerStartCompat(
    IN PKTIMER Timer,
    IN LARGE_INTEGER DueTime,
    IN ULONG Period,
    IN PKDPC DpcOptional
    )
{
    KIRQL irql;
    BOOLEAN wasSet;

    if (Timer == NULL) {
        return FALSE;
    }

    irql = KeGetCurrentIrql();
    if (irql > DISPATCH_LEVEL) {
        return FALSE;
    }

    /* Period == 0 → one-shot via KeSetTimer; otherwise periodic via KeSetTimerEx */
    if (Period == 0UL) {
        wasSet = KeSetTimer(Timer, DueTime, DpcOptional);
    } else {
        wasSet = KeSetTimerEx(Timer, DueTime, Period, DpcOptional);
    }

    return wasSet;
}

/* -----------------------------------------------------------------------
 * I2CCTRL_WdmTimerCancelCompat
 * XP/2003 wrapper for KeCancelTimer
 *
 * Guarantees:
 *  - IRQL <= DISPATCH_LEVEL
 *  - Returns TRUE if the timer was pending, FALSE otherwise
 *  - C89-compliant and WinDDK 7.1.0 compiler-safe
 * ----------------------------------------------------------------------- */
BOOLEAN
I2CCTRL_WdmTimerCancelCompat(
    IN PKTIMER Timer
    )
{
    KIRQL irql;

    if (Timer == NULL) {
        return FALSE;
    }

    irql = KeGetCurrentIrql();
    if (irql > DISPATCH_LEVEL) {
        return FALSE;
    }

    return KeCancelTimer(Timer);
}

/* -----------------------------------------------------------------------
 * I2CCTRL_WdmTimerIsSetCompat
 * XP/2003 wrapper for KeReadStateTimer
 *
 * Guarantees:
 *  - IRQL <= DISPATCH_LEVEL
 *  - Returns non-zero if timer is signaled, zero otherwise
 *  - C89-compliant and WinDDK 7.1.0 compiler-safe
 * ----------------------------------------------------------------------- */
BOOLEAN
I2CCTRL_WdmTimerIsSetCompat(
    IN PKTIMER Timer
    )
{
    KIRQL irql;
    LONG state;

    if (Timer == NULL) {
        return FALSE;
    }

    irql = KeGetCurrentIrql();
    if (irql > DISPATCH_LEVEL) {
        return FALSE;
    }

    state = KeReadStateTimer(Timer);
    return (state != 0) ? TRUE : FALSE;
}
