#pragma once

#ifndef WINVER
    #if defined(_WIN64)
        // Targets Server 2003 / XP x64 (5.2)
        #define WINVER 0x0502
        #define _WIN32_WINNT 0x0502
    #else
        // Targets Windows XP (5.1) and covers Server 2003 x86
        #define WINVER 0x0501
        #define _WIN32_WINNT 0x0501
        #define _WIN32_WINDOWS 0x0501
    #endif
#endif
