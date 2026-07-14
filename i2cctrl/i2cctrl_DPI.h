/* -----------------------------------------------------------------------
   i2cctrl_DPI.h - Device Properties Interface (DPI) for HID-over-I²C (clean, no GUID redefs)
   ----------------------------------------------------------------------- */

#ifndef _I2CCTRL_DPI_H_
#define _I2CCTRL_DPI_H_

#include "i2cctrl_spinlock_fix.h"
#include <hidport.h>   /* for HID_DESCRIPTOR */
/*
* Forward declaration for bus context pointer to avoid circular include
*/
struct _I2CCTRL_FDO;

/*
 * Property keys (deduplicated)
 */
#define DPI_KEY_VENDOR_ID           L"VendorId"
#define DPI_KEY_PRODUCT_ID          L"ProductId"
#define DPI_KEY_VERSION_NUMBER      L"VersionNumber"
#define DPI_KEY_HID_DESCRIPTOR      L"HidDescriptor"
#define DPI_KEY_REPORT_DESCRIPTOR   L"ReportDescriptor"
#define DPI_KEY_REPORT_LENGTH       L"ReportLength"
#define DPI_KEY_I2C_ADDRESS         L"I2cAddr7Bit"

/*
 * DPI structure for bus driver / child PDO context.
 * This container holds all properties i2chid.sys would need.
 */
typedef struct _I2CCTRL_DPI {
    /* Basic HID identifiers */
    USHORT VendorId;
    USHORT ProductId;
    USHORT VersionNumber;

    /* HID descriptors */
    HID_DESCRIPTOR HidDesc;          /* HID descriptor */
    PVOID          HidDescriptor;    /* pointer to HID descriptor blob */
    ULONG          HidDescriptorLength;
    PUCHAR         ReportDescriptor; /* pointer to HID report descriptor */
    USHORT         ReportDescriptorLength;

    /* Report characteristics */
    ULONG  ReportLength;

    /* I²C addressing */
    UCHAR  I2cAddr7Bit;              /* 7-bit I²C address of HID device */

    /* Interrupt resources */
    PKINTERRUPT     InterruptObject;
    BOOLEAN         InterruptConnected;
    ULONG           InterruptVector;
    KIRQL           InterruptIrql;
    KAFFINITY       InterruptAffinity;
    KINTERRUPT_MODE InterruptMode;
    BOOLEAN         InterruptSharable;

    /* ISR -> DPC handoff */
    KDPC            InterruptDpc;
    volatile LONG   PendingInputFlag;

    /* Optional polling fallback */
    KDPC            PollDpc;
    KTIMER          PollTimer;
    ULONG           PollIntervalMs;

    /* Power management */
    SYSTEM_POWER_STATE  SystemState;
    DEVICE_POWER_STATE  DeviceState;
    BOOLEAN             WakeEnabled;

    /* Idle/power timers */
    KTIMER          IdleTimer;
    KDPC            IdleDpc;
    ULONG           IdleTimeoutMs;
    volatile LONG   IdleArmed;

    /* Read queue for pending HID read IRPs */
    LIST_ENTRY      ReadQueue;
    KSPIN_LOCK      ReadQueueLock;

    /* Configuration (registry or feature report) */
    ULONG           PalmThreshold;
    ULONG           ScrollScale;
    ULONG           TapTimeMs;

    /* HID parentage */
    PDEVICE_OBJECT  ParentFdo;       /* link back to bus driver FDO */
    PDEVICE_OBJECT  PhysicalDevice;

    /* Bus driver context (needed for I²C transfers) */
    struct _I2CCTRL_FDO* BusCtx;

    /* Registry path (for parameters) */
    UNICODE_STRING  RegistryPath;

    /* Symbolic link for user-mode access */
    UNICODE_STRING  Symlink;

    /* Device-specific defaults */
    ULONG MaxX;
    ULONG MaxY;
    ULONG Sensitivity;

    /* MMIO region */
    PVOID   MmioBase;    /* virtual address returned by MmMapIoSpace */
    ULONG   MmioLength;  /* length of mapped region in bytes */
	PDEVICE_OBJECT  ControllerDevice;
} I2CCTRL_DPI, *PI2CCTRL_DPI;

/*
 * DPI helper functions (implemented in i2cctrl_DPI.c)
 */
NTSTATUS
I2cCtrl_DpiInitialize(
    PI2CCTRL_DPI dpi,
    USHORT vid,
    USHORT pid,
    USHORT ver,
    ULONG reportLen,
    PVOID desc,
    ULONG descLen,
    PUCHAR reportDesc,
    USHORT reportDescLen,
    UCHAR i2cAddr
    );

NTSTATUS
I2cCtrl_DpiRegisterInterface(
    PDEVICE_OBJECT DeviceObject,
    PUNICODE_STRING SymbolicLinkName
    );

VOID
I2cCtrl_DpiUnregisterInterface(
    PUNICODE_STRING SymbolicLinkName
    );

#endif /* _I2CCTRL_DPI_H_ */
