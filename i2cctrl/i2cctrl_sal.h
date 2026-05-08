/* i2cctrl_sal.h
 *
 * Minimal SAL annotation stubs for XP builds.
 * These macros expand to nothing so the compiler accepts annotated prototypes.
 * Include this header before using any SAL attributes in your driver code.
 */

#pragma once

/* ---------------------------------------------------------------------------
   Input/output parameter annotations
   --------------------------------------------------------------------------- */
#ifndef _In_
#define _In_
#endif

#ifndef _In_opt_
#define _In_opt_
#endif

#ifndef _Out_
#define _Out_
#endif

#ifndef _Out_opt_
#define _Out_opt_
#endif

#ifndef _Inout_
#define _Inout_
#endif

#ifndef _Inout_opt_
#define _Inout_opt_
#endif

/* ---------------------------------------------------------------------------
   String/array annotations
   --------------------------------------------------------------------------- */
#ifndef _In_z_
#define _In_z_
#endif

#ifndef _Out_z_
#define _Out_z_
#endif

#ifndef _In_reads_
#define _In_reads_(x)
#endif

#ifndef _Out_writes_
#define _Out_writes_(x)
#endif

/* ---------------------------------------------------------------------------
   Other common SAL macros
   --------------------------------------------------------------------------- */
#ifndef _Success_
#define _Success_(expr)
#endif

#ifndef _Check_return_
#define _Check_return_
#endif

#ifndef _Must_inspect_result_
#define _Must_inspect_result_
#endif

#ifndef _Reserved_
#define _Reserved_
#endif