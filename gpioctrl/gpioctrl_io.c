/* gpioctrl_io.c
 * GPIO Controller Driver (gpioctrl.sys) - IOCTL handlers
 * WinDDK 7.1.0 - XP/2003 build environment - C89 compliant
 *
 * Implements:
 *  - GpioCtrl_IoctlReadPin
 *  - GpioCtrl_IoctlWritePin
 *  - GpioCtrl_IoctlConfigurePin
 *  - GpioCtrl_IoctlQueryCaps
 *
 * Notes:
 *  - Uses METHOD_BUFFERED; input/output via Irp->AssociatedIrp.SystemBuffer.
 *  - Validates buffer sizes and pin ranges.
 *  - Synchronizes MMIO access with RegLock.
 */

#include <ntddk.h>
#include "gpioctrl_ext.h"

/* ---------------------------------------------------------------------------
   Local helpers
   --------------------------------------------------------------------------- */
static NTSTATUS
GpioIo_ValidateReady(
    IN PGPIOCTRL_FDO Ext
    )
{
    if (!Ext->Started || Ext->MmioBase == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
GpioIo_ValidatePin(
    IN PGPIOCTRL_FDO Ext,
    IN ULONG Pin
    )
{
    if (Pin >= Ext->PinCount) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   IOCTL: READ_PIN
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_IoctlReadPin(
    IN PGPIOCTRL_FDO Ext,
    IN PIRP               Irp
    )
{
    NTSTATUS status;
    PIO_STACK_LOCATION isl;
    PGPIOCTRL_READ_PIN in;
    PULONG out;
    ULONG inLen, outLen;
    ULONG pin;
    ULONG data;

    isl   = IoGetCurrentIrpStackLocation(Irp);
    in    = (PGPIOCTRL_READ_PIN)Irp->AssociatedIrp.SystemBuffer;
    out   = (PULONG)Irp->AssociatedIrp.SystemBuffer;
    inLen = isl->Parameters.DeviceIoControl.InputBufferLength;
    outLen= isl->Parameters.DeviceIoControl.OutputBufferLength;

    if (inLen < sizeof(GPIOCTRL_READ_PIN) || outLen < sizeof(ULONG)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = GpioIo_ValidateReady(Ext);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    pin = in->Pin;
    status = GpioIo_ValidatePin(Ext, pin);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    data = GpioRegRead(Ext, REG_DATA_IN_OFFSET);
    *out = ((data >> pin) & 0x1);
    Irp->IoStatus.Information = sizeof(ULONG);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   IOCTL: WRITE_PIN
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_IoctlWritePin(
    IN PGPIOCTRL_FDO Ext,
    IN PIRP               Irp
    )
{
    NTSTATUS status;
    PIO_STACK_LOCATION isl;
    PGPIOCTRL_WRITE_PIN in;
    ULONG inLen;
    ULONG pin, value;
    ULONG data;
    KIRQL oldIrql;

    isl   = IoGetCurrentIrpStackLocation(Irp);
    in    = (PGPIOCTRL_WRITE_PIN)Irp->AssociatedIrp.SystemBuffer;
    inLen = isl->Parameters.DeviceIoControl.InputBufferLength;

    if (inLen < sizeof(GPIOCTRL_WRITE_PIN)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = GpioIo_ValidateReady(Ext);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    pin   = in->Pin;
    value = (in->Value != 0) ? 1 : 0;

    status = GpioIo_ValidatePin(Ext, pin);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    KeAcquireSpinLock(&Ext->RegLock, &oldIrql);
    data = GpioRegRead(Ext, REG_DATA_OUT_OFFSET);
    if (value) {
        data |= (1UL << pin);
    } else {
        data &= ~(1UL << pin);
    }
    GpioRegWrite(Ext, REG_DATA_OUT_OFFSET, data);
    KeReleaseSpinLock(&Ext->RegLock, oldIrql);

    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   IOCTL: CONFIGURE_PIN
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_IoctlConfigurePin(
    IN PGPIOCTRL_FDO Ext,
    IN PIRP               Irp
    )
{
    NTSTATUS status;
    PIO_STACK_LOCATION isl;
    PGPIOCTRL_CONFIGURE_PIN in;
    ULONG inLen;
    ULONG pin;
    ULONG dir, pull, ien, itype, ipol;
    ULONG reg;
    KIRQL oldIrql;

    isl   = IoGetCurrentIrpStackLocation(Irp);
    in    = (PGPIOCTRL_CONFIGURE_PIN)Irp->AssociatedIrp.SystemBuffer;
    inLen = isl->Parameters.DeviceIoControl.InputBufferLength;

    if (inLen < sizeof(GPIOCTRL_CONFIGURE_PIN)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = GpioIo_ValidateReady(Ext);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    pin   = in->Pin;
    dir   = (in->Direction != 0) ? 1 : 0;
    pull  = (in->Pull != 0) ? 1 : 0;
    ien   = (in->IntEnable != 0) ? 1 : 0;
    itype = (in->IntType != 0) ? 1 : 0;
    ipol  = (in->IntPol != 0) ? 1 : 0;

    status = GpioIo_ValidatePin(Ext, pin);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    KeAcquireSpinLock(&Ext->RegLock, &oldIrql);

    /* Direction */
    reg = GpioRegRead(Ext, REG_DIR_OFFSET);
    if (dir) reg |= (1UL << pin); else reg &= ~(1UL << pin);
    GpioRegWrite(Ext, REG_DIR_OFFSET, reg);

    /* Pull (if supported) */
    if (Ext->SupportsPull) {
        reg = GpioRegRead(Ext, REG_PULL_OFFSET);
        if (pull) reg |= (1UL << pin); else reg &= ~(1UL << pin);
        GpioRegWrite(Ext, REG_PULL_OFFSET, reg);
    }

    /* Interrupt type/polarity/enable (if supported) */
    if (Ext->SupportsInterrupts) {
        reg = GpioRegRead(Ext, REG_INT_TYPE_OFFSET);
        if (itype) reg |= (1UL << pin); else reg &= ~(1UL << pin);
        GpioRegWrite(Ext, REG_INT_TYPE_OFFSET, reg);

        reg = GpioRegRead(Ext, REG_INT_POL_OFFSET);
        if (ipol) reg |= (1UL << pin); else reg &= ~(1UL << pin);
        GpioRegWrite(Ext, REG_INT_POL_OFFSET, reg);

        reg = GpioRegRead(Ext, REG_INT_EN_OFFSET);
        if (ien) reg |= (1UL << pin); else reg &= ~(1UL << pin);
        GpioRegWrite(Ext, REG_INT_EN_OFFSET, reg);

        /* Clear any stale pending bit for this pin (W1C) */
        reg = (1UL << pin);
        GpioRegWrite(Ext, REG_INT_STAT_OFFSET, reg);
    }

    KeReleaseSpinLock(&Ext->RegLock, oldIrql);

    Irp->IoStatus.Information = 0;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
   IOCTL: QUERY_CAPS
   --------------------------------------------------------------------------- */
NTSTATUS
GpioCtrl_IoctlQueryCaps(
    IN PGPIOCTRL_FDO Ext,
    IN PIRP               Irp
    )
{
    NTSTATUS status;
    PIO_STACK_LOCATION isl;
    PGPIOCTRL_CAPS out;
    ULONG outLen;

    isl    = IoGetCurrentIrpStackLocation(Irp);
    out    = (PGPIOCTRL_CAPS)Irp->AssociatedIrp.SystemBuffer;
    outLen = isl->Parameters.DeviceIoControl.OutputBufferLength;

    if (outLen < sizeof(GPIOCTRL_CAPS)) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    status = GpioIo_ValidateReady(Ext);
    if (!NT_SUCCESS(status)) {
        Irp->IoStatus.Information = 0;
        return status;
    }

    out->PinCount           = Ext->PinCount;
    out->SupportsPull       = Ext->SupportsPull;
    out->SupportsInterrupts = Ext->SupportsInterrupts;
    out->SupportsDebounce   = Ext->SupportsDebounce;

    Irp->IoStatus.Information = sizeof(GPIOCTRL_CAPS);
    return STATUS_SUCCESS;
}
