#ifndef _I2CHID_EXT_H_
#define _I2CHID_EXT_H_

#include <ntddk.h>
#include <hidport.h>
#include "..\i2cctrl\i2cctrl_ext.h"     /* brings in I2CCTRL_FDO and I2CHID_FDO */
#include "..\i2cctrl\i2cctrl_detect.h"  /* brings in I2CCTRL_DETECT_RESULT typedef */
#include "..\i2cctrl\i2cctrl_etw.h"   /* TraceEvents() and WPP flags */
#include "i2chid_i2cctrl.h"
#include "i2chid_hid.h"

/* Named device object and DOS link for pass-through */
#define I2CHID_PT_DEVNAME   L"\\Device\\I2CHID_PT"
#define I2CHID_PT_DOSLINK   L"\\DosDevices\\I2CHID_PT"

/* Tag for remove lock */
#define I2CHID_PT_TAG       'HcPT'   /* four-char tag, e.g. "PTcH" */

/* Forward declaration of controller detection routine */
NTSTATUS
I2cCtrl_DetectTouchpad(
    PI2CCTRL_FDO devctx,
    PI2CCTRL_DETECT_RESULT result
    );

/* ---------------------------------------------------------------------------
   Forward prototypes (do NOT depend on I2CHID_FDO)
   --------------------------------------------------------------------------- */

/* DPC routine scheduled by ISR */
VOID
I2CHID_InterruptDpc(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID Arg1,
    PVOID Arg2
    );

/* ---------------------------------------------------------------------------
   Prototypes that depend on I2CHID_FDO
   --------------------------------------------------------------------------- */

/* Read raw bytes from the device’s input register */
BOOLEAN
I2CHID_ReadInput(
    PI2CHID_FDO Ext,
    PUCHAR Buffer,
    ULONG BufferLen
    );

/* Complete queued HID read IRPs with new input data */
VOID
I2CHID_CompleteQueuedReads(
    PI2CHID_FDO Ext,
    PUCHAR Data,
    ULONG Length
    );

/* Issue an I²C read down to the lower i2cctrl device */
typedef struct _I2C_READ_PARAMS I2C_READ_PARAMS, *PI2C_READ_PARAMS; /* forward declaration */

NTSTATUS
I2CHID_IoctlRead(
    PI2CHID_FDO Ext,
    PI2C_READ_PARAMS Params,
    PUCHAR Buffer,
    ULONG Length
    );

/* ---------------------------------------------------------------------------
   I2CHID_PDO_EXT
   Per‑child PDO extension for HID‑over‑I²C devices.
   Holds cached descriptors, attributes, protocol/idle state, and target info.
   --------------------------------------------------------------------------- */
typedef struct _I2CHID_PDO_EXT {
    /* Back‑pointer to parent controller FDO */
    PI2CCTRL_FDO       ParentFdo;

    /* Device object and symbolic link */
    PDEVICE_OBJECT     Self;
    UNICODE_STRING     SymbolicLink;

    /* ACPI child info (HID/CID, UID, etc.) */
    UNICODE_STRING     HardwareId;
    UNICODE_STRING     InstanceId;

    /* Cached HID descriptors */
    HID_DESCRIPTOR     HidDescriptor;          /* HID device descriptor */
    PUCHAR             ReportDescriptor;       /* raw report descriptor blob */
    ULONG              ReportDescriptorLength;

    /* Device attributes */
    USHORT             VendorId;
    USHORT             ProductId;
    USHORT             Version;

    /* HID protocol and idle rate */
    UCHAR              Protocol;               /* HID_PROTOCOL_REPORT or BOOT */
    UCHAR              IdleRate;               /* idle rate in ms, 0 = indefinite */

    /* Target binding for I²C controller */
    I2CCTRL_TARGET     Target;                 /* address, speed, flags */

    /* Queued read IRPs for input reports */
    LIST_ENTRY         ReadQueue;
    KSPIN_LOCK         ReadQueueLock;

    /* Input report buffer (last received) */
    PUCHAR             InputBuffer;
    ULONG              InputBufferLen;

    /* Power/PnP state */
    BOOLEAN            Started;
    BOOLEAN            Removed;

    /* Diagnostics */
    ULONG              ErrorCount;
    ULONG              Signature;
} I2CHID_PDO_EXT, *PI2CHID_PDO_EXT;

typedef struct _I2CHID_HID_STATIC {
    HID_DESCRIPTOR HidDesc;
    PUCHAR         ReportDesc;
    USHORT         ReportDescLength;
} I2CHID_HID_STATIC, *PI2CHID_HID_STATIC;

/* ---------------------------------------------------------------------------
   HIDCLASS minidriver integration prototypes
   --------------------------------------------------------------------------- */

/* Device/Report descriptors */
NTSTATUS I2CHID_GetDeviceDescriptor(PI2CHID_FDO Ext, PIRP Irp);
NTSTATUS I2CHID_GetReportDescriptor(PI2CHID_FDO Ext, PIRP Irp);
NTSTATUS I2CHID_GetString(PI2CHID_FDO Ext, PIRP Irp);

/* Device attributes */
NTSTATUS I2CHID_GetAttributes(PI2CHID_FDO Ext, PIRP Irp);

/* Report I/O */
NTSTATUS I2CHID_ReadReport(PI2CHID_FDO Ext, PIRP Irp);
NTSTATUS I2CHID_WriteReport(PI2CHID_FDO Ext, PIRP Irp);

/* Feature reports */
NTSTATUS I2CHID_GetFeature(PI2CHID_FDO Ext, PIRP Irp);
NTSTATUS I2CHID_SetFeature(PI2CHID_FDO Ext, PIRP Irp);

/* Protocol and idle rate */
NTSTATUS I2CHID_GetProtocol(PI2CHID_FDO Ext, PIRP Irp);
NTSTATUS I2CHID_SetProtocol(PI2CHID_FDO Ext, PIRP Irp);
NTSTATUS I2CHID_GetIdle(PI2CHID_FDO Ext, PIRP Irp);
NTSTATUS I2CHID_SetIdle(PI2CHID_FDO Ext, PIRP Irp);

/* ---------------------------------------------------------------------------
   Idle / Power helper prototypes
   --------------------------------------------------------------------------- */

/* Arm the idle timer (selective suspend) */
VOID I2CHID_ArmIdle(PI2CHID_FDO Ext);

/* Disarm/cancel the idle timer */
VOID I2CHID_DisarmIdle(PI2CHID_FDO Ext);

/* Transition into D0 (fully powered) */
VOID I2CHID_EnterD0(PI2CHID_FDO Ext);

/* Transition into Dx (low-power state) */
VOID I2CHID_EnterDx(PI2CHID_FDO Ext, DEVICE_POWER_STATE Dx);

#ifndef I2CHID_REQUIRE_PASSIVE
#define I2CHID_REQUIRE_PASSIVE()                                      \
    do {                                                              \
        if (KeGetCurrentIrql() != PASSIVE_LEVEL) {                    \
            KdPrint(("I2CHID: Expected PASSIVE_LEVEL, got IRQL=%lu\n",\
                     (ULONG)KeGetCurrentIrql()));                     \
            ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);              \
        }                                                             \
    } while (0)
#endif

// ---------------------------------------------------------------------------
// Validate HID descriptor bytes and parse fields (HID-I²C v1.0, HAL-neutral, XP-safe)
// ---------------------------------------------------------------------------
BOOLEAN
I2CHID_ParseHidDescriptorV10(
    const UCHAR*              buf,
    ULONG                     len,
    PHID_I2C_DESCRIPTOR_V10   out
    );

/* Candidate address list is defined once in i2chid_detect.c */
extern const UCHAR g_HidI2cCommonCandidates[];
#define HID_I2C_COMMON_CANDIDATES_COUNT \
    (sizeof(g_HidI2cCommonCandidates) / sizeof(g_HidI2cCommonCandidates[0]))



#endif /* _I2CHID_EXT_H_ */
