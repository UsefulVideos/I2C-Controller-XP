#ifndef _I2CCTRL_IOCTL_H_
#define _I2CCTRL_IOCTL_H_

#include <windows.h>
#include <winioctl.h>

/*
 * IOCTL codes (FILE_DEVICE_UNKNOWN, METHOD_BUFFERED)
 * Unique function numbers are reserved by category to avoid collisions:
 *  - 0x800–0x80F : SMBus operations
 *  - 0x810–0x81F : Raw I2C operations
 *  - 0x820–0x82F : Legacy/compat and debug
 *  - 0x900–0x90F : SpbCx/ACPIEx façade operations
 */

#ifndef BOOLEAN
typedef unsigned char BOOLEAN;
#endif

/* ---------------------------------------------------------------------------
   SMBus IOCTLs
   --------------------------------------------------------------------------- */
#define IOCTL_I2CCTRL_SMBUS_QUICK          CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CCTRL_SMBUS_SEND_BYTE      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CCTRL_SMBUS_RECEIVE_BYTE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CCTRL_SMBUS_READ_BYTE      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CCTRL_SMBUS_WRITE_WORD     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CCTRL_SMBUS_READ_WORD      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CCTRL_SMBUS_BLOCK_WRITE    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CCTRL_SMBUS_BLOCK_READ     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Intentionally out-of-sequence (collision avoidance) */
#define IOCTL_I2CCTRL_SMBUS_WRITE_BYTE     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x813, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ---------------------------------------------------------------------------
   Raw I2C IOCTLs
   --------------------------------------------------------------------------- */
#ifndef IOCTL_I2C_WRITE
#define IOCTL_I2C_WRITE                     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_I2C_READ
#define IOCTL_I2C_READ                      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#define IOCTL_I2CCTRL_TRANSFER              CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ---------------------------------------------------------------------------
   SpbCx/ACPIEx façade IOCTLs
   --------------------------------------------------------------------------- */
#define IOCTL_I2CCTRL_SET_TARGET  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CCTRL_SEQUENCE    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CCTRL_PROBE       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ---------------------------------------------------------------------------
   Legacy/compat IOCTLs
   --------------------------------------------------------------------------- */
#define IOCTL_I2CCTRL_SMBUS_BLOCK_READ_LEGACY   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CCTRL_SMBUS_BLOCK_WRITE_LEGACY  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)

#ifndef IOCTL_I2CCTRL_XFER_DESC
#define IOCTL_I2CCTRL_XFER_DESC                 CTL_CODE(FILE_DEVICE_UNKNOWN, 0x822, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* ---------------------------------------------------------------------------
   Additional IOCTLs for helper applet
   --------------------------------------------------------------------------- */
#define IOCTL_I2CCTRL_SELFTEST   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2C_FORCE_CRASH    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x904, METHOD_BUFFERED, FILE_ANY_ACCESS)

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
   Canonical multi-message transfer descriptor
   --------------------------------------------------------------------------- */
#ifndef _I2CCTRL_TRANSFER_DEFINED
#define _I2CCTRL_TRANSFER_DEFINED
typedef struct _I2CCTRL_TRANSFER {
    ULONG       NumMessages;   /* number of messages in this transfer */
    I2CCTRL_MSG Messages[ANYSIZE_ARRAY]; /* variable-length array of messages */
} I2CCTRL_TRANSFER, *PI2CCTRL_TRANSFER;
#endif

/* ---------------------------------------------------------------------------
   Legacy single-transfer descriptor
   --------------------------------------------------------------------------- */
#ifndef _I2CCTRL_XFER_DESC_DEFINED
#define _I2CCTRL_XFER_DESC_DEFINED
typedef struct _I2CCTRL_XFER_DESC {
    ULONG   Length;      /* payload length */
    ULONG   TimeoutMs;   /* timeout */
    ULONG   Flags;       /* transfer flags */
    BOOLEAN IsRead;      /* TRUE for read, FALSE for write */
    UCHAR   Reserved1[3];
} I2CCTRL_XFER_DESC, *PI2CCTRL_XFER_DESC;
#endif

/* ---------------------------------------------------------------------------
   Common request descriptor header
   --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_IO_DESC {
    UCHAR  Address7Bit;
    UCHAR  Command;
    UCHAR  PecMode;
    UCHAR  Reserved;
    USHORT Length;
} I2CCTRL_IO_DESC, *PI2CCTRL_IO_DESC;

/* ---------------------------------------------------------------------------
   Self-test descriptor
   --------------------------------------------------------------------------- */
#ifndef _I2CCTRL_SELFTEST_DEFINED
#define _I2CCTRL_SELFTEST_DEFINED
typedef struct _I2CCTRL_SELFTEST {
    ULONG Pattern;   /* test pattern or flags */
    ULONG Reserved;  /* reserved for future use */
} I2CCTRL_SELFTEST, *PI2CCTRL_SELFTEST;
#endif

/* ---------------------------------------------------------------------------
   Internal opcodes
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

/* ---------------------------------------------------------------------------
   Extra definitions for i2cctrl_helper applet
   --------------------------------------------------------------------------- */

// Registry path constants
#define REG_PATH_I2CHID   L"SYSTEM\\CurrentControlSet\\Services\\i2chid\\Parameters"
#define REG_PATH_I2CCTRL  L"SYSTEM\\CurrentControlSet\\Services\\i2cctrl\\Parameters"

// Resource IDs
#define IDI_APPLET        101
#define IDS_NAME          102
#define IDS_INFO          103
#define IDS_ERR_REGREAD   104
#define IDS_ERR_REGWRITE  105
#define IDS_OK_APPLY      106
#define IDS_ABOUT_TITLE   107
#define IDS_ABOUT_TEXT    108

#define IDD_DIALOG        200
#define IDD_ABOUTBOX      201

#ifndef IDC_STATIC
#define IDC_STATIC        -1
#endif

#define IDC_MULTITOUCH    1001
#define IDC_TAPTOCLICK    1002
#define IDC_SCROLL        1003
#define IDC_SENSITIVITY   1004

#define IDR_ACCELERATOR   300
#define IDR_MENU          301
#define IDM_ABOUT         400

// Global instance handle
extern HINSTANCE g_hInst;

#endif /* _I2CCTRL_IOCTL_H_ */
