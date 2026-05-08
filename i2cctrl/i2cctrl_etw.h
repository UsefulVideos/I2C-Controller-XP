/* i2cctrl_etw.h
 *
 * WPP software tracing configuration for i2cctrl.sys
 * Provides TraceEvents() macro for structured ETW logging.
 * Genericized: focuses on abstract bus context instead of raw MMIO registers.
 */

#pragma once

#include <ntddk.h>
#include <evntrace.h>   // TRACE_LEVEL_*
#include <wmistr.h>
#include "i2cctrl_spinlock_fix.h"
#include "i2cctrl_sal.h"

/* ---------------------------------------------------------------------------
   Control GUID and trace flags
   Replace the GUID with one generated specifically for your project.
   --------------------------------------------------------------------------- */
#define WPP_CONTROL_GUIDS                                              \
    WPP_DEFINE_CONTROL_GUID(                                           \
        I2cCtrlTraceGuid,                                              \
        (6e7c2d3a,9e7c,4c8b,84,8e,1b,9e,5c,6a,1a,11),                  \
        WPP_DEFINE_BIT(TRACE_FLAG_INIT)                                \
        WPP_DEFINE_BIT(TRACE_FLAG_PNP)                                 \
        WPP_DEFINE_BIT(TRACE_FLAG_POWER)                               \
        WPP_DEFINE_BIT(TRACE_FLAG_IOCTL)                               \
        WPP_DEFINE_BIT(TRACE_FLAG_BUS)                                 \
        WPP_DEFINE_BIT(TRACE_FLAG_TRANSFER)                            \
        WPP_DEFINE_BIT(TRACE_FLAG_ISR)                                 \
        WPP_DEFINE_BIT(TRACE_FLAG_DPC)                                 \
        WPP_DEFINE_BIT(TRACE_FLAG_ACPI)                                \
        WPP_DEFINE_BIT(TRACE_FLAG_SNAPSHOT)                            \
    )

/* ---------------------------------------------------------------------------
   WPP macros to enable flag/level filtering
   --------------------------------------------------------------------------- */
#define WPP_FLAG_LEVEL_LOGGER(flag, level) WPP_LEVEL_LOGGER(level)
#define WPP_FLAG_LEVEL_ENABLED(flag, level) \
    (WPP_LEVEL_ENABLED(level) && WPP_CONTROL(WPPBit_##flag).Level >= level)

/* ---------------------------------------------------------------------------
   WPP configuration block
   This tells WPP to generate the TraceEvents() macro in i2cctrl_etw.tmh.
   --------------------------------------------------------------------------- */
// begin_wpp config
// FUNC TraceEvents(LEVEL, FLAGS, MSG, ...);
// USEPREFIX(TraceEvents, "%!FUNC!: ");
// end_wpp

/* ---------------------------------------------------------------------------
   Manual fallback definitions (used if .tmh is missing)
   These allow compilation even without WPP-generated files.
   --------------------------------------------------------------------------- */
#ifndef TraceEvents
#define TraceEvents(Level, Flags, Msg, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, Level, "I2CCTRL[%lx]: " Msg "\n", (Flags) , ##__VA_ARGS__)
#endif

#ifndef TRACE_FLAG_INIT
#define TRACE_FLAG_INIT       0x00000001
#define TRACE_FLAG_PNP        0x00000002
#define TRACE_FLAG_POWER      0x00000004
#define TRACE_FLAG_IOCTL      0x00000008
#define TRACE_FLAG_BUS        0x00000010
#define TRACE_FLAG_TRANSFER   0x00000020
#define TRACE_FLAG_ISR        0x00000040
#define TRACE_FLAG_DPC        0x00000080
#define TRACE_FLAG_ACPI       0x00000100
#define TRACE_FLAG_SNAPSHOT   0x00000200
#endif

/* ---------------------------------------------------------------------------
   Compatibility shims for legacy I2CCTRL_LOG_* macros
   These map old macros to TraceEvents so existing code compiles.
   --------------------------------------------------------------------------- */
#define I2CCTRL_LOG_ENTER(fn) \
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_INIT, ">> %s", fn)

#define I2CCTRL_LOG_EXIT(fn) \
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_INIT, "<< %s", fn)

#define I2CCTRL_LOG_INFO(fmt, ...) \
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_FLAG_BUS, fmt, __VA_ARGS__)

#define I2CCTRL_LOG_WARN(fmt, ...) \
    TraceEvents(TRACE_LEVEL_WARNING, TRACE_FLAG_BUS, fmt, __VA_ARGS__)

#define I2CCTRL_LOG_ERROR(fmt, ...) \
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_BUS, fmt, __VA_ARGS__)

#define I2CCTRL_LOG_TRACE(fmt, ...) \
    TraceEvents(TRACE_LEVEL_VERBOSE, TRACE_FLAG_BUS, fmt, __VA_ARGS__)

/* ---------------------------------------------------------------------------
   Public prototypes for ETW lifecycle helpers
   --------------------------------------------------------------------------- */
VOID I2cCtrlEtwInitialize(VOID);
VOID I2cCtrlEtwShutdown(VOID);
VOID I2cCtrlEtwBanner(_In_z_ const char* DriverVersion, _In_z_ const char* BuildInfo);
