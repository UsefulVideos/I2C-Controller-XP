#pragma once

//
// I2CHID_comp_sal.h
// Compatibility header to neutralize SAL annotations
// for older WDK/XP compilers that don't recognize them.
//

#ifndef _In_
#define _In_
#endif

#ifndef _Out_
#define _Out_
#endif

#ifndef _Inout_
#define _Inout_
#endif

#ifndef _In_reads_bytes_
#define _In_reads_bytes_(x)
#endif

#ifndef _Outptr_result_nullonfailure_
#define _Outptr_result_nullonfailure_
#endif

#ifndef _Dispatch_type_
#define _Dispatch_type_(x)
#endif

#ifndef _Check_return_
#define _Check_return_
#endif

#ifndef _Success_
#define _Success_(x)
#endif