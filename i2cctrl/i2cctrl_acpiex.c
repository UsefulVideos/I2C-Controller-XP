/* i2cctrl_acpiex.c
 * ACPIEx-style façade matching i2cctrl_acpiex.h (C89 compliant)
 */

#include <ntddk.h>
#include "i2cctrl_acpiex.h"
#include "i2cctrl_dump.h"
#include "i2cctrl_hw.h"
#include "i2cctrl_spbcx.h"
#include "i2cctrl_ext.h"
#include "i2cctrl_ioctl.h"   /* ensure IOCTL_* are visible */

/* Forward declaration of the private helper used below */
PI2CCTRL_ACPIEX_HANDLE AcpiexGetOrCreateHandleContext(PIO_STACK_LOCATION Isl);

/* Associate our per-handle context with file objects */
PI2CCTRL_ACPIEX_HANDLE AcpiexGetOrCreateHandleContext(PIO_STACK_LOCATION Isl)
{
    PFILE_OBJECT fo;
    PI2CCTRL_ACPIEX_HANDLE h;

    fo = Isl->FileObject;
    h = (PI2CCTRL_ACPIEX_HANDLE)fo->FsContext;
    if (h == NULL) {
        h = (PI2CCTRL_ACPIEX_HANDLE)ExAllocatePoolWithTag(NonPagedPool,
                                                          sizeof(*h),
                                                          I2CCTRL_TAG_CTX);
        if (h != NULL) {
            RtlZeroMemory(h, sizeof(*h));
            fo->FsContext = h;
        }
    }
    return h;
}

VOID AcpiexFreeHandleContext(PIO_STACK_LOCATION Isl)
{
    PFILE_OBJECT fo;

    fo = Isl->FileObject;
    if (fo != NULL && fo->FsContext != NULL) {
        ExFreePoolWithTag(fo->FsContext, I2CCTRL_TAG_CTX);
        fo->FsContext = NULL;
    }
}

/* Public: device-level init hook */
VOID I2cCtrl_AcpiexInit(PI2CCTRL_FDO Dx)
{
    UNREFERENCED_PARAMETER(Dx);
    DbgPrint("I2CCTRL(acpiex): init\n");
}

NTSTATUS I2cCtrl_AcpiexCreate(PDEVICE_OBJECT Fdo, PIRP Irp, PIO_STACK_LOCATION Isl)
{
    UNREFERENCED_PARAMETER(Fdo);

    if (AcpiexGetOrCreateHandleContext(Isl) == NULL) {
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Irp->IoStatus.Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

NTSTATUS I2cCtrl_AcpiexClose(PDEVICE_OBJECT Fdo, PIRP Irp, PIO_STACK_LOCATION Isl)
{
    UNREFERENCED_PARAMETER(Fdo);

    AcpiexFreeHandleContext(Isl);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

NTSTATUS
I2cCtrl_AcpiexSetTarget(
    PI2CCTRL_FDO Dx,
    PI2CCTRL_ACPIEX_HANDLE H,
    I2CCTRL_TARGET_CONFIG In
)
{
    NTSTATUS status = STATUS_SUCCESS;

    if (Dx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (H == NULL) {
        return STATUS_INVALID_HANDLE;
    }

    /* Bind target context */
    H->Target.Address = In.Address & 0x03FF; /* mask to 10 bits */
    H->Target.Flags   = In.Flags;
    H->Target.Bound   = TRUE;

    DbgPrint("I2CCTRL(acpiex): SetTarget addr=0x%02X flags=0x%08X speed=%luHz\n",
             In.Address, In.Flags, In.SpeedHz);

    /* Map SpeedHz to mode and program controller */
    if (In.SpeedHz <= 100000UL) {
        status = I2cCtrl_SetBusSpeed(Dx, I2cSpeedStandard);
    } else if (In.SpeedHz <= 400000UL) {
        status = I2cCtrl_SetBusSpeed(Dx, I2cSpeedFast);
    } else if (In.SpeedHz <= 3400000UL) {
        status = I2cCtrl_SetBusSpeed(Dx, I2cSpeedHigh);
    } else {
        /* Unsupported speed requested */
        status = STATUS_NOT_SUPPORTED;
    }

    return status;
}


/* -----------------------------------------------------------------------
   I2cCtrl_AcpiexTransfer - Hardened ACPI transfer helper
   XP/2003 BSOD-safe, WinDDK-compiler-safe, C89-compliant
   ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_AcpiexTransfer(
    IN  PI2CCTRL_FDO           Dx,
    IN  PI2CCTRL_ACPIEX_HANDLE H,
    IN  I2CCTRL_XFER_DESC      In,
    OUT PUCHAR                 Buf,
    IN  SIZE_T                 BufLen
    )
{
    NTSTATUS          status;
    I2CCTRL_XFER_DESC xfer;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    PAGED_CODE();

    /* C89 init */
    status = STATUS_SUCCESS;
    xfer.Address7Bit = 0U;
    xfer.Length      = 0U;
    xfer.Flags       = 0U;

    if (Dx == NULL || H == NULL || Buf == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!H->Target.Bound) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (BufLen == 0U) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (BufLen > 0xFFFFFFFFUL) {
        return STATUS_INVALID_PARAMETER; /* cannot fit into ULONG */
    }

    /* Defensive copy and validation of transfer descriptor */
    xfer = In;

    /* Validate address and length against buffer and driver caps */
    if ((xfer.Address7Bit & ~0x7FU) != 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    if (xfer.Length == 0U || xfer.Length > BufLen) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Dx->MaxTransferLen != 0U && xfer.Length > Dx->MaxTransferLen) {
        return STATUS_BUFFER_OVERFLOW;
    }

    /* Optional: clear output buffer on entry to avoid leaking stale data */
    RtlZeroMemory(Buf, (SIZE_T)xfer.Length);

    __try {
        status = I2cCtrl_IoctlTransfer(
                     Dx->Self,        /* FDO device object */
                     Dx,              /* FDO extension */
                     &H->Target,      /* ACPIEX target binding */
                     Buf,             /* output buffer */
                     (ULONG)BufLen);  /* safe cast after range check */
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_ACCESS_VIOLATION;
    }

    return status;
}

/* -----------------------------------------------------------------------
   I2cCtrl_AcpiexProbe - Hardened ACPI probe helper
   XP/2003 BSOD-safe, WinDDK-compiler-safe, C89-compliant
   ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_AcpiexProbe(
    IN  PI2CCTRL_FDO           Dx,
    IN  PI2CCTRL_ACPIEX_HANDLE H,
    IN  I2CCTRL_PROBE          In,
    OUT PULONG                 OutPresenceMask
    )
{
    NTSTATUS       status;
    I2CCTRL_PROBE  probe;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    PAGED_CODE();

    /* C89 init */
    status = STATUS_SUCCESS;
    probe.Present = 0U;
    probe.Flags   = 0U;

    if (Dx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (H == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    if (OutPresenceMask == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Defensive copy of input probe */
    probe = In;

    /* Always initialize output mask to 0 */
    *OutPresenceMask = 0UL;

    __try {
        status = I2cCtrl_IoctlProbe(
                     Dx,
                     &H->Target,
                     &probe,
                     sizeof(probe));
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_ACCESS_VIOLATION;
    }

    if (NT_SUCCESS(status)) {
        *OutPresenceMask = (probe.Present != 0U) ? 1UL : 0UL;
    }

    return status;
}

/*
 * I2cCtrl_AcpiexCloseHandle - Hardened ACPI handle close
 * XP/2003‑safe, ACPI‑safe, idempotent.
 * - PASSIVE_LEVEL only
 * - Validates FDO and handle
 * - SEH around ACPI close
 * - Always clears handle
 */
VOID
I2cCtrl_AcpiexCloseHandle(
    IN PI2CCTRL_FDO fdoExt
    )
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    PAGED_CODE();

    if (fdoExt == NULL) {
        return;
    }

    if (fdoExt->AcpiHandle == NULL) {
        return;     /* Already closed or never opened */
    }

    __try {
        I2cCtrl_AcpiCloseHandle(fdoExt->AcpiHandle);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        /* Swallow ACPI exceptions to avoid XP/2003 BSOD */
    }

    /* Prevent dangling pointer regardless of success/failure */
    fdoExt->AcpiHandle = NULL;
}



/* Hardened ACPI child close: PASSIVE_LEVEL only, ACPI-safe */
VOID
I2cCtrl_AcpiexCloseChild(
    IN PI2CCTRL_PDO ChildDx
    )
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    PAGED_CODE();

    if (ChildDx == NULL) {
        return;
    }

    if (ChildDx->AcpiHandle == NULL) {
        return;
    }

    __try {
        I2cCtrl_AcpiCloseChild(ChildDx);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        /* Swallow any unexpected ACPI exceptions */
    }

    /* I2cCtrl_AcpiCloseChild already NULLs AcpiHandle; do not touch it again here */
}

/* -----------------------------------------------------------------------
   I2cCtrl_AcpiexSequence - Hardened ACPI sequence helper
   XP/2003 BSOD-safe, WinDDK-compiler-safe, C89-compliant
   ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_AcpiexSequence(
    IN  PI2CCTRL_FDO           Dx,
    IN  PI2CCTRL_ACPIEX_HANDLE H,
    IN  PI2CCTRL_SEQUENCE_HDR  Seq,
    OUT PUCHAR                 Buf,
    IN  SIZE_T                 BufLen
    )
{
    NTSTATUS status;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    PAGED_CODE();

    /* C89 init */
    status = STATUS_SUCCESS;

    if (Dx == NULL || H == NULL || Buf == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!H->Target.Bound) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (BufLen == 0U) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (BufLen > 0xFFFFFFFFUL) {
        return STATUS_INVALID_PARAMETER; /* cannot fit into ULONG */
    }

    __try {
        status = I2cCtrl_IoctlSequence(
                     Dx->Self,       /* FDO device object */
                     Dx,             /* FDO extension */
                     &H->Target,     /* ACPIEX target binding */
                     Buf,            /* output buffer */
                     (ULONG)BufLen); /* safe cast */
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_ACCESS_VIOLATION;
    }

    return status;
}

/* DeviceControl: reuse your existing IOCTL IDs and buffer layouts (METHOD_BUFFERED) */
NTSTATUS I2cCtrl_AcpiexDeviceControl(PDEVICE_OBJECT Fdo,
                                     PIRP Irp,
                                     PIO_STACK_LOCATION Isl)
{
    PI2CCTRL_FDO Dx;
    PI2CCTRL_ACPIEX_HANDLE H;
    ULONG code;
    PVOID inBuf;
    PVOID outBuf;
    ULONG inLen;
    ULONG outLen;
    NTSTATUS status;

    Dx     = (PI2CCTRL_FDO)Fdo->DeviceExtension;
    H      = AcpiexGetOrCreateHandleContext(Isl);
    code   = Isl->Parameters.DeviceIoControl.IoControlCode;
    inBuf  = Irp->AssociatedIrp.SystemBuffer;
    outBuf = Irp->AssociatedIrp.SystemBuffer;
    inLen  = Isl->Parameters.DeviceIoControl.InputBufferLength;
    outLen = Isl->Parameters.DeviceIoControl.OutputBufferLength;
    status = STATUS_INVALID_DEVICE_REQUEST;

    switch (code) {
    case IOCTL_SET_TARGET:
        if (inBuf != NULL && inLen >= sizeof(I2CCTRL_TARGET_CONFIG)) {
            status = I2cCtrl_AcpiexSetTarget(Dx, H,
                      *(PI2CCTRL_TARGET_CONFIG)inBuf);
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_XFER_DESC:
        if (inBuf != NULL && inLen >= sizeof(I2CCTRL_XFER_DESC)) {
            I2CCTRL_XFER_DESC tr;
            PUCHAR payload;
            SIZE_T payloadLen;

            tr = *(PI2CCTRL_XFER_DESC)inBuf;
            payload = (PUCHAR)inBuf + sizeof(I2CCTRL_XFER_DESC);
            payloadLen = inLen - sizeof(I2CCTRL_XFER_DESC);

            status = I2cCtrl_AcpiexTransfer(Dx, H, tr, payload, payloadLen);
            if (NT_SUCCESS(status)) {
                Irp->IoStatus.Information = tr.Length;
            }
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_SEQUENCE:
        if (inBuf != NULL && inLen >= sizeof(I2CCTRL_SEQUENCE_HDR)) {
            I2CCTRL_SEQUENCE_HDR seq;
            PI2CCTRL_SEQUENCE_HDR hdr;
            PUCHAR payload;
            SIZE_T payloadLen;

            hdr = (PI2CCTRL_SEQUENCE_HDR)inBuf;
            seq = *hdr;

            payload    = (PUCHAR)inBuf + sizeof(I2CCTRL_SEQUENCE_HDR);
            payloadLen = inLen - sizeof(I2CCTRL_SEQUENCE_HDR);

            status = I2cCtrl_AcpiexSequence(Dx, H, &seq, payload, payloadLen);
            if (NT_SUCCESS(status)) {
                Irp->IoStatus.Information = (ULONG)seq.OutLength;
            }
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_PROBE:
        if (outBuf != NULL && outLen >= sizeof(ULONG) &&
            inBuf != NULL && inLen >= sizeof(I2CCTRL_PROBE)) {
            ULONG presence = 0;

            status = I2cCtrl_AcpiexProbe(Dx, H,
                      *(PI2CCTRL_PROBE)inBuf, &presence);
            if (NT_SUCCESS(status)) {
                *(PULONG)outBuf = presence;
                Irp->IoStatus.Information = sizeof(ULONG);
            }
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;

    case IOCTL_I2C_FORCE_CRASH:
        I2cCtrl_ForceCrash(Dx, STATUS_UNSUCCESSFUL);
        status = STATUS_SUCCESS; /* not returned */
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    return status;
}
