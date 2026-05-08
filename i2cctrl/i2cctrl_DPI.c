/* -----------------------------------------------------------------------
   i2cctrl_DPI.c - Implementation of HID-over-I2C DPI helpers (C89 + ASCII logs)
   ----------------------------------------------------------------------- */

#include "i2cctrl_spinlock_fix.h"
#include <initguid.h>   /* forces GUIDs to be defined */
#include "i2cctrl_DPI.h"
/*
 * Initialize the DPI block with descriptors, IDs, and I2C address.
 */
NTSTATUS
I2cCtrl_DpiInitialize(
    PI2CCTRL_DPI dpi,
    USHORT vid,
    USHORT pid,
    USHORT ver,
    ULONG reportLen,
    PVOID desc,
    ULONG descLen,
    PUCHAR reportDesc,
    USHORT reportDescLen,
    UCHAR i2cAddr
    )
{
    NTSTATUS status;

    status = STATUS_SUCCESS;

    if (dpi == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    dpi->VendorId            = vid;
    dpi->ProductId           = pid;
    dpi->VersionNumber       = ver;
    dpi->ReportLength        = reportLen;
    dpi->HidDescriptor       = desc;
    dpi->HidDescriptorLength = descLen;
    dpi->ReportDescriptor    = reportDesc;
    dpi->ReportDescriptorLength = reportDescLen;
    dpi->I2cAddr7Bit         = i2cAddr;

    /* Clear/initialize optional members */
    dpi->InterruptObject     = NULL;
    dpi->InterruptConnected  = FALSE;
    dpi->InterruptVector     = 0;
    dpi->InterruptIrql       = 0;
    dpi->InterruptAffinity   = 0;
    dpi->InterruptMode       = Latched;
    dpi->InterruptSharable   = FALSE;
    dpi->PendingInputFlag    = 0;

    dpi->PollIntervalMs      = 0;
    dpi->WakeEnabled         = FALSE;
    dpi->IdleTimeoutMs       = 0;
    dpi->IdleArmed           = 0;

    InitializeListHead(&dpi->ReadQueue);
    I2CCTRL_INIT_LOCK(&dpi->ReadQueueLock);

    dpi->PalmThreshold       = 0;
    dpi->ScrollScale         = 0;
    dpi->TapTimeMs           = 0;

    dpi->ParentFdo           = NULL;
    dpi->PhysicalDevice      = NULL;
    dpi->BusCtx              = NULL;

    RtlInitUnicodeString(&dpi->RegistryPath, NULL);
    RtlInitUnicodeString(&dpi->Symlink, NULL);

    dpi->MaxX                = 0;
    dpi->MaxY                = 0;
    dpi->Sensitivity         = 0;

    dpi->MmioBase            = NULL;
    dpi->MmioLength          = 0;

    return status;
}

/*
 * Register HID-over-I2C device interface so user-mode HID clients can find it.
 * Uses the standard HID interface GUID from hidclass.h.
 */
NTSTATUS
I2cCtrl_DpiRegisterInterface(
    PDEVICE_OBJECT DeviceObject,
    PUNICODE_STRING SymbolicLinkName
    )
{
    NTSTATUS status;

    status = STATUS_SUCCESS;

    if (DeviceObject == NULL || SymbolicLinkName == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = IoRegisterDeviceInterface(DeviceObject,
                                       &GUID_DEVINTERFACE_HID,
                                       NULL,
                                       SymbolicLinkName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL_DPI: IoRegisterDeviceInterface failed (0x%08X)\n", status));
        return status;
    }

    status = IoSetDeviceInterfaceState(SymbolicLinkName, TRUE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("I2CCTRL_DPI: IoSetDeviceInterfaceState failed (0x%08X)\n", status));
        return status;
    }

    /* ASCII-only log */
    KdPrint(("I2CCTRL_DPI: HID interface registered successfully\n"));

    return status;
}

/*
 * Unregister HID-over-I2C device interface.
 */
VOID
I2cCtrl_DpiUnregisterInterface(
    PUNICODE_STRING SymbolicLinkName
    )
{
    if (SymbolicLinkName == NULL) {
        return;
    }

    IoSetDeviceInterfaceState(SymbolicLinkName, FALSE);

    /* ASCII-only log */
    KdPrint(("I2CCTRL_DPI: HID interface unregistered\n"));
}
