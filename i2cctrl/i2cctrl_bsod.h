/* i2cctrl_bsod.h
 * Advanced BSOD workaround helpers with WinDBG-friendly diagnostics
 * Target: Windows XP x86/x64, C89 compliant
 */

#ifndef _I2CCTRL_BSOD_H_
#define _I2CCTRL_BSOD_H_

#include <ntddk.h>
#include "i2cctrl_etw.h"   /* TraceEvents() and WPP flags */

/* Forward declaration to avoid circular include */
struct _I2CCTRL_FDO;
typedef struct _I2CCTRL_FDO I2CCTRL_FDO, *PI2CCTRL_FDO;

/* ---------------------------------------------------------------------------
   Assertions (WinDBG friendly, XP-safe)
   --------------------------------------------------------------------------- */
#if DBG
#define I2CCTRL_ASSERT(expr) \
    do { \
        if (!(expr)) { \
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_FLAG_INIT, \
                        "ASSERT FAILED: %s at %s:%d", #expr, __FILE__, __LINE__); \
            /* XP-safe: break only at PASSIVE_LEVEL to avoid IRQL bugchecks */ \
            if (KeGetCurrentIrql() == PASSIVE_LEVEL) { \
                DbgBreakPoint(); \
            } \
        } \
    } while (0)
#else
#define I2CCTRL_ASSERT(expr) ((void)0)
#endif

/* ---------------------------------------------------------------------------
   Guard macros for BSOD prevention (XP-safe IRQL checks)
   --------------------------------------------------------------------------- */

#define I2CCTRL_REQUIRE_DISPATCH_OR_LOWER() \
    I2CCTRL_ASSERT(KeGetCurrentIrql() <= DISPATCH_LEVEL)

#define I2CCTRL_REQUIRE_PTR(p) \
    I2CCTRL_ASSERT((p) != NULL)

#define I2CCTRL_REQUIRE_ACTIVE(dx) \
    I2CCTRL_ASSERT((dx) != NULL && !(dx)->Removed)

/* ---------------------------------------------------------------------------
   Pool tags (XP-safe: four-char constants)
   --------------------------------------------------------------------------- */
#define TAG_I2C_MMIO  'OMMI'
#define TAG_I2C_STR   'RTSI'
#define TAG_I2C_MISC  'CSMI'
#define TAG_I2C_RST   'TSRI'

/* ---------------------------------------------------------------------------
   Function prototypes (implemented in i2cctrl_bsod.c)
   All functions are XP-safe: they validate pointers, IRQL, and use SEH where needed.
   --------------------------------------------------------------------------- */
VOID   I2cCtrl_SafeCompleteIrp(PIRP Irp, NTSTATUS Status);
ULONG  I2cCtrl_ReadRegisterSafe(PI2CCTRL_FDO Dx, ULONG Offset);
VOID   I2cCtrl_WriteRegisterSafe(PI2CCTRL_FDO Dx, ULONG Offset, ULONG Value);

VOID   I2cCtrl_AcquireIoLock(PI2CCTRL_FDO Dx, PKIRQL OldIrql);
VOID   I2cCtrl_ReleaseIoLock(PI2CCTRL_FDO Dx, KIRQL OldIrql);

VOID   I2cCtrl_SafeFreeUnicode(PUNICODE_STRING Str);
PUNICODE_STRING I2cCtrl_SafeDuplicateUnicode(PCUNICODE_STRING Src, ULONG Tag);

VOID   I2cCtrl_SafeDeleteChild(PDEVICE_OBJECT Pdo);

PVOID  I2cCtrl_Alloc(POOL_TYPE Type, SIZE_T Size, ULONG Tag);
VOID   I2cCtrl_Free(PVOID Ptr, ULONG Tag);

NTSTATUS I2cCtrl_SafeCopyFromUser(PVOID KernelDest, PVOID UserSrc, SIZE_T Bytes);
NTSTATUS I2cCtrl_SafeCopyToUser(VOID* UserDest, const VOID* KernelSrc, SIZE_T Bytes);

NTSTATUS I2cCtrl_SafeProbeAndLockMdl(PMDL* OutMdl, PVOID UserBuffer, SIZE_T Length, BOOLEAN ForWrite);
VOID     I2cCtrl_SafeUnlockMdl(PMDL Mdl);

VOID     I2cCtrl_SafeDeref(PVOID Object, PVOID (*DecRef)(PVOID));

BOOLEAN  I2cCtrl_SafeRefDevice(PDEVICE_OBJECT DeviceObject);
VOID     I2cCtrl_SafeDerefDevice(PDEVICE_OBJECT DeviceObject);

NTSTATUS I2cCtrl_SafeControllerReset(PI2CCTRL_FDO Dx, ULONG Tag);


#endif /* _I2CCTRL_BSOD_H_ */
