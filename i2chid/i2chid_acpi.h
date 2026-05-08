/* -----------------------------------------------------------------------
   i2chid_acpi.h - ACPI parsing helpers for HID-over-I2C (PNP0C50) on XP/2003
   ----------------------------------------------------------------------- */

#ifndef _I2CHID_ACPI_H_
#define _I2CHID_ACPI_H_

#include "i2chid_spinlock_fix.h"
#include "..\i2cctrl\i2cctrl_DPI.h"
#include "i2chid_DPI.h"

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
 * The actual GUID is defined once in i2chid_guids.c with DEFINE_GUID.
 */
EXTERN_C const GUID I2C_HID_DSM_GUID;

/*
 * HID-over-I2C _DSM function indices (per HID-I²C spec).
 */
#define I2C_HID_DSM_FN_REVISION            0x00 /* returns revision (optional) */
#define I2C_HID_DSM_FN_HID_DESCRIPTOR_ADDR 0x01 /* returns HID descriptor register address */

/*
 * Minimal ACPI evaluation buffer structures for XP/2003.
 * These mirror the definitions used by IOCTL_ACPI_EVAL_METHOD.
 */
typedef struct _ACPI_EVAL_INPUT_BUFFER_COMPLEX {
    ULONG  Signature;
    CHAR   MethodName[4];
    ULONG  Size;
    ULONG  ArgumentCount;
    ULONG  Data[ANYSIZE_ARRAY]; /* GUID + integers packed */
} ACPI_EVAL_INPUT_BUFFER_COMPLEX, *PACPI_EVAL_INPUT_BUFFER_COMPLEX;

typedef struct _ACPI_EVAL_OUTPUT_BUFFER {
    ULONG  Signature;
    ULONG  Length;
    ULONG  Count;   /* number of ACPI_METHOD_ARGUMENTs following */
    UCHAR  Data[ANYSIZE_ARRAY];
} ACPI_EVAL_OUTPUT_BUFFER, *PACPI_EVAL_OUTPUT_BUFFER;

typedef struct _ACPI_METHOD_ARGUMENT {
    USHORT Type;
    USHORT DataLength;
    ULONG  Argument; /* inline or offset into buffer */
} ACPI_METHOD_ARGUMENT, *PACPI_METHOD_ARGUMENT;

/* Common signatures/types (consistent across WDKs) */
#define ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE 'IBCD'
#define ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE        'OBCD'
#define ACPI_METHOD_ARGUMENT_INTEGER             0x0002
#define ACPI_METHOD_ARGUMENT_BUFFER              0x0003

/*
 * Public entry: parse ACPI for PNP0C50 and populate DPI context.
 */
NTSTATUS
I2cHid_AcpiParsePnp0C50(
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    IN PCM_RESOURCE_LIST RawResources,
    IN PCM_RESOURCE_LIST TranslatedResources,
    OUT PI2CCTRL_DPI Dpi
    );

/* Define a simple I2C request packet for XP */
typedef struct _I2C_XP_REQUEST {
    UCHAR Address;       /* 7-bit I2C address */
    ULONG Offset;        /* register/descriptor offset */
    PUCHAR Buffer;       /* data buffer */
    ULONG Length;        /* number of bytes to transfer */
} I2C_XP_REQUEST, *PI2C_XP_REQUEST;

NTSTATUS
I2cHid_QueryHidDescriptorLength(
    IN PDEVICE_OBJECT ControllerDevice,
    IN UCHAR I2cAddr,
    IN ULONG HidDescAddr,
    OUT PULONG HidDescLen
    );

#endif /* _I2CHID_ACPI_H_ */
