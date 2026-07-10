/* i2chid.h
 * Public interface for the HID-over-I2C (PNP0C50) function driver
 * XP/2003-compatible, C89-compliant
 */

#ifndef _I2CHID_H_
#define _I2CHID_H_

#include <ntddk.h>
#include <hidclass.h>
#include <hidport.h>

/* Local driver headers (order chosen to satisfy typedef dependencies) */
#include "I2CHID_EXT.h"      /* PI2CHID_FDO (HID-specific extension) */
#include "I2CHID_DPI.h"      /* DPI types */
#include "I2CHID_ACPI.h"     /* ACPI helpers */
#include "I2CHID_hid.h"      /* HID helpers and prototypes */

/* Shared IOCTLs and bus types */
#include "..\i2cctrl\i2cctrl_ioctl.h"  /* IOCTL_I2C_READ, IOCTL_I2C_WRITE, I2CCTRL_TRANSFER */
#include "..\\i2cctrl\\i2cctrl_detect.h"
#include "..\i2cctrl\i2cctrl_ext.h"    /* Shared I2C types */

/* ---------------------------------------------------------------------------
   Constants
   --------------------------------------------------------------------------- */
#define I2CHID_MAX_FINGERS 5   /* maximum number of simultaneous contacts supported */

/* ---------------------------------------------------------------------------
   Forward declarations for pass-through helper dispatches implemented in I2CHID_pt.c
   --------------------------------------------------------------------------- */
NTSTATUS I2CHID_PT_DispatchInternal(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2CHID_PT_DispatchPass(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* Helpers (optional, used by pass-through module) */
NTSTATUS I2CHID_PT_InitDescriptors(PI2CHID_PT_DEVEXT DevExt);
NTSTATUS I2CHID_PT_ReadRawSample(PI2CHID_PT_DEVEXT DevExt, PPT_RAW_SAMPLE Sample);

VOID
I2CHID_PT_Complete(
    PIRP Irp,
    NTSTATUS Status,
    ULONG_PTR Information
    );

/* ---------------------------------------------------------------------------
   HID driver structures – multitouch + mouse-like report
   --------------------------------------------------------------------------- */

typedef struct _I2CHID_CONTACT {
    BOOLEAN Present;   /* contact slot active */
    LONG    X;         /* absolute X coordinate */
    LONG    Y;         /* absolute Y coordinate */
    LONG    Pressure;  /* pressure value (if supported) */
    LONG    Size;      /* contact size or major axis */
} I2CHID_CONTACT;

typedef struct _I2CHID_REPORT {
    ULONG           ReportId;                     /* HID report ID */
    ULONG           ContactCount;                 /* number of active contacts */
    I2CHID_CONTACT  Contacts[I2CHID_MAX_FINGERS]; /* multitouch contacts */

    /* Touchpad-like absolute coordinates for gesture convenience
       (e.g., centroid of all active contacts or representative finger) */
    LONG            X;                            /* overall X position */
    LONG            Y;                            /* overall Y position */

    /* Physical button states (touchpad click zones) */
    BOOLEAN         BtnLeft;
    BOOLEAN         BtnRight;
    BOOLEAN         BtnMiddle;
    BOOLEAN         BtnX1;
    BOOLEAN         BtnX2;

    /* Timestamp for gesture/tap detection */
    LARGE_INTEGER   Timestamp;                    /* system time when report was generated */
} I2CHID_REPORT, *PI2CHID_REPORT;

/* ---------------------------------------------------------------------------
   Prototypes
   --------------------------------------------------------------------------- */
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD     I2CHID_DriverUnload;
DRIVER_ADD_DEVICE I2CHID_AddDevice;

NTSTATUS I2CHID_DispatchPnP(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2CHID_DispatchPower(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2CHID_DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2CHID_DispatchIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp);

NTSTATUS I2CHID_StartDevice(PI2CHID_FDO Ext,
                            PCM_RESOURCE_LIST Raw,
                            PCM_RESOURCE_LIST Translated);
NTSTATUS I2CHID_StopDevice(PI2CHID_FDO Ext);

/* Interrupt helpers */
VOID     I2CHID_InterruptDpc(PKDPC Dpc,
                             PVOID DeferredContext,
                             PVOID SystemArgument1,
                             PVOID SystemArgument2);

/* Input helpers */
BOOLEAN  I2CHID_ReadInput(PI2CHID_FDO Ext, PUCHAR Buf, ULONG BufLen);
VOID     I2CHID_CompleteQueuedReads(PI2CHID_FDO Ext, PUCHAR Data, ULONG Len);

/* Parsing/building helpers */
VOID     I2CHID_ParseRawPnp0c50(const UCHAR* Raw, ULONG Len, PI2CHID_REPORT Out);
VOID     I2CHID_BuildHidInput(const I2CHID_REPORT* Rpt, UCHAR* Hid, ULONG* HidLen);
NTSTATUS I2CHID_InitHidDescriptors(PI2CHID_FDO Ext);

/* Read queue helpers */
NTSTATUS I2CHID_QueueReadIrp(PI2CHID_FDO Ext, PIRP Irp);
PIRP     I2CHID_DequeueReadIrp(PI2CHID_FDO Ext);
VOID     I2CHID_CompleteReadIrp(PIRP Irp, UCHAR* Data, ULONG Len, NTSTATUS Status);

/* Misc helpers */
NTSTATUS I2CHID_CompletionSignalEvent(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context);
ULONG    I2CHID_GetTickMs(VOID);
VOID     I2CHID_LoadRegistryConfig(PI2CHID_FDO Ext);
NTSTATUS I2CHID_SaveRegistryConfig(PI2CHID_FDO Ext);

/* HID driver alias for controller detection */
NTSTATUS
I2CHID_DetectTouchpad(
    PI2CCTRL_FDO devctx,
    PI2CCTRL_DETECT_RESULT result
    );


#endif /* _I2CHID_H_ */
