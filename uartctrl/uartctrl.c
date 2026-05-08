/* -----------------------------------------------------------------------
   uartctrl.c – DriverEntry and dispatch setup for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl.h"
#include "uartctrl_ioctl.h"
#include "uartctrl_ext.h"

/* Forward declarations */
DRIVER_UNLOAD  UARTCTRL_DriverUnload;
DRIVER_ADD_DEVICE UARTCTRL_AddDevice;

/* -----------------------------------------------------------------------
   Create/Close dispatch – track open count
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_DispatchCreateClose(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PUARTCTRL_DEVEXT ext;
    PIO_STACK_LOCATION isl;

    ext = (PUARTCTRL_DEVEXT)DevObj->DeviceExtension;
    isl = IoGetCurrentIrpStackLocation(Irp);

    if (isl->MajorFunction == IRP_MJ_CREATE) {
        InterlockedIncrement(&ext->OpenCount);
    } else if (isl->MajorFunction == IRP_MJ_CLOSE) {
        InterlockedDecrement(&ext->OpenCount);
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

/* -----------------------------------------------------------------------
   UARTCTRL_ReadCancelRoutine – cancel routine for queued read IRPs
   ----------------------------------------------------------------------- */
VOID
UARTCTRL_ReadCancelRoutine(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PUARTCTRL_DEVEXT ext;
    KIRQL irql;
    PLIST_ENTRY le;

    UNREFERENCED_PARAMETER(DevObj);

    ext = (PUARTCTRL_DEVEXT)DevObj->DeviceExtension;

    // Release cancel spinlock first
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    // Remove IRP from ReadQueue if present
    KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
    for (le = ext->ReadQueue.Flink; le != &ext->ReadQueue; le = le->Flink) {
        PIRP queued = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);
        if (queued == Irp) {
            RemoveEntryList(le);
            Irp->IoStatus.Status = STATUS_CANCELLED;
            Irp->IoStatus.Information = 0;
            KeReleaseSpinLock(&ext->ReadQueueLock, irql);
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return;
        }
    }
    KeReleaseSpinLock(&ext->ReadQueueLock, irql);
}


/* -----------------------------------------------------------------------
   Read dispatch – complete immediately if data available, else queue IRP
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_DispatchRead(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PUARTCTRL_DEVEXT ext = (PUARTCTRL_DEVEXT)DevObj->DeviceExtension;
    PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(Irp);
    PUCHAR outBuf;
    ULONG outLen, copied = 0;
    KIRQL irql;

    // Map user buffer
    if (Irp->MdlAddress) {
        outBuf = (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
        if (!outBuf) {
            Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    } else {
        outBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    }
    outLen = isl->Parameters.Read.Length;

    // Try to copy from RX ring
    KeAcquireSpinLock(&ext->RxLock, &irql);
    while (copied < outLen) {
        UCHAR v;
        if (!RingGet(ext->RxBuf, ext->RxSize, &ext->RxHead, &ext->RxTail, &v)) {
            break; // no more data
        }
        outBuf[copied++] = v;
    }
    KeReleaseSpinLock(&ext->RxLock, irql);

    if (copied > 0) {
        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = copied;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_SUCCESS;
    }

    // No data: queue IRP
    IoMarkIrpPending(Irp);
    IoSetCancelRoutine(Irp, UARTCTRL_ReadCancelRoutine);

    KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
    InsertTailList(&ext->ReadQueue, &Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&ext->ReadQueueLock, irql);

    return STATUS_PENDING;
}


/* -----------------------------------------------------------------------
   Write dispatch – copy to TX ring and kick transmitter
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_DispatchWrite(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PUARTCTRL_DEVEXT ext = (PUARTCTRL_DEVEXT)DevObj->DeviceExtension;
    PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(Irp);
    PUCHAR inBuf;
    ULONG inLen, written = 0;
    KIRQL irql;

    // Map user buffer
    if (Irp->MdlAddress) {
        inBuf = (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
        if (!inBuf) {
            Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    } else {
        inBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    }
    inLen = isl->Parameters.Write.Length;

    // Copy into TX ring
    KeAcquireSpinLock(&ext->TxLock, &irql);
    while (written < inLen) {
        if (!RingPut(ext->TxBuf, ext->TxSize, &ext->TxHead, &ext->TxTail, inBuf[written])) {
            break; // ring full
        }
        written++;
    }
    KeReleaseSpinLock(&ext->TxLock, irql);

    // Enable TX interrupts so ISR/DPC drains the ring
    if (written > 0) {
        UartEnableInterrupts(ext, IER_THRE | IER_RDA | IER_RLS);
    }

    Irp->IoStatus.Status = (written > 0) ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
    Irp->IoStatus.Information = written;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Irp->IoStatus.Status;
}



/* -----------------------------------------------------------------------
   DriverEntry – initialize driver object and dispatch table
   ----------------------------------------------------------------------- */
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    /* Register unload and AddDevice routines */
    DriverObject->DriverUnload             = UARTCTRL_DriverUnload;
    DriverObject->DriverExtension->AddDevice = UARTCTRL_AddDevice;

    /* Default all major functions to pass‑through */
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = UARTCTRL_PT_DispatchPass;
    }

    /* Core dispatch routines */
    DriverObject->MajorFunction[IRP_MJ_CREATE]  = UARTCTRL_DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]   = UARTCTRL_DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_READ]    = UARTCTRL_DispatchRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE]   = UARTCTRL_DispatchWrite;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]          = UARTCTRL_DispatchIoctl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = UARTCTRL_DispatchIoctl;
    DriverObject->MajorFunction[IRP_MJ_PNP]     = UARTCTRL_DispatchPnP;
    DriverObject->MajorFunction[IRP_MJ_POWER]   = UARTCTRL_DispatchPower;

    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
   DriverUnload – cleanup when driver is unloaded
   ----------------------------------------------------------------------- */
VOID
UARTCTRL_DriverUnload(PDRIVER_OBJECT DriverObject)
{
    PDEVICE_OBJECT devObj = DriverObject->DeviceObject;

    /* Walk all device objects created by this driver */
    while (devObj) {
        PDEVICE_OBJECT nextDev = devObj->NextDevice;

        /* Call full remove routine for each device */
        UARTCTRL_RemoveDevice(devObj);

        devObj = nextDev;
    }

    /* No global resources to free; per‑device cleanup handled above */
}

/* -----------------------------------------------------------------------
   Supported ACPI Hardware IDs for UARTCTRL
   ----------------------------------------------------------------------- */
static const PWSTR UartCtrlHardwareIds[] = {
    L"ACPI\\INT33C4",
    L"ACPI\\INT33C5",
    L"ACPI\\INT3434",
    L"ACPI\\INT3435",
    L"ACPI\\INTC1029",
    NULL   // terminator
};

/* -----------------------------------------------------------------------
   AddDevice – create FDO and attach to stack
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_AddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    NTSTATUS status;
    PDEVICE_OBJECT fdo;
    PWSTR hwidBuffer;
    ULONG hwidLength;
    ULONG i;

    // Query hardware IDs from the PDO (REG_MULTI_SZ)
    status = IoGetDeviceProperty(PhysicalDeviceObject,
                                 DevicePropertyHardwareID,
                                 0,
                                 NULL,
                                 &hwidLength);
    if (status != STATUS_BUFFER_TOO_SMALL) {
        return status;
    }

    hwidBuffer = (PWSTR)ExAllocatePoolWithTag(NonPagedPool, hwidLength, 'hdiU');
    if (!hwidBuffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoGetDeviceProperty(PhysicalDeviceObject,
                                 DevicePropertyHardwareID,
                                 hwidLength,
                                 hwidBuffer,
                                 &hwidLength);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(hwidBuffer, 'hdiU');
        return status;
    }

    // Compare against our supported list
    for (i = 0; UartCtrlHardwareIds[i] != NULL; i++) {
        UNICODE_STRING idStr, targetStr;
        RtlInitUnicodeString(&idStr, UartCtrlHardwareIds[i]);
        RtlInitUnicodeString(&targetStr, hwidBuffer);

        if (RtlEqualUnicodeString(&idStr, &targetStr, TRUE)) {
            status = IoCreateDevice(DriverObject,
                                    sizeof(UARTCTRL_DEVEXT),
                                    NULL,
                                    FILE_DEVICE_SERIAL_PORT,
                                    0,
                                    FALSE,
                                    &fdo);
            ExFreePoolWithTag(hwidBuffer, 'hdiU');
            return status;
        }
    }

    ExFreePoolWithTag(hwidBuffer, 'hdiU');
    return STATUS_NO_SUCH_DEVICE;
}

/* -----------------------------------------------------------------------
   UARTCTRL_StartDevice – parse resources, map MMIO, connect ISR, init hardware
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_StartDevice(PUARTCTRL_DEVEXT ext,
                     PCM_RESOURCE_LIST raw,
                     PCM_RESOURCE_LIST translated)
{
    PCM_PARTIAL_RESOURCE_LIST prl;
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(raw);

    if (translated == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    prl = &translated->List[0].PartialResourceList;

    for (i = 0; i < prl->Count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR prd = &prl->PartialDescriptors[i];

        switch (prd->Type) {
        case CmResourceTypeMemory:
            ext->MmioBase = MmMapIoSpace(prd->u.Memory.Start,
                                         prd->u.Memory.Length,
                                         MmNonCached);
            if (!ext->MmioBase) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            ext->MmioLength = prd->u.Memory.Length;
            break;

        case CmResourceTypeInterrupt:
        {
            KIRQL irql = (KIRQL)prd->u.Interrupt.Level;
            KINTERRUPT_MODE mode =
                (prd->Flags & CM_RESOURCE_INTERRUPT_LATCHED) ? Latched : LevelSensitive;

            status = IoConnectInterrupt(&ext->InterruptObject,
                                        UARTCTRL_InterruptServiceRoutine,
                                        ext,
                                        NULL,
                                        prd->u.Interrupt.Vector,
                                        irql,
                                        irql,
                                        mode,
                                        TRUE,
                                        prd->u.Interrupt.Affinity,
                                        FALSE);
            if (!NT_SUCCESS(status)) {
                return status;
            }

            ext->InterruptConnected = TRUE;

            /* Initialize DPC for deferred work */
            KeInitializeDpc(&ext->PollDpc, UARTCTRL_DpcRoutine, ext);

            /* Initialize timer for optional polling fallback */
            KeInitializeTimer(&ext->PollTimer);
            ext->Polling = FALSE;
            break;
        }

        default:
            break;
        }
    }

    /* Initialize spin locks and queues */
    KeInitializeSpinLock(&ext->RxLock);
    KeInitializeSpinLock(&ext->TxLock);
    KeInitializeSpinLock(&ext->ReadQueueLock);
    InitializeListHead(&ext->ReadQueue);

    /* Allocate RX/TX buffers */
    status = UARTCTRL_ExtAllocateBuffers(ext, 4096, 2048);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Reset counters and state */
    ext->RxErrors = 0;
    ext->TxErrors = 0;
    ext->OpenCount = 0;
    ext->Started = FALSE;
    ext->Removed = FALSE;

    /* Default UART configuration */
    ext->Config.BaudRate = 115200;
    ext->Config.DataBits = 8;
    ext->Config.StopBits = 1;
    ext->Config.Parity   = 0;

    /* Program hardware */
    UartEnableFifo(ext, FCR_TRIG_8);
    UartSetLineControl(ext,
                       ext->Config.DataBits,
                       ext->Config.StopBits,
                       ext->Config.Parity);
    UartSetBaud(ext, ext->ClockHz, ext->Config.BaudRate);
    UartSetModemControl(ext, MCR_RTS | MCR_OUT2);
    UartEnableInterrupts(ext, IER_RDA | IER_THRE | IER_RLS);

    ext->Started = TRUE;
    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
   UARTCTRL_StopDevice – disconnect ISR, free buffers, unmap MMIO, flush queues
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_StopDevice(PUARTCTRL_DEVEXT ext)
{
    KIRQL irql;

    /* Cancel polling timer if active */
    if (ext->Polling) {
        KeCancelTimer(&ext->PollTimer);
        ext->Polling = FALSE;
    }

    /* Disconnect interrupt if connected */
    if (ext->InterruptConnected) {
        IoDisconnectInterrupt(ext->InterruptObject);
        ext->InterruptObject = NULL;
        ext->InterruptConnected = FALSE;
    }

    /* Unmap MMIO region if mapped */
    if (ext->MmioBase) {
        MmUnmapIoSpace(ext->MmioBase, ext->MmioLength);
        ext->MmioBase = NULL;
        ext->MmioLength = 0;
    }

    /* Flush and complete any queued read IRPs */
    KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
    while (!IsListEmpty(&ext->ReadQueue)) {
        PLIST_ENTRY le = RemoveHeadList(&ext->ReadQueue);
        PIRP irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);

        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        KeReleaseSpinLock(&ext->ReadQueueLock, irql);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
    }
    KeReleaseSpinLock(&ext->ReadQueueLock, irql);

    /* Free RX/TX buffers */
    UARTCTRL_ExtFreeBuffers(ext);

    /* Reset counters and flags */
    ext->RxErrors = 0;
    ext->TxErrors = 0;
    ext->OpenCount = 0;
    ext->Started = FALSE;

    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
   UARTCTRL_RemoveDevice – full cleanup on device removal
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_RemoveDevice(PDEVICE_OBJECT DevObj)
{
    PUARTCTRL_DEVEXT ext;
    KIRQL irql;

    ext = (PUARTCTRL_DEVEXT)DevObj->DeviceExtension;

    /* Mark state */
    ext->Removed = TRUE;
    ext->Started = FALSE;

    /* Stop hardware and free buffers */
    UARTCTRL_StopDevice(ext);

    /* Cancel any queued read IRPs */
    KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
    while (!IsListEmpty(&ext->ReadQueue)) {
        PLIST_ENTRY le = RemoveHeadList(&ext->ReadQueue);
        PIRP irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);

        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        KeReleaseSpinLock(&ext->ReadQueueLock, irql);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
    }
    KeReleaseSpinLock(&ext->ReadQueueLock, irql);

    /* Delete symbolic link if present */
    if (ext->Symlink.Buffer) {
        IoDeleteSymbolicLink(&ext->Symlink);
        RtlZeroMemory(&ext->Symlink, sizeof(ext->Symlink));
    }

    /* Detach from lower device */
    if (ext->LowerDevice) {
        IoDetachDevice(ext->LowerDevice);
        ext->LowerDevice = NULL;
    }

    /* Release remove lock and wait for outstanding I/O */
    IoReleaseRemoveLockAndWait(&ext->RemoveLock, NULL);

    /* Finally delete our device object */
    IoDeleteDevice(DevObj);

    return STATUS_SUCCESS;
}
