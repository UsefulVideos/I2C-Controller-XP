/* i2cctrl_spbcx.h
 * SpbCx-like façade (XP WDM) for i2cctrl.sys
 * Defines IOCTL contract, target context, and entry points.
 * Target: Windows XP x86/x64, C89 compliant
 */

#ifndef _I2CCTRL_SPBCX_H_
#define _I2CCTRL_SPBCX_H_

#include <ntddk.h>
#include "i2cctrl_hw.h"      /* central place for register offsets/bit definitions */
#include "i2cctrl_ioctl.h"   /* canonical IOCTL codes and I2CCTRL_TRANSFER definition */

/* ---------------------------------------------------------------------------
   Versioning and limits
   --------------------------------------------------------------------------- */
#define I2CCTRL_SPBCX_VERSION        0x00010000  /* v1.0 */
#define I2CCTRL_SPBCX_MAX_XFER       0x00010000  /* 64 KiB per transfer */
#define I2CCTRL_SPBCX_MAX_SEQUENCE   128         /* max transfers per sequence */

/* ---------------------------------------------------------------------------
   Device type and IOCTL base
   --------------------------------------------------------------------------- */
#ifndef FILE_DEVICE_I2CCTRL
#define FILE_DEVICE_I2CCTRL          0x8000
#endif

#ifndef I2CCTRL_IOCTL_BASE
#define I2CCTRL_IOCTL_BASE           0x900
#endif

/* ---------------------------------------------------------------------------
   Sequence header (used by IoctlSequence and AcpiEx façade)
   --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_SEQUENCE_HDR {
    ULONG TransferCount;   /* number of transfers in the sequence */
    ULONG Flags;           /* sequence-level flags */
    ULONG OutLength;       /* total bytes expected back (for reads) */
} I2CCTRL_SEQUENCE_HDR, *PI2CCTRL_SEQUENCE_HDR;

/* Provide SPBCX_* aliases for compatibility */
typedef I2CCTRL_SEQUENCE_HDR  SPBCX_SEQUENCE_HDR;
typedef PI2CCTRL_SEQUENCE_HDR PSPBCX_SEQUENCE_HDR;

/* ---------------------------------------------------------------------------
   SpbCx compatibility context
   --------------------------------------------------------------------------- */
typedef struct _SPBCX_COMPAT_CONTEXT {
    ULONG               TargetAddress;       /* 32-bit form of slave address */
    ULONG               TimeoutMs;           /* Timeout for this transfer */
    ULONG               Flags;               /* Transfer flags (PEC, 10-bit, etc.) */
    BOOLEAN             IsRead;              /* TRUE if read, FALSE if write */
    ULONG               BufferLen;           /* Length of Buffer in bytes */
    SPBCX_SEQUENCE_HDR  SequenceHdr;         /* Sequence header for multi-phase ops */
} SPBCX_COMPAT_CONTEXT, *PSPBCX_COMPAT_CONTEXT;

/* ---------------------------------------------------------------------------
   Flags and helpers
   --------------------------------------------------------------------------- */
#define I2CCTRL_FLAG_10BIT_ADDR 0x00000001
#define I2CCTRL_FLAG_10BIT      I2CCTRL_FLAG_10BIT_ADDR  /* alias for compatibility */
#define I2CCTRL_FLAG_PEC        0x00000002
#define I2CCTRL_FLAG_NO_STOP    0x00000004

/* ---------------------------------------------------------------------------
   Public data structures (buffer layouts)
   --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_TARGET_CONFIG {
    USHORT Address;
    USHORT Reserved0;
    ULONG  SpeedHz;
    ULONG  Flags;
} I2CCTRL_TARGET_CONFIG, *PI2CCTRL_TARGET_CONFIG;

/* Probe buffer layout */
typedef struct _I2CCTRL_PROBE {
    USHORT  Address;
    USHORT  Reserved0;
    ULONG   TimeoutMs;
    ULONG   Flags;
    BOOLEAN Present;
    UCHAR   Reserved1[3];
} I2CCTRL_PROBE, *PI2CCTRL_PROBE;

/* Note: I2CCTRL_TRANSFER is defined in i2cctrl_ioctl.h, so we don’t re-define it here. */

/* ---------------------------------------------------------------------------
   Forward declarations of target binding struct
   --------------------------------------------------------------------------- */
struct _I2CCTRL_TARGET;
typedef struct _I2CCTRL_TARGET I2CCTRL_TARGET;
typedef I2CCTRL_TARGET *PI2CCTRL_TARGET;

/* ---------------------------------------------------------------------------
   Dispatch entry points
   --------------------------------------------------------------------------- */
NTSTATUS I2cCtrl_DispatchCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2cCtrl_DispatchClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2cCtrl_SPBCX_DDC(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* ---------------------------------------------------------------------------
   Function prototypes for façade IOCTL handlers (canonical signatures)
   --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_IoctlSetTarget(
    PI2CCTRL_FDO    Dx,
    PI2CCTRL_TARGET Tgt,
    PVOID           InBuf,
    ULONG           InLen
    );

NTSTATUS
I2cCtrl_IoctlTransfer(
    PDEVICE_OBJECT  DeviceObject,
    PI2CCTRL_FDO    Dx,
    PI2CCTRL_TARGET Tgt,
    PVOID           InOutBuf,
    ULONG           InOutLen
    );

NTSTATUS
I2cCtrl_IoctlSequence(
    PDEVICE_OBJECT  DeviceObject,
    PI2CCTRL_FDO    Dx,
    PI2CCTRL_TARGET Tgt,
    PVOID           InOutBuf,
    ULONG           InOutLen
    );

NTSTATUS
I2cCtrl_IoctlProbe(
    PI2CCTRL_FDO    Dx,
    PI2CCTRL_TARGET Tgt,
    PVOID           InOutBuf,
    ULONG           InOutLen
    );

/* ---------------------------------------------------------------------------
   Additional façade helper prototypes
   --------------------------------------------------------------------------- */
NTSTATUS I2cCtrl_StartTransfer(PI2CCTRL_FDO Dx, PSPBCX_COMPAT_CONTEXT Compat);
NTSTATUS I2cCtrl_StartSequence(PI2CCTRL_FDO Dx, PSPBCX_COMPAT_CONTEXT Compat);
NTSTATUS I2cCtrl_StartProbe(PI2CCTRL_FDO Dx, PSPBCX_COMPAT_CONTEXT Compat);

/* ---------------------------------------------------------------------------
   Shared helper prototype (needed by bus side to compile cleanly)
   --------------------------------------------------------------------------- */
NTSTATUS
I2CHID_SendIoctlBuffered(
    PDEVICE_OBJECT TargetDeviceObject,
    ULONG          IoctlCode,
    PVOID          InOutBuffer,
    ULONG          InOutBufferLength
    );

/* ---------------------------------------------------------------------------
   Optional legacy façade IOCTL for single-transfer descriptor
   --------------------------------------------------------------------------- */
#ifndef IOCTL_XFER_DESC
#define IOCTL_XFER_DESC \
    CTL_CODE(FILE_DEVICE_I2CCTRL, I2CCTRL_IOCTL_BASE + 4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#endif /* _I2CCTRL_SPBCX_H_ */
