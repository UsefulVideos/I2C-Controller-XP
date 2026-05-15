/* -----------------------------------------------------------------------
   i2chid_acpi.c - ACPI parsing for HID-over-I2C (PNP0C50) on XP/2003
   ----------------------------------------------------------------------- */

#include "i2chid_spinlock_fix.h"
#include "..\i2cctrl\i2cctrl_DPI.h"
#include "..\i2cctrl\i2cctrl_ioctl.h"  /* IOCTL_I2C_READ, IOCTL_I2C_WRITE, I2CCTRL_TRANSFER */
#include "i2chid_DPI.h"
#include "i2chid_acpi.h"
#include "..\i2cctrl\i2cctrl_ext.h"   /* include ONLY for shared enums, logging, GUIDs */


/* Walk ACPI output arguments safely */
static PACPI_METHOD_ARGUMENT
AcpiGetArgument(PACPI_EVAL_OUTPUT_BUFFER Out, ULONG Index)
{
    PUCHAR p;
    ULONG  i;
    PACPI_METHOD_ARGUMENT arg;

    if (Out == NULL) {
        return NULL;
    }

    /* ACPI_EVAL_OUTPUT_BUFFER has Argument[ANYSIZE_ARRAY], not Data[] */
    p = (PUCHAR)&Out->Argument[0];

    for (i = 0; i < Out->Count; i++) {
        arg = (PACPI_METHOD_ARGUMENT)p;
        if (i == Index) {
            return arg;
        }
        p += sizeof(ACPI_METHOD_ARGUMENT) + arg->DataLength;
    }

    return NULL;
}


/* Evaluate _DSM(UUID, rev, func, arg) → returns buffer/integer */
static NTSTATUS
AcpiEvalDsm(
    IN PDEVICE_OBJECT            Pdo,
    IN const GUID*               Uuid,
    IN ULONG                     Revision,
    IN ULONG                     FunctionIndex,
    IN ULONG                     ArgInteger,
    OUT PACPI_EVAL_OUTPUT_BUFFER OutBuf,
    IN ULONG                     OutBufLen
    )
{
    NTSTATUS status;
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PIRP irp;
    PIO_STACK_LOCATION isl;
    ACPI_EVAL_INPUT_BUFFER_COMPLEX* inBuf;
    ULONG inSize;
    PACPI_METHOD_ARGUMENT arg;

    if (Pdo == NULL || Uuid == NULL || OutBuf == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    inSize = sizeof(ACPI_EVAL_INPUT_BUFFER_COMPLEX) +
             (sizeof(ACPI_METHOD_ARGUMENT) * 4);

    inBuf = (ACPI_EVAL_INPUT_BUFFER_COMPLEX*)
            ExAllocatePoolWithTag(NonPagedPool, inSize, 'cpsA');
    if (inBuf == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(inBuf, inSize);
    inBuf->Signature     = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    inBuf->MethodName[0] = '_';
    inBuf->MethodName[1] = 'D';
    inBuf->MethodName[2] = 'S';
    inBuf->MethodName[3] = 'M';
    inBuf->Size          = inSize;
    inBuf->ArgumentCount = 4;

    /* Arg0: UUID buffer */
    arg = &inBuf->Argument[0];
    arg->Type       = ACPI_METHOD_ARGUMENT_BUFFER;
    arg->DataLength = sizeof(GUID);
    RtlCopyMemory(arg->Data, Uuid, sizeof(GUID));
    ACPI_METHOD_NEXT_ARGUMENT(arg);

    /* Arg1: Revision */
    arg->Type       = ACPI_METHOD_ARGUMENT_INTEGER;
    arg->DataLength = sizeof(ULONG);
    arg->Argument   = Revision;
    ACPI_METHOD_NEXT_ARGUMENT(arg);

    /* Arg2: FunctionIndex */
    arg->Type       = ACPI_METHOD_ARGUMENT_INTEGER;
    arg->DataLength = sizeof(ULONG);
    arg->Argument   = FunctionIndex;
    ACPI_METHOD_NEXT_ARGUMENT(arg);

    /* Arg3: ArgInteger */
    arg->Type       = ACPI_METHOD_ARGUMENT_INTEGER;
    arg->DataLength = sizeof(ULONG);
    arg->Argument   = ArgInteger;

    RtlZeroMemory(OutBuf, OutBufLen);
    OutBuf->Signature = ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(
              IOCTL_ACPI_EVAL_METHOD,
              Pdo,
              inBuf,
              inSize,
              OutBuf,
              OutBufLen,
              FALSE,
              &event,
              &iosb);
    if (irp == NULL) {
        ExFreePoolWithTag(inBuf, 'cpsA');
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    isl = IoGetNextIrpStackLocation(irp);
    isl->Parameters.DeviceIoControl.InputBufferLength  = inSize;
    isl->Parameters.DeviceIoControl.OutputBufferLength = OutBufLen;

    status = IoCallDriver(Pdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    ExFreePoolWithTag(inBuf, 'cpsA');
    return status;
}

/* Query HID descriptor register address via _DSM FN 1 */
static NTSTATUS
AcpiQueryHidDescriptorAddress(IN PDEVICE_OBJECT Pdo, OUT ULONG* HidDescAddrOut)
{
    NTSTATUS status;
    ACPI_EVAL_OUTPUT_BUFFER* outBuf;
    PACPI_METHOD_ARGUMENT arg;

    if (Pdo == NULL || HidDescAddrOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    outBuf = (ACPI_EVAL_OUTPUT_BUFFER*)ExAllocatePoolWithTag(NonPagedPool, 1024, 'cpsO');
    if (outBuf == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = AcpiEvalDsm(Pdo,
                         &I2C_HID_DSM_GUID,
                         0,
                         I2C_HID_DSM_FN_HID_DESCRIPTOR_ADDR,
                         0,
                         outBuf,
                         1024);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(outBuf, 'cpsO');
        return status;
    }

    arg = AcpiGetArgument(outBuf, 0);
    if (arg == NULL || arg->Type != ACPI_METHOD_ARGUMENT_INTEGER) {
        ExFreePoolWithTag(outBuf, 'cpsO');
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    *HidDescAddrOut = arg->Argument;
    ExFreePoolWithTag(outBuf, 'cpsO');
    return STATUS_SUCCESS;
}

/* Parse translated resources for I2C address and interrupt info */
static NTSTATUS
ParseTranslatedResources(
    IN PCM_RESOURCE_LIST Translated,
    OUT UCHAR* I2cAddr7BitOut,
    OUT KINTERRUPT_MODE* ModeOut,
    OUT BOOLEAN* SharableOut
    )
{
    ULONG i;
    ULONG j;
    PCM_FULL_RESOURCE_DESCRIPTOR frd;
    PCM_PARTIAL_RESOURCE_LIST prl;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR prd;

    if (Translated == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (I2cAddr7BitOut) *I2cAddr7BitOut = 0;
    if (ModeOut)        *ModeOut        = Latched;
    if (SharableOut)    *SharableOut    = FALSE;

    frd = &Translated->List[0];
    for (i = 0; i < Translated->Count; i++, frd++) {
        prl = &frd->PartialResourceList;
        prd = prl->PartialDescriptors;
        for (j = 0; j < prl->Count; j++, prd++) {
            if (prd->Type == CmResourceTypeInterrupt) {
                if (ModeOut) {
                    *ModeOut = (prd->Flags & CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE)
                               ? LevelSensitive : Latched;
                }
                if (SharableOut) {
                    *SharableOut = (prd->ShareDisposition == CmResourceShareShared);
                }
            }
            /* Other resource types ignored or TODO */
        }
    }
    return STATUS_SUCCESS;
}

/* Generic I2C read helper for XP drivers */
NTSTATUS
I2cRead(
    IN PDEVICE_OBJECT ControllerDevice,
    IN UCHAR I2cAddr7Bit,
    IN ULONG RegisterOffset,
    OUT PUCHAR Buffer,
    IN ULONG Length,
    OUT PULONG BytesRead
    )
{
    NTSTATUS status;
    I2C_XP_REQUEST request;
    PIRP irp;
    IO_STATUS_BLOCK ioStatus;
    KEVENT event;

    status = STATUS_SUCCESS;
    *BytesRead = 0;

    if (ControllerDevice == NULL || Buffer == NULL || Length == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Fill request fields */
    request.Address = I2cAddr7Bit;
    request.Offset  = RegisterOffset;
    request.Buffer  = Buffer;
    request.Length  = Length;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    /* Build IRP for IOCTL_I2C_READ (must be defined by your controller driver) */
    irp = IoBuildDeviceIoControlRequest(
              IOCTL_I2C_READ,          /* control code your controller supports */
              ControllerDevice,        /* target device object */
              &request,                /* input buffer */
              sizeof(I2C_XP_REQUEST),  /* input length */
              Buffer,                  /* output buffer */
              Length,                  /* output length */
              FALSE,                   /* internal device control */
              &event,                  /* event for sync */
              &ioStatus);              /* I/O status block */

    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Call down synchronously */
    status = IoCallDriver(ControllerDevice, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = ioStatus.Status;
    }

    if (!NT_SUCCESS(status)) {
        return status;
    }

    *BytesRead = (ULONG)ioStatus.Information;
    return STATUS_SUCCESS;
}

/* Helper: query HID descriptor length by reading first two bytes over I2C */
NTSTATUS
I2cHid_QueryHidDescriptorLength(
    IN PDEVICE_OBJECT ControllerDevice,
    IN UCHAR I2cAddr,
    IN ULONG HidDescAddr,
    OUT PULONG HidDescLen
    )
{
    NTSTATUS status;
    UCHAR buf[2];
    ULONG bytesRead;

    *HidDescLen = 0;
    bytesRead = 0;

    /* Issue an I2C read of 2 bytes from the HID descriptor address */
    status = I2cRead(ControllerDevice,
                     I2cAddr,
                     HidDescAddr,
                     buf,
                     sizeof(buf),
                     &bytesRead);

    if (!NT_SUCCESS(status) || bytesRead < 2) {
        return STATUS_UNSUCCESSFUL;
    }

    /* HID descriptor length is a 16-bit little-endian value at offset 0 */
    *HidDescLen = (ULONG)buf[0] | ((ULONG)buf[1] << 8);

    return STATUS_SUCCESS;
}

/* Public entry: parse ACPI for PNP0C50 and populate DPI */
NTSTATUS
I2cHid_AcpiParsePnp0C50(
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    IN PCM_RESOURCE_LIST RawResources,
    IN PCM_RESOURCE_LIST TranslatedResources,
    OUT PI2CCTRL_DPI Dpi
    )
{
    NTSTATUS status;
    ULONG hidDescAddr;
    UCHAR i2cAddr;
    KINTERRUPT_MODE mode;
    BOOLEAN sharable;
    ULONG hidDescLen;

    if (PhysicalDeviceObject == NULL || Dpi == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    hidDescAddr = 0;
    i2cAddr     = 0;
    mode        = Latched;
    sharable    = FALSE;
    hidDescLen  = 0;

    /* Query HID descriptor address from ACPI */
    status = AcpiQueryHidDescriptorAddress(PhysicalDeviceObject, &hidDescAddr);
    if (!NT_SUCCESS(status)) {
        hidDescAddr = 0;
    }

    /* Parse translated resources for I2C address and interrupt info */
    (void)ParseTranslatedResources(TranslatedResources, &i2cAddr, &mode, &sharable);

    /* Defensive: ensure 7-bit address is valid */
    if (i2cAddr == 0) {
        i2cAddr = 0x2C; /* common default for ELAN/PNP0C50 */
    }

    /* Initialize controller device pointer */
    Dpi->ControllerDevice = NULL;

    /* Query HID descriptor length directly from device if ControllerDevice is set later */
    if (hidDescAddr != 0 && Dpi->ControllerDevice != NULL) {
        (void)I2cHid_QueryHidDescriptorLength(Dpi->ControllerDevice,
                                              i2cAddr,
                                              hidDescAddr,
                                              &hidDescLen);
    }

    /* Populate DPI structure */
    Dpi->I2cAddr7Bit         = i2cAddr;
    Dpi->InterruptMode       = mode;
    Dpi->InterruptSharable   = sharable;
    Dpi->HidDescriptor       = (PVOID)(ULONG_PTR)hidDescAddr;
    Dpi->HidDescriptorLength = hidDescLen;

    return STATUS_SUCCESS;
}
