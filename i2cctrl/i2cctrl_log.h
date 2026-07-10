#include <stdarg.h>
#include <ntstrsafe.h>
#include <strsafe.h>
#include "i2cctrl_etw.h"
#include "i2cctrl_etw.tmh"

/* -----------------------------------------------------------------------
 * kernel logger with printf-style formatting + timestamp prefix
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_Log(
    PCSTR Format,
    ...
    );