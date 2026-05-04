// i2chid_spinlock_fix.h
//
// Shim header to suppress the inline KeInitializeSpinLock
// definition from wdm.h on XP x86 builds. This ensures
// the driver links cleanly against ntoskrnl.lib.

#pragma once

#include <wdm.h>

// Remove the inline body injected by wdm.h
#ifdef KeInitializeSpinLock
#undef KeInitializeSpinLock
#endif

// Optionally provide a project‑specific wrapper macro
// for readability in your code.
#define I2CHID_INIT_LOCK(plock) KeInitializeSpinLock((plock))
