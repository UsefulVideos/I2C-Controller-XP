/* i2cctrl_emu.c
 * Root-enumerated I2C controller emulator for ASUS X509FA (9DE9) precision touchpad.
 * Feature-complete: DriverEntry, AddDevice, dispatch routines, ops wiring, FIFO helpers.
 * XP-BSOD-safe, WinDDK-compiler-safe, verbose logging.
 */

#include <ntddk.h>
#include <wdmsec.h>
#include "..\i2cctrl\i2cctrl_ext.h"
#include "i2cctrl_emu_ext.h"

/* ---------------------------------------------------------------------------
 * Driver prototypes
 * --------------------------------------------------------------------------- */
DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE I2CCTRL_EMU_AddDevice;
DRIVER_UNLOAD I2CCTRL_EMU_Unload;

NTSTATUS I2CCTRL_EMU_DispatchPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2CCTRL_EMU_DispatchPower(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2CCTRL_EMU_DispatchIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2CCTRL_EMU_DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* ---------------------------------------------------------------------------
 * DriverEntry
 * --------------------------------------------------------------------------- */
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    UNREFERENCED_PARAMETER(RegistryPath);

    DriverObject->DriverUnload                         = I2CCTRL_EMU_Unload;
    DriverObject->DriverExtension->AddDevice           = I2CCTRL_EMU_AddDevice;
    DriverObject->MajorFunction[IRP_MJ_PNP]            = I2CCTRL_EMU_DispatchPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER]          = I2CCTRL_EMU_DispatchPower;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = I2CCTRL_EMU_DispatchIoctl;
    DriverObject->MajorFunction[IRP_MJ_CREATE]         = I2CCTRL_EMU_DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = I2CCTRL_EMU_DispatchCreateClose;

    I2cCtrl_Emu_Log("DriverEntry OK\n");
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Unload
 * --------------------------------------------------------------------------- */
VOID
I2CCTRL_EMU_Unload(
    PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
    I2cCtrl_Emu_Log("Unload\n");
}

/* Global verbose flag */
ULONG g_EmuVerbose = 1; /* default on; set to 0 to silence */

static VOID I2CCTRL_EMU_ReadVerboseFlag(VOID)
{
    /* Optional: read HKLM\System\CCS\Services\I2CCTRL_EMU\Parameters\Verbose (DWORD) */
    /* XP-safe shortcut: leave default. Add full query later if needed. */
}


/* ---------------------------------------------------------------------------
 * AddDevice (XP-safe, C89-compliant, WDK-safe, verbose-compliant)
 * Creates FDO, attaches to stack, and enumerates a single synthetic
 * ACPI\PNP0C50 child PDO for i2chid.sys testing.
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_AddDevice(
    PDRIVER_OBJECT DriverObject,
    PDEVICE_OBJECT PhysicalDeviceObject
    )
{
    NTSTATUS status;
    PDEVICE_OBJECT fdo;
    PI2CCTRL_EMU_FDO_EXT ext;
    UNICODE_STRING devName;
    UNICODE_STRING dosName;
    ULONG i;
    static const WCHAR idTouch[] = L"ACPI\\PNP0C50";
    const PWSTR childIds[3] = { (PWSTR)idTouch, NULL, NULL };
    PDEVICE_OBJECT lower;

    /* Create the Functional Device Object (FDO) */
    RtlInitUnicodeString(&devName, I2CCTRL_EMU_DEVICE_NAME);
    status = IoCreateDevice(
        DriverObject,
        sizeof(I2CCTRL_EMU_FDO_EXT),
        &devName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &fdo
    );
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Emu_Log("AddDevice: IoCreateDevice(FDO) failed status=0x%08lX\n", status);
        return status;
    }

    /* Initialize FDO extension */
    ext = (PI2CCTRL_EMU_FDO_EXT)fdo->DeviceExtension;
    RtlZeroMemory(ext, sizeof(I2CCTRL_EMU_FDO_EXT));
    ext->Self               = fdo;
    ext->Enabled            = FALSE;
    ext->HidAddr            = I2CCTRL_EMU_DEFAULT_ADDR;
    ext->AcpiPdo            = NULL;
    ext->AcpiInterfaceReady = FALSE;
    ext->InstanceId         = 0;
    ext->ParentPdo          = PhysicalDeviceObject;
    ext->LowerDevice        = NULL;

    for (i = 0; i < 3; i++) {
        ext->ChildPdos[i]     = NULL;
        ext->ChildIds[i]      = NULL;
        ext->ChildReported[i] = FALSE;
    }

    /* Attach to underlying stack so PnP/Power IRPs flow through us */
    lower = IoAttachDeviceToDeviceStack(fdo, PhysicalDeviceObject);
    if (lower == NULL) {
        I2cCtrl_Emu_Log("AddDevice: IoAttachDeviceToDeviceStack FAILED\n");
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }
    ext->LowerDevice = lower;
    I2cCtrl_Emu_Log("AddDevice: Attached FDO to stack, lower=%p\n", lower);

    I2CCTRL_EMU_HidInitProfile(ext);

    /* FDO flags */
    fdo->DeviceType      = FILE_DEVICE_UNKNOWN;
    fdo->Characteristics = 0;
    fdo->Flags |= DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    /* Optional DOS link */
    RtlInitUnicodeString(&dosName, I2CCTRL_EMU_DOSLINK_NAME);
    ext->DosLink = dosName;
    status = IoCreateSymbolicLink(&dosName, &devName);
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Emu_Log("AddDevice: IoCreateSymbolicLink failed status=0x%08lX\n", status);
    }

    /* ACPI-assisted init */
    (VOID)I2CCTRL_EMU_AcpiInitialize(ext);
    (VOID)I2CCTRL_EMU_AcpiPrimeChildren(ext, childIds, 1);

    /* Create single synthetic ACPI\PNP0C50 child PDO */
    {
        NTSTATUS s;
        PDEVICE_OBJECT pdo;
        PI2CCTRL_EMU_PDO_EXT pext;

        pdo = NULL;
        s = IoCreateDevice(
                DriverObject,
                sizeof(I2CCTRL_EMU_PDO_EXT),
                NULL,
                FILE_DEVICE_UNKNOWN,
                0,
                FALSE,
                &pdo
            );
        if (!NT_SUCCESS(s)) {
            I2cCtrl_Emu_Log("AddDevice: IoCreateDevice(PDO[0]) failed 0x%08lX\n", s);
        } else {
            /* Init PDO extension (generic fields) */
            pext = (PI2CCTRL_EMU_PDO_EXT)pdo->DeviceExtension;
            RtlZeroMemory(pext, sizeof(*pext));
            pext->Parent     = ext;
            pext->Index      = 0;
            pext->HardwareId = childIds[0];
            pext->InstanceId = NULL;
            pext->Reported   = FALSE;
            pext->Flags      = 0;

            /* No real PCI bus numbering; keep zeros */
            pext->BusNumber      = 0;
            pext->DeviceNumber   = 0;
            pext->FunctionNumber = 0;

            /* Minimal config space, just enough for READ_CONFIG / WRITE_CONFIG */
            RtlZeroMemory(&pext->ConfigSpace, sizeof(pext->ConfigSpace));
            pext->ConfigSpace.VendorID      = 0x0000;
            pext->ConfigSpace.DeviceID      = 0x0000;
            pext->ConfigSpace.BaseClass     = 0x00;
            pext->ConfigSpace.SubClass      = 0x00;
            pext->ConfigSpace.InterruptLine = 0xFF;
            pext->ConfigSpace.InterruptPin  = 0x00;

            pdo->DeviceType      = fdo->DeviceType;
            pdo->Characteristics = fdo->Characteristics;
            pdo->Flags |= DO_POWER_PAGABLE;
            pdo->Flags &= ~DO_DEVICE_INITIALIZING;

            ext->ChildPdos[0]     = pdo;
            ext->ChildIds[0]      = childIds[0];
            ext->ChildReported[0] = FALSE;

            (VOID)I2CCTRL_EMU_AcpiAttachChildProperties(ext, pdo, childIds[0], 0);

            I2cCtrl_Emu_Log("AddDevice: Created synthetic ACPI child PDO[0] ID=%ws\n",
                            childIds[0]);
        }
    }

    /* Notify PnP to query BusRelations */
    if (ext->ParentPdo != NULL) {
        I2cCtrl_Emu_Log("AddDevice: IoInvalidateDeviceRelations(BusRelations)\n");
        IoInvalidateDeviceRelations(ext->ParentPdo, BusRelations);
    }

    I2cCtrl_Emu_Log("AddDevice OK, HidAddr=0x%02X\n", (unsigned)ext->HidAddr);
    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
 * Dispatch: CREATE/CLOSE (XP-safe, C89-compliant, WDK-safe, verbose-compliant)
 * Handles both FDO and PDO objects
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_DispatchCreateClose(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    PIO_STACK_LOCATION isl;

    isl = IoGetCurrentIrpStackLocation(Irp);

    /* Identify whether this is the bus FDO or a child PDO */
    if (DeviceObject != NULL) {
        PI2CCTRL_EMU_FDO_EXT fext = (PI2CCTRL_EMU_FDO_EXT)DeviceObject->DeviceExtension;
        if (fext != NULL && fext->Self == DeviceObject) {
            /* FDO path */
            if (isl->MajorFunction == IRP_MJ_CREATE) {
                I2cCtrl_Emu_Log("FDO DispatchCreateClose: IRP_MJ_CREATE\n");
            } else if (isl->MajorFunction == IRP_MJ_CLOSE) {
                I2cCtrl_Emu_Log("FDO DispatchCreateClose: IRP_MJ_CLOSE\n");
            } else {
                I2cCtrl_Emu_Log("FDO DispatchCreateClose: unexpected major=%u\n",
                                isl->MajorFunction);
            }
        } else {
            /* PDO path */
            PI2CCTRL_EMU_PDO_EXT pext = (PI2CCTRL_EMU_PDO_EXT)DeviceObject->DeviceExtension;
            if (isl->MajorFunction == IRP_MJ_CREATE) {
                I2cCtrl_Emu_Log("PDO[%lu] DispatchCreateClose: IRP_MJ_CREATE\n", pext->Index);
            } else if (isl->MajorFunction == IRP_MJ_CLOSE) {
                I2cCtrl_Emu_Log("PDO[%lu] DispatchCreateClose: IRP_MJ_CLOSE\n", pext->Index);
            } else {
                I2cCtrl_Emu_Log("PDO[%lu] DispatchCreateClose: unexpected major=%u\n",
                                pext->Index, isl->MajorFunction);
            }
        }
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0UL;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}


/* ---------------------------------------------------------------------------
 * Dispatch: PNP (XP-safe, C89-compliant, WDK-safe) - bus FDO and child PDOs
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_DispatchPnp(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    NTSTATUS status;
    PIO_STACK_LOCATION isl;
    PI2CCTRL_EMU_FDO_EXT fext;
    PI2CCTRL_EMU_PDO_EXT pext;
    BOOLEAN isFdo;
    ULONG i;
    ULONG j;
    ULONG k;

    status = STATUS_SUCCESS;
    isl    = IoGetCurrentIrpStackLocation(Irp);
    fext   = (PI2CCTRL_EMU_FDO_EXT)DeviceObject->DeviceExtension;
    isFdo  = (fext != NULL && fext->Self == DeviceObject);

    if (isFdo) {
        /* ---------------- FDO path ---------------- */
        switch (isl->MinorFunction) {

        case IRP_MN_START_DEVICE:
            I2cCtrl_Emu_Log("FDO PnP START\n");
            fext->Enabled = TRUE;
            break;

        case IRP_MN_STOP_DEVICE:
            I2cCtrl_Emu_Log("FDO PnP STOP\n");
            fext->Enabled = FALSE;
            break;

        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            ULONG type;

            type = isl->Parameters.QueryDeviceRelations.Type;

            if (type == BusRelations) {
                PDEVICE_RELATIONS rel;
                ULONG count;
                ULONG j;
                ULONG idx;
                PDEVICE_OBJECT p;

                I2cCtrl_Emu_Log("FDO PnP QUERY_DEVICE_RELATIONS (BusRelations)\n");

                count = 0;
                for (j = 0; j < 3; j++) {
                    if (fext->ChildPdos[j] != NULL) {
                        count++;
                    }
                }

                rel = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(
                          PagedPool,
                          sizeof(DEVICE_RELATIONS) +
                              (count ? (count - 1) * sizeof(PDEVICE_OBJECT) : 0),
                          'RlcI'
                      );
                if (rel == NULL) {
                    status = STATUS_INSUFFICIENT_RESOURCES;
                    I2cCtrl_Emu_Log("FDO PnP QUERY_DEVICE_RELATIONS: allocation failed\n");
                } else {
                    rel->Count = count;
                    idx = 0;

                    for (j = 0; j < 3; j++) {
                        p = fext->ChildPdos[j];
                        if (p != NULL) {
                            ObfReferenceObject(p);
                            rel->Objects[idx++] = p;
                            fext->ChildReported[j] = TRUE;
                        }
                    }

                    Irp->IoStatus.Information = (ULONG_PTR)rel;
                    status = STATUS_SUCCESS;

                    I2cCtrl_Emu_Log(
                        "FDO PnP QUERY_DEVICE_RELATIONS: reported %lu child(ren)\n",
                        count
                    );
                }
            } else {
                I2cCtrl_Emu_Log(
                    "FDO PnP QUERY_DEVICE_RELATIONS: type=%u (ignored)\n",
                    type
                );
            }
            break;
        }

        case IRP_MN_QUERY_DEVICE_TEXT:
            if (isl->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription) {
                PWSTR text;
                size_t bytes;
                PWSTR out;

                text  = L"Emulated PCI Root Bus";
                bytes = (wcslen(text) + 1) * sizeof(WCHAR);

                out = (PWSTR)ExAllocatePoolWithTag(PagedPool, bytes, 'tDcI');
                if (out != NULL) {
                    RtlCopyMemory(out, text, bytes);
                    Irp->IoStatus.Information = (ULONG_PTR)out;
                    status = STATUS_SUCCESS;
                    I2cCtrl_Emu_Log("FDO PnP QUERY_DEVICE_TEXT (Description)\n");
                } else {
                    status = STATUS_INSUFFICIENT_RESOURCES;
                }
            }
            break;

        case IRP_MN_QUERY_CAPABILITIES:
            if (isl->Parameters.DeviceCapabilities.Capabilities != NULL) {
                PDEVICE_CAPABILITIES caps;

                caps = isl->Parameters.DeviceCapabilities.Capabilities;
                RtlZeroMemory(caps, sizeof(DEVICE_CAPABILITIES));

                caps->Size              = sizeof(DEVICE_CAPABILITIES);
                caps->Version           = 1;
                caps->UniqueID          = TRUE;
                caps->SilentInstall     = TRUE;
                caps->SurpriseRemovalOK = TRUE;

                status = STATUS_SUCCESS;
                I2cCtrl_Emu_Log("FDO PnP QUERY_CAPABILITIES\n");
            } else {
                status = STATUS_INVALID_PARAMETER;
            }
            break;

        case IRP_MN_QUERY_BUS_INFORMATION:
        {
            PPNP_BUS_INFORMATION busInfo;

            I2cCtrl_Emu_Log("FDO PnP QUERY_BUS_INFORMATION\n");

            busInfo = (PPNP_BUS_INFORMATION)ExAllocatePoolWithTag(
                          PagedPool,
                          sizeof(PNP_BUS_INFORMATION),
                          'sBuI'
                      );
            if (busInfo == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                RtlZeroMemory(busInfo, sizeof(PNP_BUS_INFORMATION));

                /* Present this FDO as a PCI-like bus */
                busInfo->BusTypeGuid   = GUID_BUS_TYPE_PCI;
                busInfo->LegacyBusType = PCIBus;
                busInfo->BusNumber     = 0;

                Irp->IoStatus.Information = (ULONG_PTR)busInfo;
                status = STATUS_SUCCESS;

                I2cCtrl_Emu_Log("FDO PnP QUERY_BUS_INFORMATION -> PCI bus #0\n");
            }
            break;
        }

        case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
            /* Just pass through for the root bus FDO */
            I2cCtrl_Emu_Log("FDO PnP FILTER_RESOURCE_REQUIREMENTS (pass through)\n");
            break;

        case IRP_MN_QUERY_INTERFACE:
            /* Let lower drivers handle any interface requests */
            I2cCtrl_Emu_Log("FDO PnP QUERY_INTERFACE (pass down)\n");
            break;

        case IRP_MN_REMOVE_DEVICE:
            I2cCtrl_Emu_Log("FDO PnP REMOVE\n");

            if (fext->DosLink.Buffer != NULL) {
                (VOID)IoDeleteSymbolicLink(&fext->DosLink);
                RtlZeroMemory(&fext->DosLink, sizeof(fext->DosLink));
            }

            for (i = 0; i < 3; i++) {
                fext->ChildPdos[i]     = NULL;
                fext->ChildIds[i]      = NULL;
                fext->ChildReported[i] = FALSE;
            }

            if (fext->LowerDevice != NULL) {
                IoDetachDevice(fext->LowerDevice);
                fext->LowerDevice = NULL;
            }

            IoDeleteDevice(DeviceObject);
            break;

        default:
            I2cCtrl_Emu_Log("FDO PnP minor=%u (pass down)\n", isl->MinorFunction);
            break;
        }

        /* For the FDO, pass the IRP down the stack after our processing. */
        Irp->IoStatus.Status = status;
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(fext->LowerDevice, Irp);
    } else {
        /* ---------------- PDO path ---------------- */
        pext = (PI2CCTRL_EMU_PDO_EXT)DeviceObject->DeviceExtension;

        switch (isl->MinorFunction) {

        case IRP_MN_QUERY_ID:
            switch (isl->Parameters.QueryId.IdType) {
            case BusQueryHardwareIDs:
            case BusQueryCompatibleIDs:
            {
                size_t cch;
                size_t bytes;
                PWSTR multi;

                cch   = wcslen(pext->HardwareId) + 1;   /* include NUL */
                bytes = (cch + 1) * sizeof(WCHAR);      /* MULTI_SZ needs extra NUL */

                multi = (PWSTR)ExAllocatePoolWithTag(PagedPool, bytes, 'dIcI');
                if (multi == NULL) {
                    status = STATUS_INSUFFICIENT_RESOURCES;
                } else {
                    RtlZeroMemory(multi, bytes);
                    RtlCopyMemory(multi, pext->HardwareId, cch * sizeof(WCHAR));
                    Irp->IoStatus.Information = (ULONG_PTR)multi;
                    status = STATUS_SUCCESS;
                    I2cCtrl_Emu_Log("PDO[%lu] QUERY_ID -> %ws\n",
                                    pext->Index, pext->HardwareId);
                }
                break;
            }
            case BusQueryInstanceID:
            {
                WCHAR tmp[16];
                UNICODE_STRING us;
                PWSTR out;

                RtlInitEmptyUnicodeString(&us, tmp, sizeof(tmp));
                if (!NT_SUCCESS(RtlIntegerToUnicodeString(pext->Index, 10, &us))) {
                    status = STATUS_UNSUCCESSFUL;
                } else {
                    out = (PWSTR)ExAllocatePoolWithTag(
                              PagedPool,
                              us.Length + sizeof(WCHAR),
                              'nIcI'
                          );
                    if (out == NULL) {
                        status = STATUS_INSUFFICIENT_RESOURCES;
                    } else {
                        RtlZeroMemory(out, us.Length + sizeof(WCHAR));
                        RtlCopyMemory(out, us.Buffer, us.Length);
                        Irp->IoStatus.Information = (ULONG_PTR)out;
                        status = STATUS_SUCCESS;
                    }
                }
                break;
            }
            default:
                status = STATUS_NOT_SUPPORTED;
                break;
            }
            break;

        case IRP_MN_QUERY_DEVICE_RELATIONS:
            if (isl->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
                PDEVICE_RELATIONS rel;

                rel = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(
                          PagedPool,
                          sizeof(DEVICE_RELATIONS),
                          'rDcI'
                      );
                if (rel == NULL) {
                    status = STATUS_INSUFFICIENT_RESOURCES;
                } else {
                    rel->Count = 1;
                    rel->Objects[0] = DeviceObject;
                    ObfReferenceObject(DeviceObject);
                    Irp->IoStatus.Information = (ULONG_PTR)rel;
                    status = STATUS_SUCCESS;
                    I2cCtrl_Emu_Log("PDO[%lu] QUERY_DEVICE_RELATIONS (Target)\n",
                                    pext->Index);
                }
            } else if (isl->Parameters.QueryDeviceRelations.Type == RemovalRelations) {
                /* No special removal relations */
                Irp->IoStatus.Information = 0;
                status = STATUS_SUCCESS;
                I2cCtrl_Emu_Log("PDO[%lu] QUERY_DEVICE_RELATIONS (Removal)\n",
                                pext->Index);
            } else {
                I2cCtrl_Emu_Log("PDO[%lu] QUERY_DEVICE_RELATIONS type=%u (ignored)\n",
                                pext->Index,
                                isl->Parameters.QueryDeviceRelations.Type);
                status = STATUS_SUCCESS;
            }
            break;

        case IRP_MN_QUERY_DEVICE_TEXT:
            if (isl->Parameters.QueryDeviceText.DeviceTextType ==
                DeviceTextDescription) {
                PWSTR text;
                size_t bytes;
                PWSTR out;

                text  = L"Emulated PCI Device";
                bytes = (wcslen(text) + 1) * sizeof(WCHAR);

                out = (PWSTR)ExAllocatePoolWithTag(PagedPool, bytes, 'tDcI');
                if (out == NULL) {
                    status = STATUS_INSUFFICIENT_RESOURCES;
                } else {
                    RtlCopyMemory(out, text, bytes);
                    Irp->IoStatus.Information = (ULONG_PTR)out;
                    status = STATUS_SUCCESS;
                    I2cCtrl_Emu_Log("PDO[%lu] QUERY_DEVICE_TEXT (Description)\n",
                                    pext->Index);
                }
            } else {
                status = STATUS_SUCCESS;
            }
            break;

        case IRP_MN_READ_CONFIG:
        {
            ULONG which;
            ULONG offset;
            ULONG length;
            PUCHAR dest;

            which  = isl->Parameters.ReadWriteConfig.WhichSpace;
            offset = isl->Parameters.ReadWriteConfig.Offset;
            length = isl->Parameters.ReadWriteConfig.Length;
            dest   = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;

            if (which != PCI_WHICHSPACE_CONFIG || dest == NULL) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                if (offset >= sizeof(pext->ConfigSpace)) {
                    length = 0;
                } else if (offset + length > sizeof(pext->ConfigSpace)) {
                    length = sizeof(pext->ConfigSpace) - offset;
                }

                if (length > 0) {
                    RtlCopyMemory(
                        dest,
                        ((PUCHAR)&pext->ConfigSpace) + offset,
                        length
                    );
                }

                Irp->IoStatus.Information = length;
                status = STATUS_SUCCESS;
            }
            break;
        }

        case IRP_MN_WRITE_CONFIG:
        {
            ULONG which;
            ULONG offset;
            ULONG length;
            PUCHAR src;

            which  = isl->Parameters.ReadWriteConfig.WhichSpace;
            offset = isl->Parameters.ReadWriteConfig.Offset;
            length = isl->Parameters.ReadWriteConfig.Length;
            src    = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;

            if (which != PCI_WHICHSPACE_CONFIG || src == NULL) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                if (offset >= sizeof(pext->ConfigSpace)) {
                    length = 0;
                } else if (offset + length > sizeof(pext->ConfigSpace)) {
                    length = sizeof(pext->ConfigSpace) - offset;
                }

                if (length > 0) {
                    /* For now, allow writes to the whole config space */
                    RtlCopyMemory(
                        ((PUCHAR)&pext->ConfigSpace) + offset,
                        src,
                        length
                    );
                }

                Irp->IoStatus.Information = length;
                status = STATUS_SUCCESS;
            }
            break;
        }

        case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
        {
            PIO_RESOURCE_REQUIREMENTS_LIST req;
            ULONG size;

            size = sizeof(IO_RESOURCE_REQUIREMENTS_LIST);
            req = (PIO_RESOURCE_REQUIREMENTS_LIST)ExAllocatePoolWithTag(
                      PagedPool,
                      size,
                      'qRcI');
            if (req == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                RtlZeroMemory(req, size);
                req->AlternativeLists = 0;
                req->ListSize         = size;
                Irp->IoStatus.Information = (ULONG_PTR)req;
                status = STATUS_SUCCESS;
                I2cCtrl_Emu_Log("PDO[%lu] QUERY_RESOURCE_REQUIREMENTS (none)\n",
                                pext->Index);
            }
            break;
        }

        case IRP_MN_QUERY_RESOURCES:
        {
            PCM_RESOURCE_LIST res;
            ULONG size;

            size = sizeof(CM_RESOURCE_LIST);
            res = (PCM_RESOURCE_LIST)ExAllocatePoolWithTag(
                      PagedPool,
                      size,
                      'rRcI');
            if (res == NULL) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                RtlZeroMemory(res, size);
                res->Count = 0;
                Irp->IoStatus.Information = (ULONG_PTR)res;
                status = STATUS_SUCCESS;
                I2cCtrl_Emu_Log("PDO[%lu] QUERY_RESOURCES (none)\n",
                                pext->Index);
            }
            break;
        }

        case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
            /* No filtering, just succeed */
            I2cCtrl_Emu_Log("PDO[%lu] FILTER_RESOURCE_REQUIREMENTS (none)\n",
                            pext->Index);
            status = STATUS_SUCCESS;
            break;

        case IRP_MN_QUERY_INTERFACE:
            /* We are not exposing any special PCI interfaces yet */
            I2cCtrl_Emu_Log("PDO[%lu] QUERY_INTERFACE (NOT_SUPPORTED)\n",
                            pext->Index);
            status = STATUS_NOT_SUPPORTED;
            break;

        case IRP_MN_START_DEVICE:
        case IRP_MN_QUERY_REMOVE_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
        case IRP_MN_SURPRISE_REMOVAL:
        case IRP_MN_REMOVE_DEVICE:
            status = STATUS_SUCCESS;
            break;

        default:
            I2cCtrl_Emu_Log("PDO[%lu] PnP minor=%u\n",
                            pext->Index, isl->MinorFunction);
            status = STATUS_SUCCESS;
            break;
        }

        Irp->IoStatus.Status = status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }
}

/* ---------------------------------------------------------------------------
 * Dispatch: POWER (XP-safe, C89-compliant, WDK-safe, verbose-compliant)
 * Handles both FDO and PDO objects
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_DispatchPower(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    NTSTATUS status;
    PIO_STACK_LOCATION isl;
    PI2CCTRL_EMU_FDO_EXT fext;
    PI2CCTRL_EMU_PDO_EXT pext;
    BOOLEAN isFdo;
    ULONG i;
    POWER_STATE ps;

    status = STATUS_SUCCESS;
    isl    = IoGetCurrentIrpStackLocation(Irp);

    /* XP/2003 requirement: always start next power IRP */
    PoStartNextPowerIrp(Irp);

    I2cCtrl_Emu_Log("POWER IRP minor=%u\n", isl->MinorFunction);

    fext  = (PI2CCTRL_EMU_FDO_EXT)DeviceObject->DeviceExtension;
    isFdo = (fext != NULL && fext->Self == DeviceObject);

    if (isFdo) {
        /* ---------------- FDO path ---------------- */
        switch (isl->MinorFunction) {
        case IRP_MN_SET_POWER:
            I2cCtrl_Emu_Log("FDO IRP_MN_SET_POWER: type=%u sysState=%u devState=%u\n",
                            isl->Parameters.Power.Type,
                            isl->Parameters.Power.State.SystemState,
                            isl->Parameters.Power.State.DeviceState);

            /* Build POWER_STATE struct */
            ps = isl->Parameters.Power.State;

            /* Propagate to child PDOs */
            for (i = 0; i < 3; i++) {
                if (fext->ChildPdos[i] != NULL) {
                    PoRequestPowerIrp(
                        fext->ChildPdos[i],
                        IRP_MN_SET_POWER,   /* MinorFunction */
                        ps,                 /* POWER_STATE */
                        NULL,               /* CompletionFunction */
                        NULL,               /* Context */
                        NULL                /* Out IRP */
                    );
                    I2cCtrl_Emu_Log("FDO propagated power to child[%lu]\n", i);
                }
            }
            break;

        case IRP_MN_QUERY_POWER:
            I2cCtrl_Emu_Log("FDO IRP_MN_QUERY_POWER\n");
            break;

        case IRP_MN_WAIT_WAKE:
            I2cCtrl_Emu_Log("FDO IRP_MN_WAIT_WAKE (not supported)\n");
            status = STATUS_NOT_SUPPORTED;
            break;

        case IRP_MN_POWER_SEQUENCE:
            I2cCtrl_Emu_Log("FDO IRP_MN_POWER_SEQUENCE (no-op)\n");
            break;

        default:
            I2cCtrl_Emu_Log("FDO Unhandled POWER IRP minor=%u\n", isl->MinorFunction);
            break;
        }
    } else {
        /* ---------------- PDO path ---------------- */
        pext = (PI2CCTRL_EMU_PDO_EXT)DeviceObject->DeviceExtension;

        switch (isl->MinorFunction) {
        case IRP_MN_SET_POWER:
            I2cCtrl_Emu_Log("PDO[%lu] IRP_MN_SET_POWER: type=%u sysState=%u devState=%u\n",
                            pext->Index,
                            isl->Parameters.Power.Type,
                            isl->Parameters.Power.State.SystemState,
                            isl->Parameters.Power.State.DeviceState);
            break;

        case IRP_MN_QUERY_POWER:
            I2cCtrl_Emu_Log("PDO[%lu] IRP_MN_QUERY_POWER\n", pext->Index);
            break;

        case IRP_MN_WAIT_WAKE:
            I2cCtrl_Emu_Log("PDO[%lu] IRP_MN_WAIT_WAKE (not supported)\n", pext->Index);
            status = STATUS_NOT_SUPPORTED;
            break;

        case IRP_MN_POWER_SEQUENCE:
            I2cCtrl_Emu_Log("PDO[%lu] IRP_MN_POWER_SEQUENCE (no-op)\n", pext->Index);
            break;

        default:
            I2cCtrl_Emu_Log("PDO[%lu] Unhandled POWER IRP minor=%u\n",
                            pext->Index, isl->MinorFunction);
            break;
        }
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}


/* ---------------------------------------------------------------------------
 * Dispatch: IOCTL (XP-safe, C89-compliant, WDK-safe, verbose-compliant)
 * Handles both FDO and PDO objects
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_DispatchIoctl(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    )
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(Irp);
    ULONG inLen  = isl->Parameters.DeviceIoControl.InputBufferLength;
    PUCHAR inBuf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    ULONG ioctlCode = isl->Parameters.DeviceIoControl.IoControlCode;

    /* Distinguish FDO vs PDO */
    PI2CCTRL_EMU_FDO_EXT fext = (PI2CCTRL_EMU_FDO_EXT)DeviceObject->DeviceExtension;
    BOOLEAN isFdo = (fext != NULL && fext->Self == DeviceObject);

    if (isFdo) {
        /* ---------------- FDO path ---------------- */
        I2cCtrl_Emu_Log("FDO IOCTL dispatch: code=0x%08lX inLen=%lu\n", ioctlCode, inLen);

        /* Forward to buffered IOCTL handler */
        status = I2CCTRL_EMU_IoctlDispatchBuffered(
                     fext,
                     ioctlCode,
                     inBuf,
                     inLen
                 );

        I2cCtrl_Emu_Log("FDO IOCTL completed: code=0x%08lX status=0x%08lX\n",
                        ioctlCode, status);
    } else {
        /* ---------------- PDO path ---------------- */
        PI2CCTRL_EMU_PDO_EXT pext = (PI2CCTRL_EMU_PDO_EXT)DeviceObject->DeviceExtension;

        I2cCtrl_Emu_Log("PDO[%lu] IOCTL dispatch: code=0x%08lX inLen=%lu\n",
                        pext->Index, ioctlCode, inLen);

        /* For simplicity, PDOs do not handle IOCTLs in this emulator */
        status = STATUS_INVALID_DEVICE_REQUEST;

        I2cCtrl_Emu_Log("PDO[%lu] IOCTL completed: code=0x%08lX status=0x%08lX\n",
                        pext->Index, ioctlCode, status);
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0UL;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

VOID
I2cCtrl_Emu_Log(
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

    /* IRQL safety */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    if (Format == NULL) {
        return;
    }

    /* SAFE pointer formatting: convert %p to 0x%I64X */
    {
        CHAR safeFmt[256];
        SIZE_T i = 0, j = 0;

        while (Format[i] != '\0' && j < sizeof(safeFmt) - 1) {
            if (Format[i] == '%' && Format[i+1] == 'p') {
                safeFmt[j++] = '0';
                safeFmt[j++] = 'x';
                safeFmt[j++] = '%';
                safeFmt[j++] = 'I';
                safeFmt[j++] = '6';
                safeFmt[j++] = '4';
                safeFmt[j++] = 'X';
                i += 2;
                continue;
            }
            safeFmt[j++] = Format[i++];
        }
        safeFmt[j] = '\0';

        va_start(args, Format);
        status = RtlStringCbVPrintfA(buffer, sizeof(buffer), safeFmt, args);
        va_end(args);

        if (!NT_SUCCESS(status)) {
            return;
        }
    }

    /* Timestamp */
    KeQuerySystemTime(&sysTime);
    ExSystemTimeToLocalTime(&sysTime, &localTime);
    RtlTimeToTimeFields(&localTime, &tf);

    {
        CHAR ts[64];
        ULONG hour = tf.Hour;
        BOOLEAN pm = FALSE;

        if (hour == 0) {
            hour = 12;
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

        RtlStringCbPrintfA(
            final,
            sizeof(final),
            "%s%s",
            ts,
            buffer
        );
    }

    /* Open emulator log file */
    RtlInitUnicodeString(&path, L"\\SystemRoot\\System32\\i2cctrl_emu.log");

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

    /* Mirror to debugger */
    KdPrint(("I2CCTRL_EMU: %s\n", buffer));
}
