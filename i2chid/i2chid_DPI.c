/* -----------------------------------------------------------------------
   i2chid_DPI.c - HID-over-I2C Function Driver DPI helpers (final, safe with hidclass.h)
   ----------------------------------------------------------------------- */

#include "i2chid_spinlock_fix.h"
#include <hidclass.h>     /* brings in GUID_DEVINTERFACE_HID and other HID GUIDs */
#include "i2chid_DPI.h"

/* Pool tag for any DPI-related allocations */
#define I2CHID_DPI_TAG 'hDPI'

/*
 * Initialize HID DPI context from bus-provided DPI block.
 * Copies all relevant fields into the function driver's DPI structure.
 */
NTSTATUS
I2cHid_DpiInitializeFromBus(
    IN PI2CCTRL_DPI BusDpi,
    OUT PI2CCTRL_DPI HidDpi
    )
{
    if (BusDpi == NULL || HidDpi == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Basic identifiers */
    HidDpi->VendorId               = BusDpi->VendorId;
    HidDpi->ProductId              = BusDpi->ProductId;
    HidDpi->VersionNumber          = BusDpi->VersionNumber;

    /* HID descriptors */
    HidDpi->HidDescriptor          = BusDpi->HidDescriptor;
    HidDpi->HidDescriptorLength    = BusDpi->HidDescriptorLength;
    HidDpi->ReportDescriptor       = BusDpi->ReportDescriptor;
    HidDpi->ReportDescriptorLength = BusDpi->ReportDescriptorLength;

    /* Report characteristics */
    HidDpi->ReportLength           = BusDpi->ReportLength;

    /* I2C addressing */
    HidDpi->I2cAddr7Bit            = BusDpi->I2cAddr7Bit;

    /* Optional configuration defaults */
    HidDpi->PalmThreshold          = BusDpi->PalmThreshold;
    HidDpi->ScrollScale            = BusDpi->ScrollScale;
    HidDpi->TapTimeMs              = BusDpi->TapTimeMs;

    HidDpi->MaxX                   = BusDpi->MaxX;
    HidDpi->MaxY                   = BusDpi->MaxY;
    HidDpi->Sensitivity            = BusDpi->Sensitivity;

    /* Power/idle defaults */
    HidDpi->WakeEnabled            = BusDpi->WakeEnabled;
    HidDpi->IdleTimeoutMs          = BusDpi->IdleTimeoutMs;

    return STATUS_SUCCESS;
}

/*
 * Apply registry policy overrides to HID DPI context.
 * Reads DWORD values from Parameters key and applies them.
 */
NTSTATUS
I2cHid_DpiApplyRegistryPolicy(
    IN OUT PI2CCTRL_DPI HidDpi,
    IN PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS status;
    UNICODE_STRING paramsPath;
    RTL_QUERY_REGISTRY_TABLE query[6];
    ULONG palmThreshold, scrollScale, tapTime, sensitivity, idleTimeout, wakeEnabled;
    ULONG i;

    if (HidDpi == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    UNREFERENCED_PARAMETER(RegistryPath);

    /* Initialize local copies with current values */
    palmThreshold = HidDpi->PalmThreshold;
    scrollScale   = HidDpi->ScrollScale;
    tapTime       = HidDpi->TapTimeMs;
    sensitivity   = HidDpi->Sensitivity;
    idleTimeout   = HidDpi->IdleTimeoutMs;
    wakeEnabled   = HidDpi->WakeEnabled;

    RtlInitUnicodeString(&paramsPath,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\i2chid\\Parameters");

    for (i = 0; i < 6; i++) {
        RtlZeroMemory(&query[i], sizeof(query[i]));
    }

    query[0].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[0].Name          = L"PalmThreshold";
    query[0].EntryContext  = &palmThreshold;
    query[0].DefaultType   = REG_DWORD;
    query[0].DefaultData   = &palmThreshold;
    query[0].DefaultLength = sizeof(ULONG);

    query[1].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[1].Name          = L"ScrollScale";
    query[1].EntryContext  = &scrollScale;
    query[1].DefaultType   = REG_DWORD;
    query[1].DefaultData   = &scrollScale;
    query[1].DefaultLength = sizeof(ULONG);

    query[2].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[2].Name          = L"TapTimeMs";
    query[2].EntryContext  = &tapTime;
    query[2].DefaultType   = REG_DWORD;
    query[2].DefaultData   = &tapTime;
    query[2].DefaultLength = sizeof(ULONG);

    query[3].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[3].Name          = L"Sensitivity";
    query[3].EntryContext  = &sensitivity;
    query[3].DefaultType   = REG_DWORD;
    query[3].DefaultData   = &sensitivity;
    query[3].DefaultLength = sizeof(ULONG);

    query[4].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[4].Name          = L"IdleTimeoutMs";
    query[4].EntryContext  = &idleTimeout;
    query[4].DefaultType   = REG_DWORD;
    query[4].DefaultData   = &idleTimeout;
    query[4].DefaultLength = sizeof(ULONG);

    query[5].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[5].Name          = L"WakeEnabled";
    query[5].EntryContext  = &wakeEnabled;
    query[5].DefaultType   = REG_DWORD;
    query[5].DefaultData   = &wakeEnabled;
    query[5].DefaultLength = sizeof(ULONG);

    status = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
                                    paramsPath.Buffer,
                                    query,
                                    NULL,
                                    NULL);

    if (!NT_SUCCESS(status)) {
        return status;
    }

    HidDpi->PalmThreshold  = palmThreshold;
    HidDpi->ScrollScale    = scrollScale;
    HidDpi->TapTimeMs      = tapTime;
    HidDpi->Sensitivity    = sensitivity;
    HidDpi->IdleTimeoutMs  = idleTimeout;
    HidDpi->WakeEnabled    = (wakeEnabled != 0) ? 1 : 0;

    return STATUS_SUCCESS;
}

/*
 * Register HID interface for function driver.
 * Uses GUID_DEVINTERFACE_HID from hidclass.h.
 */
NTSTATUS
I2cHid_DpiRegisterInterface(
    IN PDEVICE_OBJECT DeviceObject,
    OUT PUNICODE_STRING SymbolicLinkName
    )
{
    NTSTATUS status;

    if (DeviceObject == NULL || SymbolicLinkName == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    status = IoRegisterDeviceInterface(DeviceObject,
                                       &GUID_DEVINTERFACE_HID,
                                       NULL,
                                       SymbolicLinkName);
    if (NT_SUCCESS(status)) {
        IoSetDeviceInterfaceState(SymbolicLinkName, TRUE);
        KdPrint(("I2CHID_DPI: HID interface registered\n"));
    } else {
        KdPrint(("I2CHID_DPI: HID interface registration failed (0x%08X)\n", status));
    }
    return status;
}

/*
 * Unregister HID interface.
 */
VOID
I2cHid_DpiUnregisterInterface(
    IN PUNICODE_STRING SymbolicLinkName
    )
{
    if (SymbolicLinkName != NULL) {
        IoSetDeviceInterfaceState(SymbolicLinkName, FALSE);
        KdPrint(("I2CHID_DPI: HID interface unregistered\n"));
    }
}
