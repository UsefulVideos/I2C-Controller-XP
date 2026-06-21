/* i2cctrl_hal_ops.c
 *
 * Shared HAL logic for I2C-Controller-XP
 * Contains:
 *   - MMIO mapping helpers
 *   - Backend installer
 */

#include <ntddk.h>
#include "i2cctrl_hal_ops.h"
#include "i2cctrl_hal_caps.h"
#include "i2cctrl_ext.h"

/* ---------------------------------------------------------------------------
 * External helpers / globals from the rest of the driver
 * --------------------------------------------------------------------------- */

typedef struct _I2C_REG_MAP {
    ULONG ControlReg;
    ULONG StatusReg;
    ULONG DataReg;
    ULONG ClockReg;
    ULONG Quirks;
    ULONG BsodQuirks;
} I2C_REG_MAP, *PI2C_REG_MAP;

/* ---------------------------------------------------------------------------
 * MMIO mapping helpers
 * --------------------------------------------------------------------------- */

NTSTATUS
I2cCtrl_MapMmio(
    PI2CCTRL_FDO devctx,
    PCM_RESOURCE_LIST translated
    )
{
    PCM_FULL_RESOURCE_DESCRIPTOR full;
    PCM_PARTIAL_RESOURCE_LIST partial;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;
    ULONG i;

    if (devctx == NULL || translated == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (devctx->MmioBase != NULL && devctx->MmioLength != 0) {
        return STATUS_SUCCESS;
    }

    full = &translated->List[0];
    partial = &full->PartialResourceList;

    for (i = 0; i < partial->Count; i++) {

        desc = &partial->PartialDescriptors[i];

        if (desc->Type == CmResourceTypeMemory &&
            desc->u.Memory.Length != 0) {

            devctx->MmioPhys   = desc->u.Memory.Start;
            devctx->MmioLength = desc->u.Memory.Length;

            devctx->MmioBase = (PUCHAR)MmMapIoSpace(
                devctx->MmioPhys,
                devctx->MmioLength,
                MmNonCached
            );

            if (devctx->MmioBase == NULL) {
                devctx->MmioLength = 0;
                I2cCtrl_Log("MapMmio: MmMapIoSpace FAILED\n");
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            I2cCtrl_Log("MapMmio: mapped %lu bytes at %p\n",
                        devctx->MmioLength, devctx->MmioBase);
            return STATUS_SUCCESS;
        }
    }

    I2cCtrl_Log("MapMmio: no memory resource found\n");
    return STATUS_NOT_FOUND;
}

VOID
I2cCtrl_UnmapMmio(
    PI2CCTRL_FDO devctx
    )
{
    if (devctx == NULL) {
        return;
    }

    if (devctx->MmioBase != NULL && devctx->MmioLength != 0) {

        MmUnmapIoSpace(devctx->MmioBase, devctx->MmioLength);

        I2cCtrl_Log("UnmapMmio: unmapped %p len=%lu\n",
                    devctx->MmioBase, devctx->MmioLength);

        devctx->MmioBase   = NULL;
        devctx->MmioLength = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Backend installer
 * --------------------------------------------------------------------------- */

VOID
I2cCtrl_InstallBackend(PI2CCTRL_FDO devctx)
{
    const I2CCTRL_DEVICE_ID* id;

    if (devctx == NULL) {
        I2cCtrl_Log("InstallBackend: NULL devctx\n");
        return;
    }

    id = I2cCtrl_FindControllerId(devctx->PnpId);
    if (id == NULL) {
        I2cCtrl_Log("InstallBackend: no controller profile for %S\n",
                    devctx->PnpId);
        devctx->Ops  = NULL;
        devctx->Caps = NULL;
        return;
    }

    switch (id->BackendType) {

        case BACKEND_DW:
            devctx->Ops  = &DwI2cOps;
            devctx->Caps = &DwI2cOps.Caps;
            break;

        case BACKEND_ACPI:
            devctx->Ops  = &AcpiI2cOps;
            devctx->Caps = &AcpiI2cCaps;
            break;

        case BACKEND_REALTEK:
            devctx->Ops  = &RealtekI2cOps;
            devctx->Caps = &RealtekI2cCaps;
            break;

        case BACKEND_QUALCOMM:
            devctx->Ops  = &QualcommI2cOps;
            devctx->Caps = &QualcommI2cCaps;
            break;

        case BACKEND_NVIDIA:
            devctx->Ops  = &NvidiaI2cOps;
            devctx->Caps = &NvidiaI2cCaps;
            break;

        case BACKEND_APPLE:
            devctx->Ops  = &AppleI2cOps;
            devctx->Caps = &AppleI2cCaps;
            break;

        case BACKEND_BROADCOM:
            devctx->Ops  = &BroadcomI2cOps;
            devctx->Caps = &BroadcomI2cCaps;
            break;

        case BACKEND_MEDIATEK:
            devctx->Ops  = &MediatekI2cOps;
            devctx->Caps = &MediatekI2cCaps;
            break;

        case BACKEND_SAMSUNG:
            devctx->Ops  = &SamsungI2cOps;
            devctx->Caps = &SamsungI2cCaps;
            break;

        case BACKEND_VIA:
            devctx->Ops  = &ViaI2cOps;
            devctx->Caps = &ViaI2cCaps;
            break;

        case BACKEND_SIFIVE:
            devctx->Ops  = &SiFiveI2cOps;
            devctx->Caps = &SiFiveI2cCaps;
            break;

        case BACKEND_ROCKCHIP:
            devctx->Ops  = &RockchipI2cOps;
            devctx->Caps = &RockchipI2cCaps;
            break;

        case BACKEND_AMD_VENDOR:
            devctx->Ops  = &AmdVendorI2cOps;
            devctx->Caps = &AmdVendorI2cCaps;
            break;

        case BACKEND_INTEL_VENDOR:
            devctx->Ops  = &IntelVendorI2cOps;
            devctx->Caps = &IntelVendorI2cCaps;
            break;

        case BACKEND_SOFTWARE:
            devctx->Ops  = &SoftI2cOps;
            devctx->Caps = &SoftI2cCaps;
            break;

        default:
            I2cCtrl_Log("InstallBackend: unknown backend type %lu\n",
                        id->BackendType);
            devctx->Ops  = NULL;
            devctx->Caps = NULL;
            break;
    }
}
