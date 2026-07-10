/* i2cctrl_etw.c
 *
 * XP/2003-friendly ETW/WPP-like tracing surface for i2cctrl.sys.
 * - Provides TraceEvents() front-end via i2cctrl_etw.tmh.
 * - Implements provider state, writer, and minimal init/shutdown stubs.
 * - Automatically appends abstract bus context (address/speed) to logs.
 */

#include <ntddk.h>
#include <stdio.h>
#include <stdarg.h>
#include <ntstrsafe.h>          /* RtlStringCbVPrintfA */
#include "i2cctrl_etw.h"        /* WPP flags / macros */
#include "i2cctrl_etw.tmh"      /* TraceEvents front-end declarations */
#include "i2cctrl_sal.h"        /* SAL stubs for XP builds */

/* Snapshot context (abstract bus state) */
#include "i2cctrl_dump.h"   /* declares PI2CCTRL_DUMP_SNAPSHOT gSnap */

/* ---------------------------------------------------------------------------
   Provider state (exported for other modules)
   --------------------------------------------------------------------------- */
volatile ULONG g_I2cCtrlTraceEnableMask = 0;        /* bitmask for TRACE_FLAG_* */
volatile UCHAR g_I2cCtrlMinLevelTable[32] = {0};    /* per-flag minimum level */
PVOID g_I2cCtrlEtwRegHandle = NULL;                 /* ETW provider handle (optional) */

/* ---------------------------------------------------------------------------
   Internal helpers
   --------------------------------------------------------------------------- */

/* Fetch current abstract bus context from snapshot (safe at any IRQL). */
__forceinline
VOID
I2cCtrl_GetBusContext(
    _Out_ ULONG* BusAddr,
    _Out_ ULONG* BusSpeedHz
    )
{
    ULONG addr = 0U;
    ULONG speed = 0U;

    if (BusAddr == NULL || BusSpeedHz == NULL) {
        return;
    }

    /* Use last captured snapshot values if available */
    if (gSnap != NULL) {
        addr  = gSnap->BusAddress;
        speed = gSnap->BusSpeedHz;
    }

    *BusAddr    = addr;
    *BusSpeedHz = speed;
}

/* ---------------------------------------------------------------------------
   Public API: initialization and shutdown
   --------------------------------------------------------------------------- */

/* Initialize tracing: set default masks and per-flag levels. */
VOID
I2cCtrlEtwInitialize(VOID)
{
    ULONG i;

    /* Enable INIT and IOCTL logs by default; keep others opt-in */
    InterlockedExchange((LONG*)&g_I2cCtrlTraceEnableMask,
                        (TRACE_FLAG_INIT | TRACE_FLAG_IOCTL));

    /* Default minimum level per flag: INFORMATION */
    for (i = 0; i < 32; ++i) {
        g_I2cCtrlMinLevelTable[i] = (UCHAR)TRACE_LEVEL_INFORMATION;
    }
}

/* Shutdown tracing: disable all flags and clear handle. */
VOID
I2cCtrlEtwShutdown(VOID)
{
    InterlockedExchange((LONG*)&g_I2cCtrlTraceEnableMask, 0);
    g_I2cCtrlEtwRegHandle = NULL;
}

/* Optional: register ETW provider (stubbed for XP/2003). */
NTSTATUS
I2cCtrlEtwRegisterProvider(VOID)
{
    /* On Vista+ you could call EtwRegister here; XP-safe stub. */
    g_I2cCtrlEtwRegHandle = NULL;
    return STATUS_SUCCESS;
}

/* Optional: unregister ETW provider (stubbed for XP/2003). */
VOID
I2cCtrlEtwUnregisterProvider(VOID)
{
    g_I2cCtrlEtwRegHandle = NULL;
}

/* Set the enable mask (TRACE_FLAG_* bitfield). */
VOID
I2cCtrlEtwSetEnableMask(
    _In_ ULONG EnableMask
    )
{
    InterlockedExchange((LONG*)&g_I2cCtrlTraceEnableMask, (LONG)EnableMask);
}

/* Set minimum level for a specific flag (0..31 index). */
VOID
I2cCtrlEtwSetMinLevelForFlag(
    _In_ ULONG FlagIndex,
    _In_ UCHAR MinLevel
    )
{
    if (FlagIndex < 32) {
        g_I2cCtrlMinLevelTable[FlagIndex] = MinLevel;
    }
}

/* ---------------------------------------------------------------------------
   Front-end writer: appends bus context and formats message safely
   --------------------------------------------------------------------------- */
VOID
I2cCtrlEtwWrite(
    _In_ UCHAR Level,
    _In_ ULONG Flags,
    _In_z_ const char* Msg,
    ...
    )
{
    char     formatted[512]; /* body: user message */
    va_list  args;
    NTSTATUS rc;

    ULONG busAddr  = 0U;
    ULONG busSpeed = 0U;

    I2cCtrl_GetBusContext(&busAddr, &busSpeed);

    /* Format the user message safely into 'formatted' */
    va_start(args, Msg);
    rc = RtlStringCbVPrintfA(formatted, sizeof(formatted), Msg, args);
    va_end(args);

    if (!NT_SUCCESS(rc)) {
        /* Fallback: truncated message if formatting failed */
        DbgPrintEx(DPFLTR_IHVDRIVER_ID,
                   Level,
                   "I2CCTRL[%lx]: <formatting-error> | busAddr=0x%X speed=%luHz\n",
                   Flags, busAddr, busSpeed);
        return;
    }

    /* Print combined line with bus context appended */
    DbgPrintEx(DPFLTR_IHVDRIVER_ID,
               Level,
               "I2CCTRL[%lx]: %s | busAddr=0x%X speed=%luHz\n",
               Flags, formatted, busAddr, busSpeed);
}

/* ---------------------------------------------------------------------------
   Convenience: emit a one-time banner with version and mask
   --------------------------------------------------------------------------- */
VOID
I2cCtrlEtwBanner(
    _In_z_ const char* DriverVersion,
    _In_z_ const char* BuildInfo
    )
{
    ULONG mask = g_I2cCtrlTraceEnableMask;
    DbgPrintEx(DPFLTR_IHVDRIVER_ID,
               TRACE_LEVEL_INFORMATION,
               "Tracing initialized (ver=%s, build=%s, mask=0x%08lx)\n",
               DriverVersion ? DriverVersion : "unknown",
               BuildInfo ? BuildInfo : "unknown",
               mask);
}
