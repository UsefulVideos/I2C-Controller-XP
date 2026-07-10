/* -----------------------------------------------------------------------
   I2CHID_acpi.h - ACPI parsing helpers for HID-over-I2C (PNP0C50) on XP/2003
   ----------------------------------------------------------------------- */

#ifndef _I2CHID_ACPI_H_
#define _I2CHID_ACPI_H_

#include <acpiioct.h>   /* <-- REQUIRED: XP ACPI structures */

#include "I2CHID_spinlock_fix.h"
#include "..\i2cctrl\i2cctrl_DPI.h"
#include "I2CHID_DPI.h"

/*
 * ACPI method evaluation IOCTL (XP/2003).
 * Used to evaluate ACPI control methods such as _DSM.
 */
#ifndef IOCTL_ACPI_EVAL_METHOD
#define IOCTL_ACPI_EVAL_METHOD \
    CTL_CODE(FILE_DEVICE_ACPI, 0x0002, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/*
 * HID-over-I2C _DSM UUID declaration.
 * The actual GUID is defined once in I2CHID_guids.c with DEFINE_GUID.
 */
EXTERN_C const GUID I2C_HID_DSM_GUID;

/*
 * HID-over-I2C _DSM function indices (per HID-I²C spec).
 */
#define I2C_HID_DSM_FN_REVISION            0x00
#define I2C_HID_DSM_FN_HID_DESCRIPTOR_ADDR 0x01

/*
 * Public entry: parse ACPI for PNP0C50 and populate DPI context.
 */
NTSTATUS
I2CHID_AcpiParsePnp0C50(
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    IN PCM_RESOURCE_LIST RawResources,
    IN PCM_RESOURCE_LIST TranslatedResources,
    OUT PI2CCTRL_DPI Dpi
    );

/* Define a simple I2C request packet for XP */
typedef struct _I2C_XP_REQUEST {
    UCHAR  Address;   /* 7-bit I2C address */
    ULONG  Offset;    /* register/descriptor offset */
    PUCHAR Buffer;    /* data buffer */
    ULONG  Length;    /* number of bytes to transfer */
} I2C_XP_REQUEST, *PI2C_XP_REQUEST;

NTSTATUS
I2CHID_QueryHidDescriptorLength(
    IN PDEVICE_OBJECT ControllerDevice,
    IN UCHAR          I2cAddr,
    IN ULONG          HidDescAddr,
    OUT PULONG        HidDescLen
    );

#endif /* _I2CHID_ACPI_H_ */
