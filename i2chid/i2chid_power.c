/* I2CHID_power.c
 * Advanced power management for I2C HID driver: D0/D3 transitions and selective suspend.
 * C89 compliant.
 */

#include <ntddk.h>
#include <hidport.h>
#include "I2CHID_EXT.h"

/* ---- Hardware hooks (implemented elsewhere in your driver) ---- */
VOID I2cHw_EnableController(PI2CHID_FDO ext);
VOID I2cHw_DeviceResume(PI2CHID_FDO ext);
VOID I2cHw_ReprogramDescriptor(PI2CHID_FDO ext);
VOID I2cHw_ConnectInterrupt(PI2CHID_FDO ext);

VOID I2cHw_DisconnectInterrupt(PI2CHID_FDO ext);
VOID I2cHw_DeviceIdle(PI2CHID_FDO ext);
VOID I2cHw_DeviceSuspend(PI2CHID_FDO ext);
VOID I2cHw_EnableWake(PI2CHID_FDO ext, BOOLEAN Enable);

/* -----------------------------------------------------------------------
   Idle arming helper
   - Called from DPC loop after processing input
   - Arms idle if no contacts and inactivity exceeds threshold
   ----------------------------------------------------------------------- */
static VOID
I2CHID_ArmIdleIfNeeded(
    PI2CHID_FDO ext
    )
{
    ULONG nowMs;

    if (ext == NULL) {
        return;
    }

    /* Convert interrupt time (100ns units) to ~milliseconds */
    nowMs = (ULONG)(KeQueryInterruptTime() / 10000UL);

    /* If no contacts and time since last gesture exceeds PollIntervalMs*4, arm idle */
    if (ext->LastContactCount == 0U &&
        (ULONG)(nowMs - ext->Gest.lastTickMs) >= (ULONG)(ext->Cfg.PollIntervalMs * 4)) {
        InterlockedExchange(&ext->IdleArmed, 1L);
    }
}

/* -----------------------------------------------------------------------
   Idle DPC routine
   - Invoked when idle timer expires
   - If still armed, transitions device into D3 (selective suspend)
   ----------------------------------------------------------------------- */
VOID
I2CHID_IdleDpcRoutine(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID Arg1,
    PVOID Arg2
    )
{
    PI2CHID_FDO ext;
    LONG armed;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    ext = (PI2CHID_FDO)DeferredContext;
    if (ext == NULL) {
        return;
    }

    /* Atomically check and clear idle armed flag */
    armed = InterlockedCompareExchange(&ext->IdleArmed, 0L, 1L);
    if (armed == 1L) {
        /* Transition to D3 (selective suspend) */
        I2CHID_EnterDx(ext, PowerDeviceD3);
        ext->DeviceState = PowerDeviceD3;
        ext->SystemState = PowerSystemSleeping3;
    }
}

/* ---- Public power helpers ---- */

VOID
I2CHID_PowerInit(
    PI2CHID_FDO ext
    )
{
    ext->SystemState = PowerSystemWorking;
    ext->DeviceState = PowerDeviceD0;

    /* Defaults; override via registry (optional) elsewhere */
    ext->IdleTimeoutMs = 3000; /* 3s */
    ext->IdleArmed = 0;
    ext->WakeEnabled = FALSE;

    KeInitializeTimer(&ext->IdleTimer);
    KeInitializeDpc(&ext->IdleDpc, I2CHID_IdleDpcRoutine, ext);
}

VOID
I2CHID_NotifyActivity(
    PI2CHID_FDO ext
    )
{
    /* Any input activity cancels idle and rearms */
    I2CHID_DisarmIdle(ext);
    I2CHID_ArmIdle(ext);
}