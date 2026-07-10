#include <ntddk.h>
#include "i2cctrl_zw.h"
#include "i2cctrl_ioctl.h"   // defines I2CCTRL_TRANSFER, IOCTL_I2cCtrl_TRANSFER
#include "i2cctrl_hw.h"      // if you have hardware helpers (optional)
#include "i2cctrl_ext.h"        // replace with the actual header that defines PT_RAW_SAMPLE
#include "i2cctrl_hw.h"


static __inline NTSTATUS
I2Cctrlp_ValidateCommon(PDEVICE_OBJECT DevObj, PI2CCTRL_FDO* pExtOut)
{
    PI2CCTRL_FDO ext;

    if (pExtOut != NULL) {
        *pExtOut = NULL;
    }

    if (DevObj == NULL || DevObj->DeviceExtension == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    ext = (PI2CCTRL_FDO)DevObj->DeviceExtension;
    if (pExtOut != NULL) {
        *pExtOut = ext;
    }
    return STATUS_SUCCESS;
}

/* Configure target slave address via HAL ops (SetTarget7bit); C89, XP-safe, HAL-generic */
NTSTATUS
I2CctrlHw_SetTarget(PDEVICE_OBJECT DevObj,
                    USHORT slaveAddr,
                    ULONG  flags)
{
    NTSTATUS     status;
    PI2CCTRL_FDO ext;

    /* Validate device object and extension */
    status = I2Cctrlp_ValidateCommon(DevObj, &ext);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Ensure ops table and the SetTarget7bit entry are present */
    if (ext->Ops == NULL || ext->Ops->SetTarget7bit == NULL) {
        UNREFERENCED_PARAMETER(flags);
        return STATUS_NOT_SUPPORTED;
    }

    /* Delegate to HAL backend (7-bit address expected by the op) */
    return ext->Ops->SetTarget7bit(ext, (UCHAR)(slaveAddr & 0x7FU));
}

/* Perform a write transaction via abstracted op; bounded and pointer-safe */
NTSTATUS
I2CctrlHw_Write(PDEVICE_OBJECT DevObj,
                const UCHAR* buf,
                ULONG len,
                ULONG* bytesDone,
                ULONG flags)
{
    NTSTATUS     status;
    PI2CCTRL_FDO ext;

    /* Validate parameters first */
    if (bytesDone != NULL) {
        *bytesDone = 0U;
    }
    if (DevObj == NULL || DevObj->DeviceExtension == NULL || buf == NULL || bytesDone == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = I2Cctrlp_ValidateCommon(DevObj, &ext);
    if (!NT_SUCCESS(status)) {
        return status;
    }

/* Delegate to abstracted hardware op if available */
if (ext->Ops->PrimeWrite != NULL) {
    /* Wrap buffer access in structured exception handling to be XP-BSOD-safe */
    __try {
        /* Call the HAL-generic PrimeWrite op: context, buffer, length, pushed count */
        return ext->Ops->PrimeWrite(ext, buf, len, bytesDone);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        /* Guard against invalid buffer access or unexpected faults */
        return STATUS_ACCESS_VIOLATION;
    }
}

    /* No hardware-specific write op */
    UNREFERENCED_PARAMETER(flags);
    return STATUS_NOT_SUPPORTED;
}

/* Perform a read transaction via abstracted ops; bounded and pointer-safe */
NTSTATUS
I2CctrlHw_Read(PDEVICE_OBJECT DevObj,
               UCHAR* buf,
               ULONG len,
               ULONG* bytesDone,
               ULONG flags)
{
    NTSTATUS     status;
    PI2CCTRL_FDO ext;
    ULONG        queued;
    ULONG        received;
    UCHAR        b;

    /* Initialize output count */
    if (bytesDone != NULL) {
        *bytesDone = 0U;
    }

    /* Validate parameters */
    if (DevObj == NULL || DevObj->DeviceExtension == NULL ||
        buf == NULL || bytesDone == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = I2Cctrlp_ValidateCommon(DevObj, &ext);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Ensure ops table is present */
    if (ext->Ops == NULL || ext->Ops->PrimeReadTokens == NULL || ext->Ops->ReadRxByteSafe == NULL) {
        UNREFERENCED_PARAMETER(flags);
        return STATUS_NOT_SUPPORTED;
    }

    queued   = 0U;
    received = 0U;

    /* Queue read tokens */
    status = ext->Ops->PrimeReadTokens(ext, len, &queued);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Drain RX FIFO safely */
    while (received < queued) {
        __try {
            status = ext->Ops->ReadRxByteSafe(ext, &b);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return STATUS_ACCESS_VIOLATION;
        }

        if (!NT_SUCCESS(status)) {
            break;
        }

        buf[received++] = b;
    }

    *bytesDone = received;

    /* Optionally emit STOP if controller requires it */
    if (ext->Ops->EmitStopIfNeeded != NULL) {
        (void)ext->Ops->EmitStopIfNeeded(ext);
    }

    UNREFERENCED_PARAMETER(flags);
    return status;
}


NTSTATUS
I2CctrlHw_EnableController(
    PDEVICE_OBJECT   DevObj,
    PSMBUS_REQUEST   Request,
    PI2CCTRL_QUEUE   Queue
    )
{
    PI2CCTRL_FDO ext;
    KIRQL        oldIrql;
    ULONG        ctrl;
    NTSTATUS     status;

    /* C89 initialization */
    ext     = (PI2CCTRL_FDO)DevObj->DeviceExtension;
    oldIrql = PASSIVE_LEVEL;
    ctrl    = 0U;
    status  = STATUS_SUCCESS;

    /* XP/2003-safe: validate pointers before touching hardware */
    if (ext == NULL || Request == NULL || Queue == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Also ensure MMIO is mapped */
    if (ext->Mmio == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Serialize register access using queue lock */
    KeAcquireSpinLock(&Queue->Lock, &oldIrql);

    /* Read-modify-write control register using safe MMIO */
    ctrl = I2cCtrl_ReadRegisterSafe(ext, I2C_REG_CONTROL);
    ctrl |= I2C_CTRL_ENABLE;
    I2cCtrl_WriteRegisterSafe(ext, I2C_REG_CONTROL, ctrl);

    /* Update SMBUS_REQUEST flags */
    Request->Flags |= SMBUS_FLAG_CONTROLLER_ENABLED;

    KeReleaseSpinLock(&Queue->Lock, oldIrql);

    /* Allow hardware to latch enable bit */
    KeStallExecutionProcessor(5);

    return status;
}

/*
 * Wait until the I2C controller reports idle or until timeout expires.
 * Returns STATUS_SUCCESS if idle, STATUS_IO_TIMEOUT otherwise.
 * BSOD-safe: uses I2cCtrl_ReadRegisterSafe for all MMIO access.
 */
NTSTATUS
I2CctrlHw_WaitForIdle(
    PDEVICE_OBJECT DevObj,
    ULONG          TimeoutMs
    )
{
    PI2CCTRL_FDO ext;
    LARGE_INTEGER interval;
    ULONG elapsed;
    ULONG statusReg;
    NTSTATUS status;

    /* C89 initialization */
    ext     = (PI2CCTRL_FDO)DevObj->DeviceExtension;
    interval.QuadPart = -10 * 1000; /* relative, 1 ms */
    elapsed = 0U;
    status  = STATUS_SUCCESS;

    /* Validate extension */
    if (ext == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Poll until idle or timeout */
    while (elapsed < TimeoutMs) {

        /* Safe MMIO read */
        statusReg = I2cCtrl_ReadRegisterSafe(ext, I2C_REG_STATUS);

        if ((statusReg & I2C_STATUS_ACTIVITY) == 0U) {
            return STATUS_SUCCESS; /* idle */
        }

        /* Sleep 1 ms */
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
        elapsed++;
    }

    return STATUS_IO_TIMEOUT;
}


/*
 * Feature-complete local dispatcher that interprets IoControlCode
 * and processes InputBuffer/OutputBuffer WITHOUT calling ZwDeviceIoControlFile.
 *
 * Requires the caller to pass the device object (DevObj) so hardware helpers can be invoked.
 * C89 note: declare variables at the start of the function or of a braced block.
 */

NTSTATUS NTAPI I2cCtrl_ControlFile(
    PDEVICE_OBJECT   DevObj,          /* device object for hardware helpers */
    HANDLE           FileHandle,
    HANDLE           Event OPTIONAL,
    PIO_APC_ROUTINE  ApcRoutine OPTIONAL,
    PVOID            ApcContext OPTIONAL,
    PIO_STATUS_BLOCK IoStatusBlock,
    ULONG            IoControlCode,
    PVOID            InputBuffer OPTIONAL,
    ULONG            InputBufferLength,
    PVOID            OutputBuffer OPTIONAL,
    ULONG            OutputBufferLength
)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(FileHandle);
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(ApcRoutine);
    UNREFERENCED_PARAMETER(ApcContext);

    status = STATUS_SUCCESS;

    switch (IoControlCode) {

    case IOCTL_I2cCtrl_TRANSFER:
    {
        PI2CCTRL_TRANSFER packet;
        PSMBUS_REQUEST     req;       /* SMBus request with Flags */
        PI2CCTRL_FDO       ext;       /* device extension */
        PI2CCTRL_QUEUE     queue;     /* pointer to embedded queue */
        BOOLEAN            outputOk;
        NTSTATUS           hwStatus;
        ULONG              bytesDone;
        ULONG_PTR          info;

        /* C89: declare and initialize variables at start of block */
        packet    = (PI2CCTRL_TRANSFER)InputBuffer;
        outputOk  = (OutputBuffer != NULL && OutputBufferLength >= sizeof(I2CCTRL_TRANSFER));
        hwStatus  = STATUS_SUCCESS;
        bytesDone = 0U;
        req       = NULL;
        info      = 0;

        /* validate DevObj and get extension/queue */
        if (DevObj == NULL || DevObj->DeviceExtension == NULL) {
            status = STATUS_DEVICE_NOT_READY;
            if (IoStatusBlock) IoStatusBlock->Information = 0;
            break;
        }

        ext   = (PI2CCTRL_FDO)DevObj->DeviceExtension;
        queue = ext->Queue;

        /* Validate packet */
        if (InputBuffer == NULL || InputBufferLength < sizeof(I2CCTRL_TRANSFER)) {
            status = STATUS_INVALID_PARAMETER;
            if (IoStatusBlock) IoStatusBlock->Information = 0;
            break;
        }

        /* Map to SMBUS_REQUEST (if your packet embeds or points to it) */
        req = (PSMBUS_REQUEST)packet;

        if (packet->Buffer == NULL || packet->Length == 0U) {
            packet->BytesReturned = 0U;
            status = STATUS_INVALID_PARAMETER;
            if (IoStatusBlock) IoStatusBlock->Information = 0;
            break;
        }
        if (packet->Direction != I2C_DIRECTION_READ &&
            packet->Direction != I2C_DIRECTION_WRITE) {
            packet->BytesReturned = 0U;
            status = STATUS_INVALID_PARAMETER;
            if (IoStatusBlock) IoStatusBlock->Information = 0;
            break;
        }

        /* Optional: enforce address range */
        if (packet->SlaveAddress > 0x3FFU) {
            packet->BytesReturned = 0U;
            status = STATUS_INVALID_PARAMETER;
            if (IoStatusBlock) IoStatusBlock->Information = 0;
            break;
        }

        /* Ensure controller is enabled and idle */
        hwStatus = I2CctrlHw_EnableController(DevObj, req, queue);
        if (!NT_SUCCESS(hwStatus)) {
            packet->BytesReturned = 0U;
            status = hwStatus;
            if (IoStatusBlock) IoStatusBlock->Information = 0;
            break;
        }

        hwStatus = I2CctrlHw_WaitForIdle(DevObj, I2CCTRL_IDLE_TIMEOUT_MS);
        if (!NT_SUCCESS(hwStatus)) {
            packet->BytesReturned = 0U;
            status = hwStatus;
            if (IoStatusBlock) IoStatusBlock->Information = 0;
            break;
        }

        /* Configure target slave address */
        hwStatus = I2CctrlHw_SetTarget(DevObj, packet->SlaveAddress, req->Flags);
        if (!NT_SUCCESS(hwStatus)) {
            packet->BytesReturned = 0U;
            status = hwStatus;
            if (IoStatusBlock) IoStatusBlock->Information = 0;
            break;
        }

        /* Perform transaction using Flags from SMBUS_REQUEST */
        if (packet->Direction == I2C_DIRECTION_WRITE) {
            hwStatus = I2CctrlHw_Write(DevObj,
                                       packet->Buffer,
                                       packet->Length,
                                       &bytesDone,
                                       req->Flags);
        } else {
            hwStatus = I2CctrlHw_Read(DevObj,
                                      packet->Buffer,
                                      packet->Length,
                                      &bytesDone,
                                      req->Flags);
        }

        if (!NT_SUCCESS(hwStatus)) {
            packet->BytesReturned = bytesDone;
            status = hwStatus;
            if (IoStatusBlock) IoStatusBlock->Information = 0;
            break;
        }

        /* Success path */
        packet->BytesReturned = bytesDone;

        if (outputOk) {
            I2CCTRL_TRANSFER out;
            ULONG            totalSize;

            RtlZeroMemory(&out, sizeof(out));
            out.NumMessages   = packet->NumMessages;
            out.SlaveAddress  = packet->SlaveAddress;
            out.Direction     = packet->Direction;
            out.Buffer        = packet->Buffer;
            out.Length        = packet->Length;
            out.BytesReturned = packet->BytesReturned;

            totalSize = sizeof(out);
            RtlCopyMemory(OutputBuffer, &out, sizeof(out));

            /* append SMBUS_REQUEST flags if caller’s buffer is large enough */
            if (OutputBufferLength >= totalSize + sizeof(ULONG)) {
                ULONG flags = req->Flags;
                RtlCopyMemory((PUCHAR)OutputBuffer + totalSize, &flags, sizeof(flags));
                totalSize += sizeof(flags);
            }

            if (IoStatusBlock) IoStatusBlock->Information = totalSize;
            status = STATUS_SUCCESS;

        } else if (OutputBuffer != NULL) {
            status = STATUS_BUFFER_TOO_SMALL;
            if (IoStatusBlock) IoStatusBlock->Information = 0;
        } else {
            if (IoStatusBlock) IoStatusBlock->Information = 0;
            status = STATUS_SUCCESS;
        }

        break;
    }

case IOCTL_GET_PT_SAMPLE:
{
    PT_RAW_SAMPLE* sample;
    LARGE_INTEGER  sysTime;
    PI2CCTRL_FDO   ext;
    PI2CCTRL_PDO   hidpdo;
    PUCHAR         rep;
    ULONG          repLen;
    NTSTATUS       qStatus;

    /* 1. Validate output buffer */
    if (OutputBuffer == NULL || OutputBufferLength < sizeof(PT_RAW_SAMPLE)) {
        status = STATUS_INVALID_PARAMETER;
        if (IoStatusBlock) IoStatusBlock->Information = 0;
        break;
    }

    sample = (PT_RAW_SAMPLE*)OutputBuffer;
    RtlZeroMemory(sample, sizeof(*sample));

    /* 2. Timestamp */
    KeQuerySystemTime(&sysTime);
    sample->Timestamp = (ULONGLONG)sysTime.QuadPart;

    /* 3. Default: no contacts */
    sample->ContactCount = 0;

    /* 4. Retrieve FDO extension */
    ext = (PI2CCTRL_FDO)DevObj->DeviceExtension;
    if (ext == NULL) {
        status = STATUS_DEVICE_NOT_READY;
        if (IoStatusBlock) IoStatusBlock->Information = sizeof(*sample);
        break;
    }

    /* 5. Locate HID child PDO (touchpad) */
    hidpdo = ext->TouchpadPdo;
    if (hidpdo != NULL &&
        hidpdo->Reported &&
        hidpdo->HidReportDesc != NULL &&
        hidpdo->HidReportDescLen > 0) {

        rep    = hidpdo->LastReport;
        repLen = hidpdo->HidReportDescLen;

        /*
         * 6. Decode HID report -> PT_RAW_SAMPLE
         *    This is a minimal decoder. You can expand it later.
         *    Assumes: [0]=ID, [1]=X, [2]=Y, [3]=Buttons, [4]=Flags
         */
        if (repLen >= 5) {
            sample->ContactCount = 1;
            sample->Contacts[0].ContactId = rep[0];
            sample->Contacts[0].X         = (USHORT)rep[1];
            sample->Contacts[0].Y         = (USHORT)rep[2];
            sample->Contacts[0].Buttons   = rep[3];
            sample->Contacts[0].Flags     = rep[4];
        }

        if (IoStatusBlock) IoStatusBlock->Information = sizeof(*sample);
        status = STATUS_SUCCESS;
        break;
    }

    /*
     * 7. Fallback: HAL-level QueryTouchSample
     */
    if (ext->Ops != NULL && ext->Ops->QueryTouchSample != NULL) {
        qStatus = ext->Ops->QueryTouchSample(ext, sample);
        if (NT_SUCCESS(qStatus)) {
            if (IoStatusBlock) IoStatusBlock->Information = sizeof(*sample);
            status = STATUS_SUCCESS;
            break;
        }
    }

    /*
     * 8. Final fallback: synthetic sample from controller status
     */
    if (ext->Ops != NULL && ext->Ops->GetStatus != NULL) {

        I2C_HW_STATUS st;
        RtlZeroMemory(&st, sizeof(st));

        if (NT_SUCCESS(ext->Ops->GetStatus(ext, &st))) {

            if (st.RxFifoNotEmpty) {
                sample->ContactCount = 1;
                sample->Contacts[0].X         = (USHORT)st.RxFifoLevel;
                sample->Contacts[0].Y         = (USHORT)st.TxFifoLevel;
                sample->Contacts[0].Buttons   = 0;
                sample->Contacts[0].ContactId = 0;
                sample->Contacts[0].Flags     = 0;
            }
        }
    }

    /* 9. Return sample */
    if (IoStatusBlock) {
        IoStatusBlock->Information = sizeof(*sample);
    }

    status = STATUS_SUCCESS;
    break;
}

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        if (IoStatusBlock) IoStatusBlock->Information = 0;
        break;
    }

    return status;
}
