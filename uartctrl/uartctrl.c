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

/* ---------------------------------------------------------------------------
   Global driver context definition
   --------------------------------------------------------------------------- */
UARTCTRL_GLOBAL g_UartCtrlGlobal = {0};

/* -----------------------------------------------------------------------
   Create/Close dispatch – track open count
   ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_DispatchCreateClose(PDEVICE_OBJECT DevObj, PIRP Irp)
{
    PUARTCTRL_FDO ext;
    PIO_STACK_LOCATION isl;

    ext = (PUARTCTRL_FDO)DevObj->DeviceExtension;
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
    PUARTCTRL_FDO ext;
    KIRQL irql;
    PLIST_ENTRY le;

    UNREFERENCED_PARAMETER(DevObj);

    ext = (PUARTCTRL_FDO)DevObj->DeviceExtension;

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
    PUARTCTRL_FDO ext = (PUARTCTRL_FDO)DevObj->DeviceExtension;
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
    PUARTCTRL_FDO ext = (PUARTCTRL_FDO)DevObj->DeviceExtension;
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
 * DriverEntry – UART Controller driver entry point (WDM, XP/2003-safe)
 * ----------------------------------------------------------------------- */
NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT  DriverObject,
    IN PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    ULONG    i;

    /* C89 initialization */
    status = STATUS_SUCCESS;
    i      = 0U;

    UNREFERENCED_PARAMETER(RegistryPath);

    /* Must run at PASSIVE_LEVEL */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (DriverObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Initialize any global UART state here if you have it */
    RtlZeroMemory(&g_UartCtrlGlobal, sizeof(g_UartCtrlGlobal));

    /* Default all IRP major functions to a safe pass-through handler */
    for (i = 0U; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = UARTCTRL_PT_DispatchPass;
    }

    /* Core dispatch routines */
    DriverObject->MajorFunction[IRP_MJ_CREATE]                 = UARTCTRL_DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]                  = UARTCTRL_DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_READ]                   = UARTCTRL_DispatchRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE]                  = UARTCTRL_DispatchWrite;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL]         = UARTCTRL_DispatchIoctl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL]= UARTCTRL_DispatchIoctl;
    DriverObject->MajorFunction[IRP_MJ_PNP]                    = UARTCTRL_DispatchPnP;
    DriverObject->MajorFunction[IRP_MJ_POWER]                  = UARTCTRL_DispatchPower;

    /* Set unload routine early */
    DriverObject->DriverUnload = UARTCTRL_DriverUnload;

    /* AddDevice must be available */
    if (DriverObject->DriverExtension == NULL) {
        return STATUS_UNSUCCESSFUL;
    }

    /* Assign AddDevice (PnP entry point) */
    DriverObject->DriverExtension->AddDevice = UARTCTRL_AddDevice;

    /* Optional: register lifecycle helpers in a global struct, if you have them */
    g_UartCtrlGlobal.StartDevice = UARTCTRL_StartDevice;
    g_UartCtrlGlobal.StopDevice  = UARTCTRL_StopDevice;

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
 * AddDevice – create FDO, attach to PDO, initialize PDO extension
 * ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_AddDevice(
    PDRIVER_OBJECT  DriverObject,
    PDEVICE_OBJECT  PhysicalDeviceObject
    )
{
    NTSTATUS        status;
    PDEVICE_OBJECT  fdo;
    PUARTCTRL_FDO   fdoExt;
    PUARTCTRL_PDO   pdoExt;
    PWSTR           hwidBuffer;
    ULONG           hwidLength;
    PWSTR           p;
    ULONG           idIndex;

    UartCtrl_Log("AddDevice: PDO=%p\n", PhysicalDeviceObject);

    /* -------------------------------------------------------------
     * Get PDO extension (we now use PUARTCTRL_PDO)
     * ------------------------------------------------------------- */
    pdoExt = (PUARTCTRL_PDO)PhysicalDeviceObject->DeviceExtension;

    /* PDO may not be ours yet — initialize minimal fields */
    if (pdoExt->Self == NULL) {
        pdoExt->Self      = PhysicalDeviceObject;
        pdoExt->ParentFdo = NULL;
        pdoExt->Present   = TRUE;
        pdoExt->Removed   = FALSE;
        pdoExt->Started   = FALSE;
        IoInitializeRemoveLock(&pdoExt->RemoveLock, 'URTP', 0, 0);
        UartCtrl_Log("AddDevice: initialized PDO extension\n");
    }

    /* -------------------------------------------------------------
     * Query required buffer size for hardware IDs
     * ------------------------------------------------------------- */
    status = IoGetDeviceProperty(
                 PhysicalDeviceObject,
                 DevicePropertyHardwareID,
                 0,
                 NULL,
                 &hwidLength);

    if (status != STATUS_BUFFER_TOO_SMALL) {
        UartCtrl_Log("AddDevice: IoGetDeviceProperty(size) failed (0x%08lx)\n", status);
        return status;
    }

    hwidBuffer = (PWSTR)ExAllocatePoolWithTag(NonPagedPool, hwidLength, 'hdiU');
    if (hwidBuffer == NULL) {
        UartCtrl_Log("AddDevice: failed to allocate HWID buffer\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* -------------------------------------------------------------
     * Retrieve the actual REG_MULTI_SZ hardware ID list
     * ------------------------------------------------------------- */
    status = IoGetDeviceProperty(
                 PhysicalDeviceObject,
                 DevicePropertyHardwareID,
                 hwidLength,
                 hwidBuffer,
                 &hwidLength);

    if (!NT_SUCCESS(status)) {
        UartCtrl_Log("AddDevice: IoGetDeviceProperty(data) failed (0x%08lx)\n", status);
        ExFreePoolWithTag(hwidBuffer, 'hdiU');
        return status;
    }

    /* -------------------------------------------------------------
     * REG_MULTI_SZ: iterate through each null‑terminated string
     * ------------------------------------------------------------- */
    p = hwidBuffer;

    while (*p != UNICODE_NULL) {

        UNICODE_STRING hwid;
        RtlInitUnicodeString(&hwid, p);

        UartCtrl_Log("AddDevice: checking HWID '%ws'\n", p);

        /* Compare against our supported list */
        for (idIndex = 0; UartCtrlHardwareIds[idIndex] != NULL; idIndex++) {

            UNICODE_STRING target;
            RtlInitUnicodeString(&target, UartCtrlHardwareIds[idIndex]);

            if (RtlEqualUnicodeString(&hwid, &target, TRUE)) {

                UartCtrl_Log("AddDevice: matched supported HWID '%ws'\n",
                             UartCtrlHardwareIds[idIndex]);

                /* -----------------------------------------------------
                 * Create FDO
                 * ----------------------------------------------------- */
                status = IoCreateDevice(
                             DriverObject,
                             sizeof(UARTCTRL_FDO),
                             NULL,
                             FILE_DEVICE_SERIAL_PORT,
                             0,
                             FALSE,
                             &fdo);

                if (!NT_SUCCESS(status)) {
                    UartCtrl_Log("AddDevice: IoCreateDevice FAILED (0x%08lx)\n", status);
                    ExFreePoolWithTag(hwidBuffer, 'hdiU');
                    return status;
                }

                fdoExt = (PUARTCTRL_FDO)fdo->DeviceExtension;
                RtlZeroMemory(fdoExt, sizeof(UARTCTRL_FDO));

                /* -----------------------------------------------------
                 * Attach to device stack
                 * ----------------------------------------------------- */
                fdoExt->LowerDevice = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);

                if (fdoExt->LowerDevice == NULL) {
                    UartCtrl_Log("AddDevice: IoAttachDeviceToDeviceStack FAILED\n");
                    IoDeleteDevice(fdo);
                    ExFreePoolWithTag(hwidBuffer, 'hdiU');
                    return STATUS_NO_SUCH_DEVICE;
                }

                /* -----------------------------------------------------
                 * Initialize remove lock
                 * ----------------------------------------------------- */
                IoInitializeRemoveLock(&fdoExt->RemoveLock, 'URTF', 0, 0);

                /* -----------------------------------------------------
                 * Link PDO <-> FDO
                 * ----------------------------------------------------- */
                pdoExt->ParentFdo = fdo;
                fdoExt->Started   = FALSE;
                fdoExt->Removed   = FALSE;

                /* -----------------------------------------------------
                 * Initialize FDO flags
                 * ----------------------------------------------------- */
                fdo->Flags |= DO_POWER_PAGABLE;
                fdo->Flags &= ~DO_DEVICE_INITIALIZING;

                UartCtrl_Log("AddDevice: FDO created and attached successfully\n");

                ExFreePoolWithTag(hwidBuffer, 'hdiU');
                return STATUS_SUCCESS;
            }
        }

        /* Move to next string in REG_MULTI_SZ */
        p += wcslen(p) + 1;
    }

    UartCtrl_Log("AddDevice: no supported HWID matched\n");

    ExFreePoolWithTag(hwidBuffer, 'hdiU');
    return STATUS_NO_SUCH_DEVICE;
}

/* -----------------------------------------------------------------------
 * UARTCTRL_StartDevice – parse resources, map MMIO, connect ISR, init HW
 * ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_StartDevice(
    PUARTCTRL_FDO        ext,
    PCM_RESOURCE_LIST    raw,
    PCM_RESOURCE_LIST    translated
    )
{
    PCM_PARTIAL_RESOURCE_LIST prl;
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(raw);

    UartCtrl_Log("StartDevice: begin\n");

    if (translated == NULL) {
        UartCtrl_Log("StartDevice: translated resource list is NULL\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    prl = &translated->List[0].PartialResourceList;

    UartCtrl_Log("StartDevice: parsing %lu resources\n", prl->Count);

    /* -------------------------------------------------------------
     * Parse resources
     * ------------------------------------------------------------- */
    for (i = 0; i < prl->Count; i++) {

        PCM_PARTIAL_RESOURCE_DESCRIPTOR prd =
            &prl->PartialDescriptors[i];

        switch (prd->Type) {

        /* ---------------------------------------------------------
         * MMIO resource
         * --------------------------------------------------------- */
        case CmResourceTypeMemory:

            UartCtrl_Log("StartDevice: MMIO @ %08lx%08lx len=%lu\n",
                         prd->u.Memory.Start.HighPart,
                         prd->u.Memory.Start.LowPart,
                         prd->u.Memory.Length);

            ext->MmioBase = MmMapIoSpace(
                                prd->u.Memory.Start,
                                prd->u.Memory.Length,
                                MmNonCached);

            if (ext->MmioBase == NULL) {
                UartCtrl_Log("StartDevice: MmMapIoSpace FAILED\n");
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            ext->MmioLength = prd->u.Memory.Length;
            break;

        /* ---------------------------------------------------------
         * Interrupt resource
         * --------------------------------------------------------- */
        case CmResourceTypeInterrupt:
        {
            KIRQL irql = (KIRQL)prd->u.Interrupt.Level;
            KINTERRUPT_MODE mode =
                (prd->Flags & CM_RESOURCE_INTERRUPT_LATCHED)
                    ? Latched
                    : LevelSensitive;

            UartCtrl_Log("StartDevice: IRQ vector=%lu level=%lu mode=%s\n",
                         prd->u.Interrupt.Vector,
                         prd->u.Interrupt.Level,
                         (mode == Latched) ? "Latched" : "LevelSensitive");

            status = IoConnectInterrupt(
                         &ext->InterruptObject,
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
                UartCtrl_Log("StartDevice: IoConnectInterrupt FAILED (0x%08lx)\n",
                             status);
                return status;
            }

            ext->InterruptConnected = TRUE;

            /* Initialize DPC + timer */
            KeInitializeDpc(&ext->PollDpc, UARTCTRL_DpcRoutine, ext);
            KeInitializeTimer(&ext->PollTimer);
            ext->Polling = FALSE;

            break;
        }

        default:
            break;
        }
    }

    /* -------------------------------------------------------------
     * Initialize locks and queues
     * ------------------------------------------------------------- */
    KeInitializeSpinLock(&ext->RxLock);
    KeInitializeSpinLock(&ext->TxLock);
    KeInitializeSpinLock(&ext->ReadQueueLock);
    InitializeListHead(&ext->ReadQueue);

    UartCtrl_Log("StartDevice: spinlocks + queues initialized\n");

    /* -------------------------------------------------------------
     * Allocate RX/TX buffers
     * ------------------------------------------------------------- */
    status = UARTCTRL_ExtAllocateBuffers(ext, 4096, 2048);
    if (!NT_SUCCESS(status)) {
        UartCtrl_Log("StartDevice: buffer allocation FAILED (0x%08lx)\n",
                     status);
        return status;
    }

    UartCtrl_Log("StartDevice: buffers allocated\n");

    /* -------------------------------------------------------------
     * Reset counters and state
     * ------------------------------------------------------------- */
    ext->RxErrors  = 0;
    ext->TxErrors  = 0;
    ext->OpenCount = 0;
    ext->Started   = FALSE;
    ext->Removed   = FALSE;

    /* -------------------------------------------------------------
     * Default UART configuration
     * ------------------------------------------------------------- */
    ext->Config.BaudRate = 115200;
    ext->Config.DataBits = 8;
    ext->Config.StopBits = 1;
    ext->Config.Parity   = 0;

    UartCtrl_Log("StartDevice: default config: %u baud, %u data, %u stop, parity=%u\n",
                 ext->Config.BaudRate,
                 ext->Config.DataBits,
                 ext->Config.StopBits,
                 ext->Config.Parity);

    /* -------------------------------------------------------------
     * Program hardware
     * ------------------------------------------------------------- */
    UartEnableFifo(ext, FCR_TRIG_8);
    UartSetLineControl(ext,
                       ext->Config.DataBits,
                       ext->Config.StopBits,
                       ext->Config.Parity);
    UartSetBaud(ext, ext->ClockHz, ext->Config.BaudRate);
    UartSetModemControl(ext, MCR_RTS | MCR_OUT2);
    UartEnableInterrupts(ext, IER_RDA | IER_THRE | IER_RLS);

    UartCtrl_Log("StartDevice: hardware initialized\n");

    ext->Started = TRUE;

    UartCtrl_Log("StartDevice: SUCCESS\n");

    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * UARTCTRL_StopDevice – disconnect ISR, free buffers, unmap MMIO, flush queues
 * ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_StopDevice(
    PUARTCTRL_FDO ext
    )
{
    KIRQL irql;

    UartCtrl_Log("StopDevice: begin\n");

    /* -------------------------------------------------------------
     * Cancel polling timer if active
     * ------------------------------------------------------------- */
    if (ext->Polling) {
        UartCtrl_Log("StopDevice: cancelling polling timer\n");
        KeCancelTimer(&ext->PollTimer);
        ext->Polling = FALSE;
    }

    /* -------------------------------------------------------------
     * Disconnect interrupt if connected
     * ------------------------------------------------------------- */
    if (ext->InterruptConnected) {
        UartCtrl_Log("StopDevice: disconnecting interrupt\n");
        IoDisconnectInterrupt(ext->InterruptObject);
        ext->InterruptObject    = NULL;
        ext->InterruptConnected = FALSE;
    }

    /* -------------------------------------------------------------
     * Unmap MMIO region
     * ------------------------------------------------------------- */
    if (ext->MmioBase != NULL) {
        UartCtrl_Log("StopDevice: unmapping MMIO (len=%lu)\n",
                     ext->MmioLength);

        MmUnmapIoSpace(ext->MmioBase, ext->MmioLength);

        ext->MmioBase   = NULL;
        ext->MmioLength = 0;
    }

    /* -------------------------------------------------------------
     * Flush queued read IRPs
     * ------------------------------------------------------------- */
    UartCtrl_Log("StopDevice: flushing read queue\n");

    KeAcquireSpinLock(&ext->ReadQueueLock, &irql);

    while (!IsListEmpty(&ext->ReadQueue)) {

        PLIST_ENTRY le = RemoveHeadList(&ext->ReadQueue);
        PIRP irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);

        UartCtrl_Log("StopDevice: completing pending read IRP %p\n", irp);

        irp->IoStatus.Status      = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;

        KeReleaseSpinLock(&ext->ReadQueueLock, irql);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
    }

    KeReleaseSpinLock(&ext->ReadQueueLock, irql);

    /* -------------------------------------------------------------
     * Free RX/TX buffers
     * ------------------------------------------------------------- */
    UartCtrl_Log("StopDevice: freeing RX/TX buffers\n");
    UARTCTRL_ExtFreeBuffers(ext);

    /* -------------------------------------------------------------
     * Reset counters and flags
     * ------------------------------------------------------------- */
    ext->RxErrors  = 0;
    ext->TxErrors  = 0;
    ext->OpenCount = 0;
    ext->Started   = FALSE;

    UartCtrl_Log("StopDevice: complete\n");

    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * UARTCTRL_RemoveDevice – full cleanup on device removal
 * ----------------------------------------------------------------------- */
NTSTATUS
UARTCTRL_RemoveDevice(
    PDEVICE_OBJECT DevObj
    )
{
    PUARTCTRL_FDO ext;
    KIRQL irql;

    ext = (PUARTCTRL_FDO)DevObj->DeviceExtension;

    UartCtrl_Log("RemoveDevice: begin (FDO=%p)\n", DevObj);

    /* -------------------------------------------------------------
     * Mark state
     * ------------------------------------------------------------- */
    ext->Removed = TRUE;
    ext->Started = FALSE;

    /* -------------------------------------------------------------
     * Stop hardware and free buffers
     * ------------------------------------------------------------- */
    UartCtrl_Log("RemoveDevice: calling StopDevice\n");
    UARTCTRL_StopDevice(ext);

    /* -------------------------------------------------------------
     * Flush queued read IRPs
     * ------------------------------------------------------------- */
    UartCtrl_Log("RemoveDevice: flushing read queue\n");

    KeAcquireSpinLock(&ext->ReadQueueLock, &irql);

    while (!IsListEmpty(&ext->ReadQueue)) {

        PLIST_ENTRY le = RemoveHeadList(&ext->ReadQueue);
        PIRP irp = CONTAINING_RECORD(le, IRP, Tail.Overlay.ListEntry);

        UartCtrl_Log("RemoveDevice: completing pending read IRP %p\n", irp);

        irp->IoStatus.Status      = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;

        KeReleaseSpinLock(&ext->ReadQueueLock, irql);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        KeAcquireSpinLock(&ext->ReadQueueLock, &irql);
    }

    KeReleaseSpinLock(&ext->ReadQueueLock, irql);

    /* -------------------------------------------------------------
     * Delete symbolic link if present
     * ------------------------------------------------------------- */
    if (ext->Symlink.Buffer != NULL) {
        UartCtrl_Log("RemoveDevice: deleting symbolic link '%wZ'\n",
                     &ext->Symlink);

        IoDeleteSymbolicLink(&ext->Symlink);
        RtlZeroMemory(&ext->Symlink, sizeof(ext->Symlink));
    }

    /* -------------------------------------------------------------
     * Detach from lower device
     * ------------------------------------------------------------- */
    if (ext->LowerDevice != NULL) {
        UartCtrl_Log("RemoveDevice: detaching from lower device %p\n",
                     ext->LowerDevice);

        IoDetachDevice(ext->LowerDevice);
        ext->LowerDevice = NULL;
    }

    /* -------------------------------------------------------------
     * Release remove lock and wait for outstanding I/O
     * ------------------------------------------------------------- */
    UartCtrl_Log("RemoveDevice: releasing remove lock and waiting\n");

    IoReleaseRemoveLockAndWait(&ext->RemoveLock, NULL);

    /* -------------------------------------------------------------
     * Delete our device object
     * ------------------------------------------------------------- */
    UartCtrl_Log("RemoveDevice: deleting FDO %p\n", DevObj);

    IoDeleteDevice(DevObj);

    UartCtrl_Log("RemoveDevice: complete\n");

    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * kernel logger with printf-style formatting + timestamp prefix
 * ----------------------------------------------------------------------- */
VOID
UartCtrl_Log(
    PCSTR Format,
    ...
    )
{
    CHAR  buffer[512];
    CHAR  final[600];
    va_list args;
    NTSTATUS status;

    UNICODE_STRING      path;
    OBJECT_ATTRIBUTES   oa;
    IO_STATUS_BLOCK     iosb;
    HANDLE              hFile;

    LARGE_INTEGER       sysTime, localTime;
    TIME_FIELDS         tf;

    PAGED_CODE();

    if (Format == NULL) {
        return;
    }

    /* Format the caller's message */
    va_start(args, Format);
    status = RtlStringCbVPrintfA(buffer, sizeof(buffer), Format, args);
    va_end(args);

    if (!NT_SUCCESS(status)) {
        return;
    }

    /* Get local time */
    KeQuerySystemTime(&sysTime);
    ExSystemTimeToLocalTime(&sysTime, &localTime);
    RtlTimeToTimeFields(&localTime, &tf);

    /* Format timestamp prefix: [DD/MM/YYYY, HH:MM AM/PM] */
    {
        CHAR ts[64];
        ULONG hour = tf.Hour;
        BOOLEAN pm = FALSE;

        if (hour == 0) {
            hour = 12;
            pm = FALSE;
        } else if (hour == 12) {
            pm = TRUE;
        } else if (hour > 12) {
            hour -= 12;
            pm = TRUE;
        }

        RtlStringCbPrintfA(
            ts,
            sizeof(ts),
            "[%02u/%02u/%04u, %02u:%02u %s] ",
            tf.Day,
            tf.Month,
            tf.Year,
            hour,
            tf.Minute,
            pm ? "PM" : "AM"
        );

        /* Combine timestamp + message */
        RtlStringCbPrintfA(
            final,
            sizeof(final),
            "%s%s",
            ts,
            buffer
        );
    }

    /* Open log file */
    RtlInitUnicodeString(&path, L"\\SystemRoot\\System32\\uartctrl.log");

    InitializeObjectAttributes(
        &oa,
        &path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL
    );

    status = ZwCreateFile(
                 &hFile,
                 FILE_APPEND_DATA | SYNCHRONIZE,
                 &oa,
                 &iosb,
                 NULL,
                 FILE_ATTRIBUTE_NORMAL,
                 0,
                 FILE_OPEN_IF,
                 FILE_SYNCHRONOUS_IO_NONALERT,
                 NULL,
                 0
             );

    if (!NT_SUCCESS(status)) {
        return;
    }

    /* Write timestamped line */
    ZwWriteFile(
        hFile,
        NULL,
        NULL,
        NULL,
        &iosb,
        final,
        (ULONG)strlen(final),
        NULL,
        NULL
    );

    ZwClose(hFile);
}

/* -----------------------------------------------------------------------
 * UartCtrl_LogIsr – lightweight ISR/DPC-safe logger (DbgPrint only)
 * ----------------------------------------------------------------------- */
VOID
UartCtrl_LogIsr(
    PCSTR Format,
    ...
    )
{
    va_list args;

    if (Format == NULL) {
        return;
    }

    va_start(args, Format);
    vDbgPrintEx(DPFLTR_IHVDRIVER_ID,
                DPFLTR_INFO_LEVEL,
                Format,
                args);
    va_end(args);
}
