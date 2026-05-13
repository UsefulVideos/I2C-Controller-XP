/* i2cctrl_pdo.c */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <strsafe.h>
#include "i2cctrl_ext.h"
#include "i2cctrl_hw.h"
#include "i2cctrl_bsod.h"
#include "i2cctrl_i2c.h"
#include "i2cctrl_detect.h"

#define TAG_PDO 'doPC'

/* Duplicate/free UNICODE_STRINGs (NonPagedPool) */
NTSTATUS
I2cCtrl_DupString(
    PUNICODE_STRING Dest,
    PCWSTR Src
    );

VOID
I2cCtrl_FreeString(
    PUNICODE_STRING S
    );

/* -----------------------------------------------------------------------
 * I2cCtrl_SurpriseRemoveQuiesce - quiesce hardware on surprise removal
 * - Masks interrupts under HW lock
 * - Acks latched causes
 * - Drains RX, flushes TX with bounded helpers
 * - Disables controller and waits for it to latch
 * - Marks controller as failed if anything looks suspicious
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant.
 * Never re-enable during teardown.
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_SurpriseRemoveQuiesce(
    PI2CCTRL_FDO devctx
    )
{
    KIRQL    oldIrql;
    ULONG    raw1;
    ULONG    raw2;
    NTSTATUS status;

    oldIrql = 0;
    raw1    = 0U;
    raw2    = 0U;
    status  = STATUS_SUCCESS;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    PAGED_CODE();

    if (devctx == NULL || devctx->Ops == NULL) {
        return;
    }

    /* If MMIO is already gone, nothing to touch */
    if (devctx->MmioBase == NULL || devctx->MmioLength == 0U) {
        return;
    }

    /* Mark as being torn down so ISR/DPC/IO paths can bail early */
    devctx->Removed = TRUE;

    /* 1) Mask all controller interrupts under HW lock */
    KeAcquireSpinLock(&devctx->HwLock, &oldIrql);
    if (devctx->Ops->MaskInterrupts != NULL) {
        devctx->Ops->MaskInterrupts(devctx, 0U);
    }
    KeReleaseSpinLock(&devctx->HwLock, oldIrql);

    /* 2) Acknowledge common interrupt causes (best-effort) */
    if (devctx->Ops->AckInterrupts != NULL) {
        devctx->Ops->AckInterrupts(
            devctx,
            I2C_INT_TX_ABORT      |
            I2C_INT_RX_OVER       |
            I2C_INT_RX_UNDER      |
            I2C_INT_STOP_DETECTED |
            I2C_INT_START_DETECTED|
            I2C_INT_GEN_CALL      |
            I2C_INT_ACTIVITY      |
            I2C_INT_RX_DONE       |
            I2C_INT_RD_REQ
            );
    }

    /* 3) Interrupt storm protection: confirm raw status clears */
    if (devctx->Ops->GetRawIntr != NULL) {
        raw1 = devctx->Ops->GetRawIntr(devctx);

        if (devctx->Ops->AckInterrupts != NULL && raw1 != 0U) {
            devctx->Ops->AckInterrupts(
                devctx,
                I2C_INT_TX_ABORT      |
                I2C_INT_RX_OVER       |
                I2C_INT_RX_UNDER      |
                I2C_INT_STOP_DETECTED |
                I2C_INT_START_DETECTED|
                I2C_INT_GEN_CALL      |
                I2C_INT_ACTIVITY      |
                I2C_INT_RX_DONE       |
                I2C_INT_RD_REQ
                );
        }

        raw2 = devctx->Ops->GetRawIntr(devctx);

        if (raw1 != 0U && raw2 != 0U) {
            KdPrint(("I2CCTRL: SurpriseRemoveQuiesce: interrupt storm, marking HW failed\n"));
            devctx->HardwareFailure = TRUE;
        }
    }

    /* 4) Drain RX FIFO safely using bounded helper */
    if (devctx->Ops->DrainRxBounded != NULL) {
        devctx->Ops->DrainRxBounded(devctx);
    }

    /* 5) Flush TX FIFO safely using bounded helper */
    if (devctx->Ops->FlushTxBounded != NULL) {
        devctx->Ops->FlushTxBounded(devctx);
    }

    /* 6) Disable controller (never re-enable during teardown) */
    if (devctx->Ops->Enable != NULL) {
        (VOID)devctx->Ops->Enable(devctx, FALSE);
    }

    status = I2cCtrl_WaitForEnableState(devctx, FALSE, 500U);
    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL: SurpriseRemoveQuiesce: disable did not latch (status=0x%08lx)\n",
                 status));
        devctx->HardwareFailure = TRUE;
    }

    /* 7) Cancel outstanding IRPs/transactions gracefully */
    I2cCtrl_CancelAllQueuedTransfers(devctx);
}

/*
 * Safe ACPI child handle close (XP/2003‑compatible, ACPI‑safe)
 * - Close ACPI handle exactly once
 * - PASSIVE_LEVEL only
 * - Never touches ACPI-owned memory
 * - Never zeroes ACPI-visible fields
 */
VOID
I2cCtrl_AcpiCloseChild(
    PI2CCTRL_PDO ChildDx
    )
{
    PVOID acpiHandle;

    if (ChildDx == NULL) {
        return;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    acpiHandle = ChildDx->AcpiHandle;
    if (acpiHandle == NULL) {
        return;     /* Already closed or never opened */
    }

    /* Close the ACPI handle safely */
    I2cCtrl_AcpiCloseHandle(acpiHandle);

    /* Clear our cached pointer */
    ChildDx->AcpiHandle = NULL;

    DbgPrint("I2CCTRL: ACPI child handle closed for PDO %p\n",
             ChildDx->Pdo);
}


/* -----------------------------------------------------------------------
 * I2cCtrl_PdoDispatchPower - PDO dispatch power
 * XP/2003 BSOD-safe, WinDDK-compiler-safe, C89-compliant.
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_PdoDispatchPower(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    )
{
    PI2CCTRL_PDO       pdoExt;
    PIO_STACK_LOCATION ps;
    DEVICE_POWER_STATE newState;

    PAGED_CODE();

    pdoExt = (PI2CCTRL_PDO)DeviceObject->DeviceExtension;
    ps     = IoGetCurrentIrpStackLocation(Irp);

    PoStartNextPowerIrp(Irp);

    if (ps->MinorFunction == IRP_MN_SET_POWER &&
        ps->Parameters.Power.Type == DevicePowerState)
    {
        newState = ps->Parameters.Power.State.DeviceState;

        switch (newState) {
        case PowerDeviceD0:
            (void)I2cHwPowerOnAcpi(DeviceObject);
            pdoExt->CurrentPowerState = PowerDeviceD0;
            pdoExt->Started           = TRUE;
            break;

        case PowerDeviceD3:
            (void)I2cHwPowerOffAcpi(DeviceObject);
            pdoExt->CurrentPowerState = PowerDeviceD3;
            pdoExt->Started           = FALSE;
            break;

        default:
            break;
        }
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}


/* -----------------------------------------------------------------------
 * I2cCtrl_PdoDispatch - PDO dispatch routine
 * XP/2003 BSOD-safe, WinDDK-compiler-safe, C89-compliant.
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_PdoDispatch(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp
    )
{
    NTSTATUS           status;
    PIO_STACK_LOCATION irpSp;
    PI2CCTRL_PDO       pdoExt;

    /* C89 init */
    status = STATUS_SUCCESS;

    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    PAGED_CODE();

    if (DeviceObject == NULL || Irp == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

pdoExt = (PI2CCTRL_PDO)DeviceObject->DeviceExtension;

if (pdoExt == NULL || pdoExt->Pdo != DeviceObject) {

    I2cCtrl_Log("PDO: Invalid extension or PDO mismatch "
                "(pdoExt=%p, pdoExt->Pdo=%p, DeviceObject=%p) "
                "→ STATUS_INVALID_DEVICE_REQUEST\n",
                pdoExt,
                (pdoExt ? pdoExt->Pdo : NULL),
                DeviceObject);

    Irp->IoStatus.Status      = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0U;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}


if (pdoExt->Removed) {

    I2cCtrl_Log("PDO: Device %p already marked Removed → STATUS_NO_SUCH_DEVICE\n",
                DeviceObject);

    Irp->IoStatus.Status      = STATUS_NO_SUCH_DEVICE;
    Irp->IoStatus.Information = 0U;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_NO_SUCH_DEVICE;
}

irpSp = IoGetCurrentIrpStackLocation(Irp);

if (irpSp == NULL) {

    I2cCtrl_Log("PDO: irpSp NULL for device %p → STATUS_INVALID_PARAMETER\n",
                DeviceObject);

    Irp->IoStatus.Status      = STATUS_INVALID_PARAMETER;
    Irp->IoStatus.Information = 0U;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_PARAMETER;
}

    status = Irp->IoStatus.Status;

    switch (irpSp->MajorFunction) {

    /* ================================================================
       PNP IRPs
       ================================================================ */
    case IRP_MJ_PNP:

        switch (irpSp->MinorFunction) {

/* ------------------------------------------------------------
   START_DEVICE (PDO)
   ------------------------------------------------------------ */
case IRP_MN_START_DEVICE:
{
    PI2CCTRL_FDO fdoExt;
    ULONG        len;

    I2cCtrl_Log("PDO: START_DEVICE entered for PDO %p\n", DeviceObject);

    if (pdoExt->Removed) {
        I2cCtrl_Log("PDO: START_DEVICE rejected — device already Removed\n");
        status = STATUS_NO_SUCH_DEVICE;
        break;
    }

    fdoExt = pdoExt->ParentFdo;
    if (fdoExt == NULL) {
        I2cCtrl_Log("PDO: START_DEVICE failed — ParentFdo is NULL\n");
        status = STATUS_DEVICE_NOT_CONNECTED;
        break;
    }

    /* ACPI-derived HID registers must be valid */
    if (pdoExt->HidDescRegister == 0 ||
        pdoExt->DataRegister    == 0 ||
        pdoExt->SlaveAddress    == 0)
    {
        I2cCtrl_Log("PDO: START_DEVICE failed — invalid ACPI HID registers "
                    "(HidDescReg=0x%X, DataReg=0x%X, SlaveAddr=0x%X)\n",
                    pdoExt->HidDescRegister,
                    pdoExt->DataRegister,
                    pdoExt->SlaveAddress);

        status = STATUS_INVALID_DEVICE_STATE;
        break;
    }

    /* Clamp GPIO values */
    if (pdoExt->GpioLevel  > 255) pdoExt->GpioLevel  = 5;
    if (pdoExt->GpioVector > 255) pdoExt->GpioVector = 5;

    I2cCtrl_Log("PDO: Reading HID descriptor (SlaveAddr=0x%X)\n",
                pdoExt->SlaveAddress);

    /* Read HID descriptor */
    status = I2cCtrl_ReadHidDescriptor(fdoExt, pdoExt, &pdoExt->HidDesc);
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("PDO: ReadHidDescriptor FAILED (0x%08X)\n", status);
        break;
    }

    /* Allocate + read HID report descriptor */
    len = pdoExt->HidDesc.DescriptorList[0].wReportLength;

    I2cCtrl_Log("PDO: HID report length = %u bytes\n", len);

    if (len == 0 || len > HID_REPORT_MAX_LEN) {
        I2cCtrl_Log("PDO: Invalid HID report length %u → STATUS_INVALID_DEVICE_STATE\n", len);
        status = STATUS_INVALID_DEVICE_STATE;
        break;
    }

    pdoExt->HidReportDescLen = len;
    pdoExt->HidReportDesc = ExAllocatePoolWithTag(
                                NonPagedPool,
                                len,
                                TAG_I2C_MISC);

    if (pdoExt->HidReportDesc == NULL) {
        I2cCtrl_Log("PDO: HID report descriptor allocation FAILED (%u bytes)\n", len);
        status = STATUS_INSUFFICIENT_RESOURCES;
        break;
    }

    RtlZeroMemory(pdoExt->HidReportDesc, len);

    I2cCtrl_Log("PDO: Reading HID report descriptor (%u bytes)\n", len);

    status = I2cCtrl_ReadReportDescriptor(
                 fdoExt,
                 pdoExt,
                 pdoExt->HidReportDesc,
                 len);

    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("PDO: ReadReportDescriptor FAILED (0x%08X) — freeing buffer\n", status);
        ExFreePoolWithTag(pdoExt->HidReportDesc, TAG_I2C_MISC);
        pdoExt->HidReportDesc = NULL;
        break;
    }

    /* Initialize HID input queue */
    KeInitializeEvent(&pdoExt->HidReportEvent, NotificationEvent, FALSE);
    pdoExt->PendingHidReadIrp = NULL;

    pdoExt->Started = TRUE;

    I2cCtrl_Log("PDO: START_DEVICE completed successfully for PDO %p\n",
                DeviceObject);

    status = STATUS_SUCCESS;
    break;
}

/* ------------------------------------------------------------
   QUERY_ID
   ------------------------------------------------------------ */
case IRP_MN_QUERY_ID:
{
    UNICODE_STRING src;
    PWSTR          idBuf;
    SIZE_T         len;
    PWSTR          p;
    SIZE_T         total;

    I2cCtrl_Log("PDO: QUERY_ID entered (IdType=%u) for PDO %p\n",
                irpSp->Parameters.QueryId.IdType,
                DeviceObject);

    src.Buffer        = NULL;
    src.Length        = 0;
    src.MaximumLength = 0;
    idBuf             = NULL;
    len               = 0U;

    switch (irpSp->Parameters.QueryId.IdType) {

    /* --------------------------------------------------------
       BusQueryDeviceID
       -------------------------------------------------------- */
    case BusQueryDeviceID:

        I2cCtrl_Log("PDO: QUERY_ID → BusQueryDeviceID\n");

        src = pdoExt->HardwareId;
        if (src.Buffer == NULL || src.Length == 0) {
            I2cCtrl_Log("PDO: BusQueryDeviceID FAILED — HardwareId missing\n");
            status = STATUS_NOT_FOUND;
            break;
        }

        len = src.Length + sizeof(WCHAR);
        idBuf = (PWSTR)ExAllocatePoolWithTag(PagedPool, len, TAG_PDO);
        if (idBuf == NULL) {
            I2cCtrl_Log("PDO: BusQueryDeviceID FAILED — allocation %u bytes\n", len);
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        RtlZeroMemory(idBuf, len);
        RtlCopyMemory(idBuf, src.Buffer, src.Length);

        Irp->IoStatus.Information = (ULONG_PTR)idBuf;

        I2cCtrl_Log("PDO: BusQueryDeviceID SUCCESS — \"%wZ\"\n", &src);

        status = STATUS_SUCCESS;
        break;

    /* --------------------------------------------------------
       BusQueryHardwareIDs (MULTI_SZ)
       -------------------------------------------------------- */
    case BusQueryHardwareIDs:

        I2cCtrl_Log("PDO: QUERY_ID → BusQueryHardwareIDs\n");

        if (pdoExt->HardwareIdsMultiSz == NULL) {
            I2cCtrl_Log("PDO: BusQueryHardwareIDs FAILED — no MULTI_SZ\n");
            status = STATUS_NOT_FOUND;
            break;
        }

        p     = pdoExt->HardwareIdsMultiSz;
        total = 0U;

        while (*p) {
            SIZE_T l = wcslen(p) + 1;
            total += l;
            p += l;
        }
        total++;

        len = total * sizeof(WCHAR);

        idBuf = (PWSTR)ExAllocatePoolWithTag(PagedPool, len, TAG_PDO);
        if (idBuf == NULL) {
            I2cCtrl_Log("PDO: BusQueryHardwareIDs FAILED — allocation %u bytes\n", len);
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        RtlCopyMemory(idBuf, pdoExt->HardwareIdsMultiSz, len);
        Irp->IoStatus.Information = (ULONG_PTR)idBuf;

        I2cCtrl_Log("PDO: BusQueryHardwareIDs SUCCESS (MULTI_SZ length=%u chars)\n", total);

        status = STATUS_SUCCESS;
        break;

    /* --------------------------------------------------------
       BusQueryCompatibleIDs (MULTI_SZ)
       -------------------------------------------------------- */
    case BusQueryCompatibleIDs:

        I2cCtrl_Log("PDO: QUERY_ID → BusQueryCompatibleIDs\n");

        if (pdoExt->CompatibleIdsMultiSz == NULL) {
            I2cCtrl_Log("PDO: BusQueryCompatibleIDs — no MULTI_SZ, returning empty\n");
            Irp->IoStatus.Information = 0;
            status = STATUS_SUCCESS;
            break;
        }

        p     = pdoExt->CompatibleIdsMultiSz;
        total = 0U;

        while (*p) {
            SIZE_T l = wcslen(p) + 1;
            total += l;
            p += l;
        }
        total++;

        len = total * sizeof(WCHAR);

        idBuf = (PWSTR)ExAllocatePoolWithTag(PagedPool, len, TAG_PDO);
        if (idBuf == NULL) {
            I2cCtrl_Log("PDO: BusQueryCompatibleIDs FAILED — allocation %u bytes\n", len);
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        RtlCopyMemory(idBuf, pdoExt->CompatibleIdsMultiSz, len);
        Irp->IoStatus.Information = (ULONG_PTR)idBuf;

        I2cCtrl_Log("PDO: BusQueryCompatibleIDs SUCCESS (MULTI_SZ length=%u chars)\n", total);

        status = STATUS_SUCCESS;
        break;

    /* --------------------------------------------------------
       BusQueryInstanceID
       -------------------------------------------------------- */
    case BusQueryInstanceID:

        I2cCtrl_Log("PDO: QUERY_ID → BusQueryInstanceID\n");

        src = pdoExt->InstanceId;
        if (src.Buffer == NULL || src.Length == 0) {
            I2cCtrl_Log("PDO: BusQueryInstanceID FAILED — InstanceId missing\n");
            status = STATUS_NOT_FOUND;
            break;
        }

        len = src.Length + sizeof(WCHAR);
        idBuf = (PWSTR)ExAllocatePoolWithTag(PagedPool, len, TAG_PDO);
        if (idBuf == NULL) {
            I2cCtrl_Log("PDO: BusQueryInstanceID FAILED — allocation %u bytes\n", len);
            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        RtlZeroMemory(idBuf, len);
        RtlCopyMemory(idBuf, src.Buffer, src.Length);

        Irp->IoStatus.Information = (ULONG_PTR)idBuf;

        I2cCtrl_Log("PDO: BusQueryInstanceID SUCCESS — \"%wZ\"\n", &src);

        status = STATUS_SUCCESS;
        break;

    /* --------------------------------------------------------
       Unsupported ID type
       -------------------------------------------------------- */
    default:
        I2cCtrl_Log("PDO: QUERY_ID unsupported IdType=%u → STATUS_NOT_SUPPORTED\n",
                    irpSp->Parameters.QueryId.IdType);

        status = STATUS_NOT_SUPPORTED;
        break;
    }

    break;
}

/* ------------------------------------------------------------
   QUERY_DEVICE_TEXT
   ------------------------------------------------------------ */
case IRP_MN_QUERY_DEVICE_TEXT:
{
    PWSTR  buf;
    SIZE_T len;

    I2cCtrl_Log("PDO: QUERY_DEVICE_TEXT entered (Type=%u) for PDO %p\n",
                irpSp->Parameters.QueryDeviceText.DeviceTextType,
                DeviceObject);

    if (irpSp->Parameters.QueryDeviceText.DeviceTextType !=
        DeviceTextDescription)
    {
        I2cCtrl_Log("PDO: QUERY_DEVICE_TEXT unsupported Type=%u → STATUS_NOT_SUPPORTED\n",
                    irpSp->Parameters.QueryDeviceText.DeviceTextType);

        status = STATUS_NOT_SUPPORTED;
        break;
    }

    {
        static const WCHAR desc[] =
            L"ACPI HID-compliant I2C Touchpad (PNP0C50)";

        len = sizeof(desc);

        I2cCtrl_Log("PDO: Allocating %u bytes for DeviceTextDescription\n", (unsigned)len);

        buf = (PWSTR)ExAllocatePoolWithTag(PagedPool, len, TAG_PDO);
        if (buf == NULL) {

            I2cCtrl_Log("PDO: QUERY_DEVICE_TEXT allocation FAILED (%u bytes)\n",
                        (unsigned)len);

            status = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        RtlCopyMemory(buf, desc, len);
        Irp->IoStatus.Information = (ULONG_PTR)buf;

        I2cCtrl_Log("PDO: QUERY_DEVICE_TEXT SUCCESS — \"%ws\"\n", desc);

        status = STATUS_SUCCESS;
    }
    break;
}

/* ------------------------------------------------------------
   QUERY_CAPABILITIES
   ------------------------------------------------------------ */
case IRP_MN_QUERY_CAPABILITIES:
{
    PDEVICE_CAPABILITIES caps;

    I2cCtrl_Log("PDO: QUERY_CAPABILITIES entered for PDO %p\n",
                DeviceObject);

    caps = irpSp->Parameters.DeviceCapabilities.Capabilities;
    if (caps == NULL) {

        I2cCtrl_Log("PDO: QUERY_CAPABILITIES FAILED — caps pointer is NULL\n");

        status = STATUS_INVALID_PARAMETER;
        break;
    }

    I2cCtrl_Log("PDO: Filling DEVICE_CAPABILITIES structure\n");

    RtlZeroMemory(caps, sizeof(*caps));
    caps->Size    = sizeof(DEVICE_CAPABILITIES);
    caps->Version = 1;

    caps->DeviceD1 = FALSE;
    caps->DeviceD2 = FALSE;

    caps->WakeFromD3 = TRUE;
    caps->DeviceWake = PowerDeviceD3;
    caps->SystemWake = PowerSystemSleeping3;

    caps->Removable         = FALSE;
    caps->SurpriseRemovalOK = FALSE;
    caps->UniqueID          = TRUE;

    caps->Address  = pdoExt->SavedBusAddress;
    caps->UINumber = 0;

    I2cCtrl_Log("PDO: QUERY_CAPABILITIES SUCCESS — "
                "Address=%u, UniqueID=%u, WakeFromD3=%u\n",
                caps->Address,
                caps->UniqueID,
                caps->WakeFromD3);

    status = STATUS_SUCCESS;
    break;
}

/* ------------------------------------------------------------
   QUERY_DEVICE_RELATIONS (TargetDeviceRelation)
   ------------------------------------------------------------ */
case IRP_MN_QUERY_DEVICE_RELATIONS:
{
    PDEVICE_RELATIONS rel;

    I2cCtrl_Log("PDO: QUERY_DEVICE_RELATIONS entered (Type=%u) for PDO %p\n",
                irpSp->Parameters.QueryDeviceRelations.Type,
                DeviceObject);

    if (irpSp->Parameters.QueryDeviceRelations.Type != TargetDeviceRelation) {

        I2cCtrl_Log("PDO: QUERY_DEVICE_RELATIONS unsupported Type=%u "
                    "→ STATUS_NOT_SUPPORTED\n",
                    irpSp->Parameters.QueryDeviceRelations.Type);

        status = STATUS_NOT_SUPPORTED;
        break;
    }

    I2cCtrl_Log("PDO: Allocating DEVICE_RELATIONS for TargetDeviceRelation\n");

    rel = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(
              PagedPool,
              sizeof(DEVICE_RELATIONS),
              TAG_PDO);

    if (rel == NULL) {

        I2cCtrl_Log("PDO: QUERY_DEVICE_RELATIONS allocation FAILED "
                    "(%u bytes)\n",
                    (unsigned)sizeof(DEVICE_RELATIONS));

        status = STATUS_INSUFFICIENT_RESOURCES;
        break;
    }

    rel->Count      = 1;
    rel->Objects[0] = DeviceObject;

    ObReferenceObject(DeviceObject);

    Irp->IoStatus.Information = (ULONG_PTR)rel;

    I2cCtrl_Log("PDO: QUERY_DEVICE_RELATIONS SUCCESS — returning self (%p)\n",
                DeviceObject);

    status = STATUS_SUCCESS;
    break;
}


/* ------------------------------------------------------------
   QUERY_RESOURCES (synthetic)
   ------------------------------------------------------------ */
case IRP_MN_QUERY_RESOURCES:
{
    PCM_RESOURCE_LIST res;
    PCM_FULL_RESOURCE_DESCRIPTOR frd;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR prd;
    ULONG size;

    I2cCtrl_Log("PDO: QUERY_RESOURCES entered for PDO %p\n",
                DeviceObject);

    size = sizeof(CM_RESOURCE_LIST) +
           sizeof(CM_FULL_RESOURCE_DESCRIPTOR) +
           2 * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);

    I2cCtrl_Log("PDO: Allocating %u bytes for synthetic resource list\n",
                (unsigned)size);

    res = (PCM_RESOURCE_LIST)ExAllocatePoolWithTag(
              PagedPool,
              size,
              TAG_PDO);

    if (res == NULL) {

        I2cCtrl_Log("PDO: QUERY_RESOURCES allocation FAILED (%u bytes)\n",
                    (unsigned)size);

        status = STATUS_INSUFFICIENT_RESOURCES;
        break;
    }

    RtlZeroMemory(res, size);
    res->Count = 1;

    frd = &res->List[0];
    frd->InterfaceType = Internal;
    frd->BusNumber     = 0;

    frd->PartialResourceList.Version  = 1;
    frd->PartialResourceList.Revision = 1;
    frd->PartialResourceList.Count    = 2;

    /* --------------------------------------------------------
       1) synthetic MEMORY resource
       -------------------------------------------------------- */
    prd = &frd->PartialResourceList.PartialDescriptors[0];
    prd->Type             = CmResourceTypeMemory;
    prd->ShareDisposition = CmResourceShareShared;
    prd->Flags            = CM_RESOURCE_MEMORY_READ_WRITE;

    prd->u.Memory.Start.QuadPart =
        (ULONGLONG)(0x1000 + pdoExt->SavedBusAddress);
    prd->u.Memory.Length = 0x100;

    I2cCtrl_Log("PDO: QUERY_RESOURCES MEMORY: Start=0x%llX Length=0x%X\n",
                prd->u.Memory.Start.QuadPart,
                prd->u.Memory.Length);

    /* --------------------------------------------------------
       2) Interrupt resource
       -------------------------------------------------------- */
    prd = &frd->PartialResourceList.PartialDescriptors[1];
    prd->Type             = CmResourceTypeInterrupt;
    prd->ShareDisposition = CmResourceShareShared;
    prd->Flags            = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;

    if (pdoExt->GpioLevel  > 255) pdoExt->GpioLevel  = 5;
    if (pdoExt->GpioVector > 255) pdoExt->GpioVector = 5;

    prd->u.Interrupt.Level    = pdoExt->GpioLevel;
    prd->u.Interrupt.Vector   = pdoExt->GpioVector;
    prd->u.Interrupt.Affinity = (ULONG)-1;

    I2cCtrl_Log("PDO: QUERY_RESOURCES IRQ: Level=%u Vector=%u Affinity=0x%X\n",
                prd->u.Interrupt.Level,
                prd->u.Interrupt.Vector,
                prd->u.Interrupt.Affinity);

    Irp->IoStatus.Information = (ULONG_PTR)res;

    I2cCtrl_Log("PDO: QUERY_RESOURCES SUCCESS for PDO %p\n",
                DeviceObject);

    status = STATUS_SUCCESS;
    break;
}

/* ------------------------------------------------------------
   REMOVE_DEVICE (PDO)
   ------------------------------------------------------------ */
case IRP_MN_REMOVE_DEVICE:
{
    PI2CCTRL_PDO p = pdoExt;
    PI2CCTRL_FDO fdoExt;
    KIRQL        lockIrql;

    I2cCtrl_Log("PDO: REMOVE_DEVICE entered for PDO %p\n", DeviceObject);

    p->Removed = TRUE;
    p->Started = FALSE;

    /* Close ACPI child handle */
    if (p->AcpiHandle != NULL) {
        I2cCtrl_Log("PDO: Closing ACPI child handle %p\n", p->AcpiHandle);
        I2cCtrl_AcpiCloseChild(p);
        p->AcpiHandle = NULL;
    }

    /* Free HID report descriptor */
    if (p->HidReportDesc != NULL) {
        I2cCtrl_Log("PDO: Freeing HID report descriptor %p\n", p->HidReportDesc);
        ExFreePoolWithTag(p->HidReportDesc, TAG_I2C_MISC);
        p->HidReportDesc = NULL;
    }

    /* Free MULTI_SZ ID lists */
    if (p->HardwareIdsMultiSz != NULL) {
        I2cCtrl_Log("PDO: Freeing HardwareIdsMultiSz %p\n", p->HardwareIdsMultiSz);
        ExFreePoolWithTag(p->HardwareIdsMultiSz, TAG_I2C_MISC);
        p->HardwareIdsMultiSz = NULL;
    }

    if (p->CompatibleIdsMultiSz != NULL) {
        I2cCtrl_Log("PDO: Freeing CompatibleIdsMultiSz %p\n", p->CompatibleIdsMultiSz);
        ExFreePoolWithTag(p->CompatibleIdsMultiSz, TAG_I2C_MISC);
        p->CompatibleIdsMultiSz = NULL;
    }

    /* Free UNICODE_STRING IDs */
    if (p->HardwareId.Buffer != NULL) {
        I2cCtrl_Log("PDO: Freeing HardwareId buffer %p\n", p->HardwareId.Buffer);
        ExFreePoolWithTag(p->HardwareId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&p->HardwareId, sizeof(UNICODE_STRING));
    }

    if (p->InstanceId.Buffer != NULL) {
        I2cCtrl_Log("PDO: Freeing InstanceId buffer %p\n", p->InstanceId.Buffer);
        ExFreePoolWithTag(p->InstanceId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&p->InstanceId, sizeof(UNICODE_STRING));
    }

    if (p->CompatibleId.Buffer != NULL) {
        I2cCtrl_Log("PDO: Freeing CompatibleId buffer %p\n", p->CompatibleId.Buffer);
        ExFreePoolWithTag(p->CompatibleId.Buffer, TAG_I2C_MISC);
        RtlZeroMemory(&p->CompatibleId, sizeof(UNICODE_STRING));
    }

    /* Remove from parent FDO child list */
    fdoExt = p->ParentFdo;
    if (fdoExt != NULL) {

        I2cCtrl_Log("PDO: Removing PDO %p from parent FDO %p child list\n",
                    DeviceObject, fdoExt);

        KeAcquireSpinLock(&fdoExt->ChildLock, &lockIrql);

        if (!IsListEmpty(&p->ListEntry)) {
            RemoveEntryList(&p->ListEntry);
            InitializeListHead(&p->ListEntry);
        }

        if (fdoExt->NumChildren > 0) {
            fdoExt->NumChildren--;
        }

        KeReleaseSpinLock(&fdoExt->ChildLock, lockIrql);
    }

    Irp->IoStatus.Status      = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;

    I2cCtrl_Log("PDO: Completing REMOVE_DEVICE for PDO %p\n", DeviceObject);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    I2cCtrl_Log("PDO: Deleting PDO device object %p\n", DeviceObject);

    IoDeleteDevice(DeviceObject);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------
   Unsupported PNP minor
   ------------------------------------------------------------ */
default:

    I2cCtrl_Log("PDO: Unsupported PnP minor 0x%02X for PDO %p "
                "→ STATUS_NOT_SUPPORTED\n",
                irpSp->MinorFunction,
                DeviceObject);

    status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Information = 0;
    break;
}

Irp->IoStatus.Status = status;

I2cCtrl_Log("PDO: Completing PnP IRP (Minor=0x%02X) for PDO %p "
            "with status 0x%08X\n",
            irpSp->MinorFunction,
            DeviceObject,
            status);

IoCompleteRequest(Irp, IO_NO_INCREMENT);
return status;

/* ================================================================
   Unsupported major functions
   ================================================================ */
default:

    I2cCtrl_Log("PDO: Unsupported major 0x%02X for PDO %p "
                "→ STATUS_NOT_SUPPORTED\n",
                irpSp->MajorFunction,
                DeviceObject);

    status = STATUS_NOT_SUPPORTED;
    Irp->IoStatus.Status      = status;
    Irp->IoStatus.Information = 0;

    I2cCtrl_Log("PDO: Completing unsupported major 0x%02X for PDO %p "
                "with status 0x%08X\n",
                irpSp->MajorFunction,
                DeviceObject,
                status);

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}


}