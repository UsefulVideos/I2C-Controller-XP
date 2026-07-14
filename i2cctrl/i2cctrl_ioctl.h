/* i2cctrl_ioctl.h */
#ifndef _I2CCTRL_IOCTL_H_
#define _I2CCTRL_IOCTL_H_

#include <ntddk.h>

/*
 * IOCTL codes (FILE_DEVICE_UNKNOWN, METHOD_BUFFERED)
 * Function number ranges reserved by category to avoid collisions:
 *  - 0x800-0x80F : SMBus operations
 *  - 0x810-0x81F : Raw I2C operations
 *  - 0x820-0x82F : Legacy/compat and debug
 *  - 0x830-0x83F : HID/PT sample
 *  - 0x900-0x90F : SpbCx/ACPIEx façade operations
 */

/* ---------------------------------------------------------------------------
   SMBus IOCTLs
   --------------------------------------------------------------------------- */
#define IOCTL_SMBUS_QUICK        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SMBUS_SEND_BYTE    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SMBUS_RECEIVE_BYTE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SMBUS_READ_BYTE    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SMBUS_WRITE_BYTE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SMBUS_READ_WORD    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SMBUS_WRITE_WORD   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SMBUS_BLOCK_READ   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SMBUS_BLOCK_WRITE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ---------------------------------------------------------------------------
   Raw I2C IOCTLs
   --------------------------------------------------------------------------- */
#define IOCTL_I2C_WRITE          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2C_READ           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TRANSFER   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ---------------------------------------------------------------------------
   Legacy/compat IOCTLs
   --------------------------------------------------------------------------- */
#define IOCTL_SMBUS_BLOCK_READ_LEGACY   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SMBUS_BLOCK_WRITE_LEGACY  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_XFER_DESC                 CTL_CODE(FILE_DEVICE_UNKNOWN, 0x822, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ---------------------------------------------------------------------------
   HID/PT sample IOCTL
   --------------------------------------------------------------------------- */
#ifndef FILE_DEVICE_I2CCTRL
#define FILE_DEVICE_I2CCTRL   0x8331  /* unique device type for I2C controller */
#endif

#ifndef IOCTL_GET_PT_SAMPLE
#define IOCTL_GET_PT_SAMPLE \
    CTL_CODE(FILE_DEVICE_I2CCTRL, 0x830, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* ---------------------------------------------------------------------------
   SpbCx/ACPIEx façade IOCTLs
   --------------------------------------------------------------------------- */
#define IOCTL_SET_TARGET  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SEQUENCE    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PROBE       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ---------------------------------------------------------------------------
   Debug / crash IOCTL
   --------------------------------------------------------------------------- */
#define IOCTL_I2C_FORCE_CRASH     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x814, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ---------------------------------------------------------------------------
   Message structure for multi-phase transfers
   --------------------------------------------------------------------------- */
#define I2C_MSG_READ   0x01  /* if set, this message is a read */
#define I2C_MSG_STOP   0x02  /* if set, generate STOP after this message */

typedef struct _I2CCTRL_MSG {
    UCHAR  Address;   /* 7-bit I2C address (bit7 must be 0) */
    UCHAR  Flags;     /* I2C_MSG_* flags */
    USHORT Length;    /* number of bytes */
    UCHAR  Data[1];   /* variable-length payload for write, or space for read */
} I2CCTRL_MSG, *PI2CCTRL_MSG;

/* ---------------------------------------------------------------------------
   Canonical multi-message transfer descriptor for IOCTL_TRANSFER
   --------------------------------------------------------------------------- */
#ifndef _I2CCTRL_TRANSFER_DEFINED
#define _I2CCTRL_TRANSFER_DEFINED
typedef struct _I2CCTRL_TRANSFER {
    ULONG       NumMessages;   /* number of messages in this transfer */
	USHORT SlaveAddress;     /* 7-bit address in low bits (or your format) */
    ULONG  Direction;        /* I2C_DIRECTION_READ or I2C_DIRECTION_WRITE */
    PUCHAR Buffer;           /* user-provided buffer to read/write */
    ULONG  Length;           /* number of bytes to transfer */
    ULONG  BytesReturned;    /* filled on completion for reads */
    I2CCTRL_MSG Messages[ANYSIZE_ARRAY]; /* variable-length array of messages */
} I2CCTRL_TRANSFER, *PI2CCTRL_TRANSFER;
#endif /* _I2CCTRL_TRANSFER_DEFINED */

/* ---------------------------------------------------------------------------
   Legacy single-transfer descriptor (used by HID façade)
   --------------------------------------------------------------------------- */
#ifndef _I2CCTRL_XFER_DESC_DEFINED
#define _I2CCTRL_XFER_DESC_DEFINED
typedef struct _I2CCTRL_XFER_DESC {
    ULONG   Length;      /* payload length for the operation */
    ULONG   TimeoutMs;   /* timeout for this transfer */
    ULONG   Flags;       /* transfer flags (e.g., 10-bit, PEC, no-stop) */
    BOOLEAN IsRead;      /* TRUE for read, FALSE for write */
    UCHAR   Reserved1[3];
    UCHAR   Address7Bit;
} I2CCTRL_XFER_DESC, *PI2CCTRL_XFER_DESC;
#endif /* _I2CCTRL_XFER_DESC_DEFINED */

/* ---------------------------------------------------------------------------
   Common request descriptor header for SMBus and raw I2C ioctls
   --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_IO_DESC {
    UCHAR  Address7Bit;  /* 7-bit slave address (bit7 must be 0) */
    UCHAR  Command;      /* SMBus command/register when applicable */
    UCHAR  PecMode;      /* 0 = off, 1 = on (SMBus PEC) */
    UCHAR  Reserved;     /* alignment */
    USHORT Length;       /* payload length for data operations */
} I2CCTRL_IO_DESC, *PI2CCTRL_IO_DESC;

/* ---------------------------------------------------------------------------
   Internal opcodes used by the transfer engine
   --------------------------------------------------------------------------- */
#define I2CCTRL_OPCODE_SEND_BYTE     0x01
#define I2CCTRL_OPCODE_RECEIVE_BYTE  0x02
#define I2CCTRL_OPCODE_WRITE_BYTE    0x03
#define I2CCTRL_OPCODE_READ_BYTE     0x04
#define I2CCTRL_OPCODE_WRITE_WORD    0x05
#define I2CCTRL_OPCODE_READ_WORD     0x06
#define I2CCTRL_OPCODE_BLOCK_WRITE   0x07
#define I2CCTRL_OPCODE_BLOCK_READ    0x08
#define I2CCTRL_OPCODE_I2C_WRITE     0x09
#define I2CCTRL_OPCODE_I2C_READ      0x0A

#endif /* _I2CCTRL_IOCTL_H_ */
