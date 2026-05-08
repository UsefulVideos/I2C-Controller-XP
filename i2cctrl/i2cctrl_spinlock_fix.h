// i2cctrl_spinlock_fix.h
//
// Shim header to suppress the inline KeInitializeSpinLock
// definition from wdm.h on XP x86 builds. This ensures
// the i2cctrl driver links cleanly against ntoskrnl.lib.
// Adds defensive zeroing before initialization.
//
// C89-compliant, XP/2003 BSOD-safe.

#pragma once

#include <wdm.h>

// Remove the inline body injected by wdm.h
#ifdef KeInitializeSpinLock
#undef KeInitializeSpinLock
#endif

// Project-specific wrapper macro: zero then initialize
#define I2CCTRL_INIT_LOCK(plock)           \
    do {                                   \
        RtlZeroMemory((plock), sizeof(KSPIN_LOCK)); \
        KeInitializeSpinLock((plock));     \
    } while (0)
