#pragma once
//
// i2cctrl_comp_sal.h
// Compatibility header to neutralize SAL annotations
// for older WDK/XP compilers that don't recognize them.
// XP/2003 BSOD-safe: purely compile-time no-ops, never evaluate arguments,
// never emit runtime code, and avoid redefinitions.
//

#ifndef I2CCTRL_COMP_SAL_H
#define I2CCTRL_COMP_SAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
   Core presence switch for SAL
   --------------------------------------------------------------------------- */
#ifndef _SAL_VERSION
#define _SAL_VERSION 0
#endif

/* ---------------------------------------------------------------------------
   Basic annotations (no side effects, no evaluation)
   --------------------------------------------------------------------------- */
#ifndef _In_
#define _In_
#endif

#ifndef _Out_
#define _Out_
#endif

#ifndef _Inout_
#define _Inout_
#endif

/* ---------------------------------------------------------------------------
   Optional pointer annotations
   --------------------------------------------------------------------------- */
#ifndef _In_opt_
#define _In_opt_
#endif

#ifndef _Out_opt_
#define _Out_opt_
#endif

#ifndef _Inout_opt_
#define _Inout_opt_
#endif

/* ---------------------------------------------------------------------------
   Array/size annotations
   --------------------------------------------------------------------------- */
#ifndef _In_reads_
#define _In_reads_(n)
#endif

#ifndef _In_reads_bytes_
#define _In_reads_bytes_(n)
#endif

#ifndef _In_reads_z_
#define _In_reads_z_(n)
#endif

#ifndef _Out_writes_
#define _Out_writes_(n)
#endif

#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(n)
#endif

#ifndef _Out_writes_z_
#define _Out_writes_z_(n)
#endif

#ifndef _Out_writes_to_
#define _Out_writes_to_(start, end)
#endif

#ifndef _Out_writes_all_
#define _Out_writes_all_(n)
#endif

/* ---------------------------------------------------------------------------
   Pointer result annotations
   --------------------------------------------------------------------------- */
#ifndef _Outptr_
#define _Outptr_
#endif

#ifndef _Outptr_opt_
#define _Outptr_opt_
#endif

#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif

#ifndef _Outptr_opt_result_maybenull_
#define _Outptr_opt_result_maybenull_
#endif

#ifndef _Outptr_result_nullonfailure_
#define _Outptr_result_nullonfailure_
#endif

/* ---------------------------------------------------------------------------
   Dispatch type
   --------------------------------------------------------------------------- */
#ifndef _Dispatch_type_
#define _Dispatch_type_(x)
#endif

/* ---------------------------------------------------------------------------
   Return value annotations
   --------------------------------------------------------------------------- */
#ifndef _Check_return_
#define _Check_return_
#endif

#ifndef _Must_inspect_result_
#define _Must_inspect_result_
#endif

#ifndef _Success_
#define _Success_(expr)
#endif

/* ---------------------------------------------------------------------------
   Conditional annotations
   --------------------------------------------------------------------------- */
#ifndef _When_
#define _When_(cond, annos)
#endif

#ifndef _Pre_
#define _Pre_(annos)
#endif

#ifndef _Post_
#define _Post_(annos)
#endif

/* ---------------------------------------------------------------------------
   Field/struct annotations
   --------------------------------------------------------------------------- */
#ifndef _Field_size_
#define _Field_size_(n)
#endif

#ifndef _Field_size_opt_
#define _Field_size_opt_(n)
#endif

#ifndef _Field_range_
#define _Field_range_(min, max)
#endif

#ifndef _Field_z_
#define _Field_z_
#endif

#ifndef _In_range_
#define _In_range_(min, max)
#endif

/* ---------------------------------------------------------------------------
   Function-level usage
   --------------------------------------------------------------------------- */
#ifndef _Use_decl_annotations_
#define _Use_decl_annotations_
#endif

/* ---------------------------------------------------------------------------
   Analysis helpers (make them true no-ops)
   --------------------------------------------------------------------------- */
#ifndef _Analysis_assume_
#define _Analysis_assume_(expr) ((void)0)
#endif

#ifndef _Analysis_noreturn_
#define _Analysis_noreturn_
#endif

#ifndef _Analysis_mode_
#define _Analysis_mode_(x)
#endif

/* ---------------------------------------------------------------------------
   Concurrency/locking annotations
   --------------------------------------------------------------------------- */
#ifndef _Requires_lock_held_
#define _Requires_lock_held_(lock)
#endif

#ifndef _Requires_no_locks_held_
#define _Requires_no_locks_held_
#endif

#ifndef _Lock_held_
#define _Lock_held_(lock)
#endif

#ifndef _Guarded_by_
#define _Guarded_by_(lock)
#endif

/* ---------------------------------------------------------------------------
   Nullability (no-op for XP)
   --------------------------------------------------------------------------- */
#ifndef _Null_terminated_
#define _Null_terminated_
#endif

#ifndef _Notnull_
#define _Notnull_
#endif

#ifndef _Maybe_raises_
#define _Maybe_raises_(x)
#endif

/* ---------------------------------------------------------------------------
   Format/security-related
   --------------------------------------------------------------------------- */
#ifndef _Printf_format_string_
#define _Printf_format_string_
#endif

#ifndef _Scanf_format_string_
#define _Scanf_format_string_
#endif

/* ---------------------------------------------------------------------------
   Deprecated/annotation toggles
   --------------------------------------------------------------------------- */
#ifndef _Deprecated_
#define _Deprecated_(msg)
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* I2CCTRL_COMP_SAL_H */
