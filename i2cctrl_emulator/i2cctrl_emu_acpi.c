/* i2cctrl_emu_acpi.c
 * XP/2003-safe ACPI helpers for I2CCTRL_EMU (no struct definitions in this file)
 *
 * Requirements:
 * - GUID_ACPI_INTERFACE_STANDARD available via wdmguid.h
 * - ACPI context fields are declared inside I2CCTRL_EMU_FDO_EXT (e.g., AcpiPdo, AcpiInterfaceReady)
 * - ACPI helper prototypes declared in i2cctrl_emu_ext.h
 */

#include <wdm.h>
#include <initguid.h>
#include "i2cctrl_emu_ext.h"

/* Forward declarations */
static NTSTATUS EmuSendQueryInterface(PDEVICE_OBJECT fdo, LPGUID guid, USHORT size, PVOID ifaceOut);
static NTSTATUS EmuEvalAcpiMethodViaIoctl(PDEVICE_OBJECT pdo, PCWSTR methodName);
static PDEVICE_OBJECT EmuGetAttachedDevice(PDEVICE_OBJECT top);

/* ---------------------------------------------------------------------------
 * Public: Initialize ACPI (query interface and run _INI if desired)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_AcpiInitialize(PI2CCTRL_EMU_FDO_EXT ext)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_OBJECT attached;

    if (ext == NULL || ext->Self == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    ext->AcpiInterfaceReady = FALSE;
    ext->AcpiPdo            = NULL;

    /* Find the ACPI PDO by walking down the stack from our FDO */
    attached = EmuGetAttachedDevice(ext->Self);
    if (attached == NULL) {
        I2cCtrl_Emu_Log("ACPI: no attached device stack\n");
        return STATUS_SUCCESS; /* non-fatal */
    }

    ext->AcpiPdo = attached;

    /* Try to obtain ACPI interface (GUID_ACPI_INTERFACE_STANDARD) */
    {
        GUID guid = GUID_ACPI_INTERFACE_STANDARD;
        UCHAR buffer[sizeof(INTERFACE)]; /* generic INTERFACE storage */
        RtlZeroMemory(buffer, sizeof(buffer));

        status = EmuSendQueryInterface(ext->Self, &guid, (USHORT)sizeof(INTERFACE), buffer);
        if (NT_SUCCESS(status)) {
            ext->AcpiInterfaceReady = TRUE;
            I2cCtrl_Emu_Log("ACPI: Interface acquired\n");
        } else {
            I2cCtrl_Emu_Log("ACPI: Interface unavailable 0x%08lX\n", status);
            status = STATUS_SUCCESS; /* non-fatal */
        }
    }

    /* Optionally evaluate _INI on our ACPI PDO to initialize firmware state */
    if (ext->AcpiPdo != NULL) {
        (VOID)EmuEvalAcpiMethodViaIoctl(ext->AcpiPdo, L"_INI");
    }

    return status;
}

/* ---------------------------------------------------------------------------
 * Public: Prime children based on ACPI (IDs, order, optional properties)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_AcpiPrimeChildren(PI2CCTRL_EMU_FDO_EXT ext, const PWSTR* ids, ULONG count)
{
    UNREFERENCED_PARAMETER(ids);
    UNREFERENCED_PARAMETER(count);

    if (ext == NULL) return STATUS_INVALID_PARAMETER;

    if (ext->AcpiPdo != NULL) {
        I2cCtrl_Emu_Log("ACPI: prime children (ACPI PDO present)\n");
        (VOID)EmuEvalAcpiMethodViaIoctl(ext->AcpiPdo, L"_DSM");
    } else {
        I2cCtrl_Emu_Log("ACPI: prime children (no ACPI PDO)\n");
    }

    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------
 * Public: Attach ACPI-derived properties to the child PDO
 * XP-safe: surface properties via IRP_MN_QUERY_ID handlers, logging here.
 * --------------------------------------------------------------------------- */
NTSTATUS
I2CCTRL_EMU_AcpiAttachChildProperties(PI2CCTRL_EMU_FDO_EXT ext,
                                      PDEVICE_OBJECT ChildPdo,
                                      PWSTR HardwareId,
                                      ULONG Index)
{
    UNREFERENCED_PARAMETER(ext);
    UNREFERENCED_PARAMETER(ChildPdo);
    UNREFERENCED_PARAMETER(HardwareId);
    UNREFERENCED_PARAMETER(Index);

    I2cCtrl_Emu_Log("ACPI: attach properties to child[%lu]\n", Index);
    return STATUS_SUCCESS;
}

/* ===========================================================================
 * Internal helpers
 * =========================================================================== */

/* Walk down the stack to find the attached device below our FDO */
static PDEVICE_OBJECT
EmuGetAttachedDevice(PDEVICE_OBJECT top)
{
    PDEVICE_OBJECT lower;
    if (top == NULL) return NULL;

    lower = IoGetAttachedDeviceReference(top);
    if (lower == NULL) return NULL;

    /* Caller may dereference later if stored long-term */
    return lower;
}

/* Send IRP_MN_QUERY_INTERFACE to obtain an ACPI interface (generic) */
static NTSTATUS
EmuSendQueryInterface(PDEVICE_OBJECT fdo, LPGUID guid, USHORT size, PVOID ifaceOut)
{
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    PIRP irp;
    KEVENT evt;
    IO_STATUS_BLOCK iosb;
    PIO_STACK_LOCATION sl;
    PDEVICE_OBJECT lower;

    if (fdo == NULL || guid == NULL || ifaceOut == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    lower = EmuGetAttachedDevice(fdo);
    if (lower == NULL) {
        return STATUS_UNSUCCESSFUL;
    }

    KeInitializeEvent(&evt, NotificationEvent, FALSE);

    irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP,
                                       lower,
                                       NULL,
                                       0,
                                       NULL,
                                       &evt,
                                       &iosb);
    if (irp == NULL) {
        ObfDereferenceObject(lower);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = 0;

    sl = IoGetNextIrpStackLocation(irp);
    sl->MajorFunction = IRP_MJ_PNP;
    sl->MinorFunction = IRP_MN_QUERY_INTERFACE;
    sl->Parameters.QueryInterface.InterfaceType = guid;
    sl->Parameters.QueryInterface.Size = size;
    sl->Parameters.QueryInterface.Version = 1;
    sl->Parameters.QueryInterface.Interface = (PINTERFACE)ifaceOut;
    sl->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    status = IoCallDriver(lower, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&evt, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    ObfDereferenceObject(lower);
    return status;
}

/* Evaluate a simple ACPI method via IOCTL to the ACPI PDO (XP-compatible) */
#ifndef IOCTL_ACPI_EVAL_METHOD
#define IOCTL_ACPI_EVAL_METHOD CTL_CODE(FILE_DEVICE_ACPI, 0x0001, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

static NTSTATUS
EmuEvalAcpiMethodViaIoctl(PDEVICE_OBJECT pdo, PCWSTR methodName)
{
    NTSTATUS status;
    KEVENT evt;
    IO_STATUS_BLOCK iosb;
    PIRP irp;

    UCHAR inb[8];   /* signature + 4-char method name */
    UCHAR outb[256];

    if (pdo == NULL || methodName == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Build minimal input buffer: 4-byte tag + 4 ASCII chars of method name */
    RtlZeroMemory(inb, sizeof(inb));
    inb[0] = 'I'; inb[1] = 'P'; inb[2] = 'C'; inb[3] = 'A';
    inb[4] = (UCHAR)((methodName[0] <= 0x7F) ? methodName[0] : '_');
    inb[5] = (UCHAR)((methodName[1] <= 0x7F) ? methodName[1] : '_');
    inb[6] = (UCHAR)((methodName[2] <= 0x7F) ? methodName[2] : '_');
    inb[7] = (UCHAR)((methodName[3] <= 0x7F) ? methodName[3] : '_');

    RtlZeroMemory(outb, sizeof(outb));
    KeInitializeEvent(&evt, NotificationEvent, FALSE);

    irp = IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD,
                                        pdo,
                                        inb, sizeof(inb),
                                        outb, sizeof(outb),
                                        FALSE,
                                        &evt,
                                        &iosb);
    if (irp == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IoCallDriver(pdo, irp);
    if (status == STATUS_PENDING) {
        KeWaitForSingleObject(&evt, Executive, KernelMode, FALSE, NULL);
        status = iosb.Status;
    }

    I2cCtrl_Emu_Log("ACPI: eval %ws -> 0x%08lX\n", methodName, status);
    return status;
}
