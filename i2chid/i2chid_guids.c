/* -----------------------------------------------------------------------
   i2chid_guids.c — single GUID instantiation unit
   ----------------------------------------------------------------------- */

#define INITGUID

#include "i2chid_spinlock_fix.h"
#include "i2chid_acpi.h"  /* declares I2C_HID_DSM_GUID */

/*
 * Define the HID-over-I²C _DSM UUID here so it is instantiated once.
 * Other modules should only reference it via extern in i2chid_acpi.h.
 */
DEFINE_GUID(I2C_HID_DSM_GUID,
    0x3cdff6f7, 0x4267, 0x4555,
    0xad, 0x05, 0xb3, 0x0a, 0x3d, 0x89, 0x38, 0xde);
