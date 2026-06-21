#ifndef I2CCTRL_HAL_ACPI_H
#define I2CCTRL_HAL_ACPI_H

#include <ntddk.h>
#include "i2cctrl_hal_ops.h"
#include "i2cctrl_hal_caps.h"
#include "i2cctrl_hal_dw.h"

/*
 * ACPI-backed I2C controller backend
 * Implemented in i2cctrl_hal_acpi.c
 */

extern I2C_HW_OPS  AcpiI2cOps;
extern I2C_HW_CAPS AcpiI2cCaps;

#endif /* I2CCTRL_HAL_ACPI_H */
