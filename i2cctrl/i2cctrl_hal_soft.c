#include <ntddk.h>
#include "i2cctrl_hal_ops.h"
#include "i2cctrl_hal_caps.h"

I2C_HW_CAPS SoftI2cCaps = { 0 };

I2C_HW_OPS SoftI2cOps = {
    NULL, NULL,
    NULL, NULL,
    NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL,
    NULL, NULL,
    NULL, NULL, NULL,
    { 0 },
    NULL, NULL, NULL,
    NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL,
    NULL, NULL
};
