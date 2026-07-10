/* I2CCTRL_EMU_ext.h
 * Shared header for I2C Controller Emulator (ASUS X509FA 9DE9).
 * XP-safe, C89-compliant.
 */

#ifndef __I2CCTRL_EMU_EXT_H__
#define __I2CCTRL_EMU_EXT_H__

#include <ntddk.h>
#include "..\i2cctrl\i2cctrl_ext.h" /* PI2CCTRL_FDO, I2C_HW_STATUS */

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */
#define I2CCTRL_EMU_DEVICE_NAME      L"\\Device\\I2CCTRL_EMU"
#define I2CCTRL_EMU_DOSLINK_NAME     L"\\DosDevices\\I2CCTRL_EMU"
#define I2CCTRL_EMU_DEFAULT_ADDR     0x15U

/* IOCTL definitions */
#define FILE_DEVICE_I2CCTRL_EMU 0x8330
#define IOCTL_I2CCTRL_EMU_PUSH_REPORT \
    CTL_CODE(FILE_DEVICE_I2CCTRL_EMU, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_I2CCTRL_EMU_RESET \
    CTL_CODE(FILE_DEVICE_I2CCTRL_EMU, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* HID register offsets (emulated) */
#define I2CCTRL_EMU_REG_HID_HEADER   0x01U
#define I2CCTRL_EMU_REG_REPORT_DESC  0x02U
#define I2CCTRL_EMU_REG_INPUT        0x03U
#define I2CCTRL_EMU_REG_OUTPUT       0x04U
#define I2CCTRL_EMU_REG_COMMAND      0x05U
#define I2CCTRL_EMU_REG_DATA         0x06U

/* XP-safe extern declaration: GUID_BUS_TYPE_INTERNAL is missing in WDK 3790 */
#ifndef GUID_BUS_TYPE_INTERNAL
EXTERN_C const GUID GUID_BUS_TYPE_INTERNAL;
#endif

/* ---------------------------------------------------------------------------
 * HID-I2C v1.0 descriptor structure (place before I2CCTRL_EMU_FDO)
 * --------------------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct _HID_I2C_DESCRIPTOR_V10 {
    USHORT wHIDDescLength;
    USHORT bcdVersion;
    USHORT wReportDescLength;
    USHORT wReportDescRegister;
    USHORT wInputRegister;
    USHORT wOutputRegister;
    USHORT wCommandRegister;
    USHORT wDataRegister;
    USHORT wVendorID;
    USHORT wProductID;
    USHORT wVersionID;
} HID_I2C_DESCRIPTOR_V10, *PHID_I2C_DESCRIPTOR_V10;
#pragma pack(pop)

/* ---------------------------------------------------------------------------
 * PCI BusType GUID (not defined on XP/2003 WDK)
 * --------------------------------------------------------------------------- */
#ifndef GUID_BUS_TYPE_PCI
#include <initguid.h>
DEFINE_GUID(
    GUID_BUS_TYPE_PCI,
    0xC8EAE088, 0x1E3C, 0x4B24,
    0x9B, 0x6B, 0x3F, 0x0F, 0x8B, 0x1A, 0x5A, 0x2F
);
#endif


/* ---------------------------------------------------------------------------
 * FIFO structure
 * --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_EMU_FIFO {
    UCHAR  Data[256];
    ULONG  Head;
    ULONG  Tail;
} I2CCTRL_EMU_FIFO, *PI2CCTRL_EMU_FIFO;

/* ---------------------------------------------------------------------------
 * Emulator FDO extension
 * --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_EMU_FDO {
    /* Public-facing controller extension (shared layout with i2cctrl) */
    I2CCTRL_FDO Public;

    /* Emulator-specific state */
    BOOLEAN Enabled;          /* controller enabled flag */
    UCHAR   Target7bit;       /* current target address */
    UCHAR   LastReg;          /* last register written */
    ULONG   RawIntr;          /* raw interrupt flags */
    I2CCTRL_EMU_FIFO RxFifo;  /* receive FIFO */

    /* Config */
    UCHAR   HidAddr;          /* HID device address */

    /* Device objects */
    PDEVICE_OBJECT Self;      /* our FDO */
    UNICODE_STRING DosLink;   /* symbolic link */

    /* Child PDO support (multiple children: 9DC5, 9DE8, 9DE9) */
    PDEVICE_OBJECT ChildPdos[3];   /* enumerated HID child PDOs */
    PWSTR          ChildIds[3];    /* hardware ID strings for each child */
    BOOLEAN        ChildReported[3]; /* whether each child was reported in BusRelations */

    /* HID profile cache */
    HID_I2C_DESCRIPTOR_V10 HidDescriptor; /* cached HID-I2C v1.0 descriptor */
    USHORT ReportDescLength;              /* cached report descriptor length */
	USHORT HidDescLength;				  /* cached HID descriptor length */

    /* ACPI context */
    PDEVICE_OBJECT AcpiPdo;       /* ACPI PDO below our FDO (if present) */
    BOOLEAN        AcpiInterfaceReady; /* flag if ACPI interface was acquired */

    /* Bookkeeping */
    ULONG InstanceId;         /* instance counter for multiple emulators */
	PDEVICE_OBJECT ParentPdo;
	
	PDEVICE_OBJECT LowerDevice;   /* Attached lower device (PDO or next filter) */
} I2CCTRL_EMU_FDO, *PI2CCTRL_EMU_FDO;

typedef struct _I2CCTRL_EMU_PCI_CONFIG {
    USHORT VendorID;    /* 0x00 */
    USHORT DeviceID;    /* 0x02 */
    USHORT Command;     /* 0x04 */
    USHORT Status;      /* 0x06 */
    UCHAR  RevisionID;  /* 0x08 */
    UCHAR  ProgIf;      /* 0x09 */
    UCHAR  SubClass;    /* 0x0A */
    UCHAR  BaseClass;   /* 0x0B */
    UCHAR  CacheLineSize;   /* 0x0C */
    UCHAR  LatencyTimer;    /* 0x0D */
    UCHAR  HeaderType;      /* 0x0E */
    UCHAR  BIST;            /* 0x0F */

    ULONG  Bar[6];      /* 0x10–0x27 */

    ULONG  CardbusCIS;  /* 0x28 */
    USHORT SubVendorID; /* 0x2C */
    USHORT SubSystemID; /* 0x2E */
    ULONG  ExpansionROM;/* 0x30 */
    UCHAR  CapPtr;      /* 0x34 */
    UCHAR  Reserved1[3];
    ULONG  Reserved2;
    UCHAR  InterruptLine;  /* 0x3C */
    UCHAR  InterruptPin;   /* 0x3D */
    UCHAR  MinGrant;       /* 0x3E */
    UCHAR  MaxLatency;     /* 0x3F */
} I2CCTRL_EMU_PCI_CONFIG, *PI2CCTRL_EMU_PCI_CONFIG;


/* ---------------------------------------------------------------------------
 * Emulator PDO (ACPI PNP0C50 synthetic HID-I2C child)
 * --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_EMU_PDO {

    /* Back-pointer to parent FDO */
    PI2CCTRL_EMU_FDO Parent;

    /* Child index (always 0 for single ACPI child) */
    ULONG Index;

    /* ACPI Hardware ID (L"ACPI\\PNP0C50") */
    PWSTR HardwareId;

    /* Optional instance ID ("0", "1", etc.) */
    PWSTR InstanceId;

    /* Reported flag: has this PDO been returned in BusRelations */
    BOOLEAN Reported;

    /* Custom flags */
    ULONG Flags;

    /* ACPI identity */
    PWSTR CompatibleId;       /* same as HardwareId */
    BOOLEAN IsAcpiPnpDevice;  /* TRUE */

    /* HID-I2C descriptor lengths (cached) */
    USHORT HidDescLength;
    USHORT ReportDescLength;

} I2CCTRL_EMU_PDO, *PI2CCTRL_EMU_PDO;

/* ---------------------------------------------------------------------------
 * Function prototypes (ops, HID, IOCTL helpers)
 * --------------------------------------------------------------------------- */

/* Ops */
NTSTATUS I2CCTRL_EMU_Enable(PI2CCTRL_FDO fdo, BOOLEAN on);
NTSTATUS I2CCTRL_EMU_SetTarget7bit(PI2CCTRL_FDO fdo, UCHAR addr);
NTSTATUS I2CCTRL_EMU_IssueWriteByte(PI2CCTRL_FDO fdo, UCHAR reg);
NTSTATUS I2CCTRL_EMU_IssueReadToken(PI2CCTRL_FDO fdo);
NTSTATUS I2CCTRL_EMU_GetStatus(PI2CCTRL_FDO fdo, PI2C_HW_STATUS st);
NTSTATUS I2CCTRL_EMU_ReadRxByte(PI2CCTRL_FDO fdo, PUCHAR out);
VOID     I2CCTRL_EMU_AckInterrupts(PI2CCTRL_FDO fdo, ULONG rawIntr);

/* HID emulation */
VOID   I2CCTRL_EMU_HidInitProfile(PI2CCTRL_EMU_FDO FdoExt);
VOID   I2CCTRL_EMU_HidPrimeForRegister(PI2CCTRL_EMU_FDO FdoExt);
VOID   I2CCTRL_EMU_HidPrimeFullDescriptor(PI2CCTRL_EMU_FDO FdoExt);
USHORT I2CCTRL_EMU_HidGetDescriptorLength(PI2CCTRL_EMU_FDO FdoExt);
USHORT I2CCTRL_EMU_HidGetReportDescriptorLength(PI2CCTRL_EMU_FDO FdoExt);

/* IOCTL dispatch */
NTSTATUS I2CCTRL_EMU_IoctlDispatchBuffered(PI2CCTRL_EMU_FDO FdoExt,
                                   ULONG IoctlCode,
                                   PUCHAR inBuf,
                                   ULONG inLen);

/* ---------------------------------------------------------------------------
 * C89-safe logging macro
 * --------------------------------------------------------------------------- */
extern ULONG g_EmuVerbose;

VOID
I2cCtrl_Emu_Log(
    PCSTR Format,
    ...
    );

/* ACPI helper prototypes (implemented in I2CCTRL_EMU_ACPI.c) */
NTSTATUS I2CCTRL_EMU_AcpiInitialize(PI2CCTRL_EMU_FDO FdoExt);
/* Evaluate ACPI methods and cache any descriptors needed for children */
NTSTATUS I2CCTRL_EMU_AcpiPrimeChildren(PI2CCTRL_EMU_FDO FdoExt, const PWSTR* ids, ULONG count);
/* Set device properties / instance data (e.g., compatible IDs, location) */
NTSTATUS I2CCTRL_EMU_AcpiAttachChildProperties(PI2CCTRL_EMU_FDO FdoExt,
                                               PDEVICE_OBJECT ChildPdo,
                                               PWSTR HardwareId,
                                               ULONG Index);

/* Cached minimal ACPI interface (opaque for XP/2003) */
typedef struct _EMU_ACPI_CONTEXT {
    PDEVICE_OBJECT  AcpiPdo;          /* ACPI PDO below our FDO (if present) */
    BOOLEAN         InterfaceReady;   /* We obtained a usable ACPI interface */
} EMU_ACPI_CONTEXT, *PEMU_ACPI_CONTEXT;

#ifndef GUID_ACPI_INTERFACE_STANDARD
/* {B1B3A3F0-9C79-11D0-9BA3-00A0C922E6EB} */
DEFINE_GUID(GUID_ACPI_INTERFACE_STANDARD,
0xb1b3a3f0, 0x9c79, 0x11d0,
0x9b, 0xa3, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
#endif


#endif /* __I2CCTRL_EMU_EXT_H__ */
