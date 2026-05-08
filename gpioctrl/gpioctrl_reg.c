/* gpioctrl_reg.c
 * GPIO Controller Driver (gpioctrl.sys) - registry policy loader
 * WinDDK 7.1.0 - XP/2003 build environment - C89 compliant
 *
 * Implements:
 *  - GpioCtrl_LoadRegistryPolicy: reads DWORD parameters from registry
 *
 * Parameters (HKLM\System\CurrentControlSet\Services\gpioctrl\Parameters):
 *  - DebounceDefaultMs   (DWORD) : default debounce time in milliseconds
 *  - CrashOnError        (DWORD) : 0/1 - force bugcheck on severe errors
 *  - PinCount            (DWORD) : overrides detected pin count
 *  - SupportsPull        (DWORD) : 0/1 - controller supports pull configuration
 *  - SupportsInterrupts  (DWORD) : 0/1 - controller supports interrupts
 *  - SupportsDebounce    (DWORD) : 0/1 - driver applies basic debounce
 *
 * Notes:
 *  - Uses RtlQueryRegistryValues with DIRECT entries to write into fields.
 *  - Caller should initialize sane defaults in AddDevice before calling here.
 */

#include <ntddk.h>
#include "gpioctrl_ext.h"

/* Pool tag for allocations in this file: 'gReg' */
#define GPIOCTRL_REG_TAG 'gReg'

/* ---------------------------------------------------------------------------
   Helpers to form Parameters key path
   --------------------------------------------------------------------------- */
static NTSTATUS
GpioReg_BuildParametersPath(
    IN  PUNICODE_STRING RegistryPath, /* service key (from DriverEntry), optional */
    OUT PUNICODE_STRING ParametersPath,
    IN  PVOID           PoolTag       /* ignored; NonPagedPool used */
    )
{
    NTSTATUS status;
    UNICODE_STRING suffix;
    USHORT newLen;
    UNREFERENCED_PARAMETER(PoolTag);

    RtlInitUnicodeString(&suffix, L"\\Parameters");

    if (RegistryPath != NULL && RegistryPath->Length != 0) {
        newLen = (USHORT)(RegistryPath->Length + suffix.Length);

        ParametersPath->Buffer = (PWCH)ExAllocatePoolWithTag(NonPagedPool,
                                                             newLen + sizeof(WCHAR),
                                                             GPIOCTRL_REG_TAG);
        if (ParametersPath->Buffer == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ParametersPath->Length = 0;
        ParametersPath->MaximumLength = (USHORT)(newLen + sizeof(WCHAR));

        status = RtlAppendUnicodeStringToString(ParametersPath, RegistryPath);
        if (!NT_SUCCESS(status)) {
            ExFreePool(ParametersPath->Buffer);
            ParametersPath->Buffer = NULL;
            ParametersPath->Length = 0;
            ParametersPath->MaximumLength = 0;
            return status;
        }

        status = RtlAppendUnicodeStringToString(ParametersPath, &suffix);
        if (!NT_SUCCESS(status)) {
            ExFreePool(ParametersPath->Buffer);
            ParametersPath->Buffer = NULL;
            ParametersPath->Length = 0;
            ParametersPath->MaximumLength = 0;
            return status;
        }

        /* Null-terminate */
        ParametersPath->Buffer[ParametersPath->Length / sizeof(WCHAR)] = L'\0';
        return STATUS_SUCCESS;
    } else {
        UNICODE_STRING hardcoded;

        RtlInitUnicodeString(&hardcoded, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\gpioctrl\\Parameters");

        ParametersPath->Buffer = (PWCH)ExAllocatePoolWithTag(NonPagedPool,
                                                             hardcoded.Length + sizeof(WCHAR),
                                                             GPIOCTRL_REG_TAG);
        if (ParametersPath->Buffer == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        ParametersPath->Length = 0;
        ParametersPath->MaximumLength = (USHORT)(hardcoded.Length + sizeof(WCHAR));

        (VOID)RtlAppendUnicodeStringToString(ParametersPath, &hardcoded);
        ParametersPath->Buffer[ParametersPath->Length / sizeof(WCHAR)] = L'\0';
        return STATUS_SUCCESS;
    }
}

/* ---------------------------------------------------------------------------
   Registry loader
   --------------------------------------------------------------------------- */
VOID
GpioCtrl_LoadRegistryPolicy(
    IN PGPIOCTRL_FDO_EXT Ext,
    IN PUNICODE_STRING   RegistryPath /* service key from DriverEntry, optional */
    )
{
    NTSTATUS status;
    UNICODE_STRING paramsPath;
    RTL_QUERY_REGISTRY_TABLE query[7];
    ULONG pinCount;
    ULONG supportsPull;
    ULONG supportsInts;
    ULONG supportsDebounce;
    ULONG debounceMs;
    ULONG crashOnError;
    ULONG i;

    /* Local copies initialized with existing defaults from Ext */
    pinCount         = Ext->PinCount;
    supportsPull     = Ext->SupportsPull;
    supportsInts     = Ext->SupportsInterrupts;
    supportsDebounce = Ext->SupportsDebounce;
    debounceMs       = Ext->DebounceDefaultMs;
    crashOnError     = Ext->CrashOnError;

    paramsPath.Buffer = NULL;
    paramsPath.Length = 0;
    paramsPath.MaximumLength = 0;

    status = GpioReg_BuildParametersPath(RegistryPath, &paramsPath, (PVOID)0);
    if (!NT_SUCCESS(status)) {
        /* If we cannot build path, keep defaults and return */
        return;
    }

    /* Zero the query table */
    for (i = 0; i < 7; i++) {
        RtlZeroMemory(&query[i], sizeof(query[i]));
    }

    /* Each DIRECT entry writes into our local variables */
    query[0].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[0].Name          = L"PinCount";
    query[0].EntryContext  = &pinCount;
    query[0].DefaultType   = REG_DWORD;
    query[0].DefaultData   = &pinCount;
    query[0].DefaultLength = sizeof(ULONG);

    query[1].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[1].Name          = L"SupportsPull";
    query[1].EntryContext  = &supportsPull;
    query[1].DefaultType   = REG_DWORD;
    query[1].DefaultData   = &supportsPull;
    query[1].DefaultLength = sizeof(ULONG);

    query[2].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[2].Name          = L"SupportsInterrupts";
    query[2].EntryContext  = &supportsInts;
    query[2].DefaultType   = REG_DWORD;
    query[2].DefaultData   = &supportsInts;
    query[2].DefaultLength = sizeof(ULONG);

    query[3].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[3].Name          = L"SupportsDebounce";
    query[3].EntryContext  = &supportsDebounce;
    query[3].DefaultType   = REG_DWORD;
    query[3].DefaultData   = &supportsDebounce;
    query[3].DefaultLength = sizeof(ULONG);

    query[4].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[4].Name          = L"DebounceDefaultMs";
    query[4].EntryContext  = &debounceMs;
    query[4].DefaultType   = REG_DWORD;
    query[4].DefaultData   = &debounceMs;
    query[4].DefaultLength = sizeof(ULONG);

    query[5].Flags         = RTL_QUERY_REGISTRY_DIRECT;
    query[5].Name          = L"CrashOnError";
    query[5].EntryContext  = &crashOnError;
    query[5].DefaultType   = REG_DWORD;
    query[5].DefaultData   = &crashOnError;
    query[5].DefaultLength = sizeof(ULONG);

    /* Terminator */
    query[6].Flags = 0;
    query[6].Name  = NULL;

    status = RtlQueryRegistryValues(
        RTL_REGISTRY_ABSOLUTE,
        paramsPath.Buffer,
        query,
        NULL,
        NULL);

    /* Free path buffer */
    ExFreePool(paramsPath.Buffer);
    paramsPath.Buffer = NULL;

    if (!NT_SUCCESS(status)) {
        /* Keep existing defaults if query fails */
        return;
    }

    /* Apply sanitized values */
    if (pinCount == 0) {
        pinCount = Ext->PinCount; /* ignore zero override */
    }

    Ext->PinCount           = pinCount;
    Ext->SupportsPull       = (supportsPull != 0) ? 1 : 0;
    Ext->SupportsInterrupts = (supportsInts != 0) ? 1 : 0;
    Ext->SupportsDebounce   = (supportsDebounce != 0) ? 1 : 0;
    Ext->DebounceDefaultMs  = debounceMs;
    Ext->CrashOnError       = (crashOnError != 0) ? 1 : 0;
}
