/* -----------------------------------------------------------------------
   i2chid_hw_shim.h
   Interrupt-driven touchpad-like input engine for XP/2003 (C89-compliant)
   Public interface for hardware shim used by i2chid.sys
   ----------------------------------------------------------------------- */

#ifndef _I2CHID_HW_SHIM_H_
#define _I2CHID_HW_SHIM_H_

#include <ntddk.h>
#include "i2chid_ext.h"   /* PI2CHID_FDO */
#include "i2chid.h"       /* shared types/defs */
#include "i2chid_hid.h"   /* HID helpers */
#include "i2chid_contacts.h"

#ifndef CTL_CODE
#include <devioctl.h>
#endif

typedef struct _I2CCTRL_RW {
    UCHAR  Address;     /* 7-bit I2C address */
    UCHAR  Data[1];     /* flexible array tail */
    USHORT Length;      /* payload length */
} I2CCTRL_RW, *PI2CCTRL_RW;

/* -----------------------------------------------------------------------
   Forward declarations (C89 compliant, declared before use)
   ----------------------------------------------------------------------- */
static VOID I2cHw_PollDpc(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArg1,
    PVOID SystemArg2
    );

static NTSTATUS I2cHw_ReadInputPacket(
    PI2CHID_FDO ext,
    PUCHAR buf,
    PULONG bytesRead
    );

static VOID I2cHw_CompleteOneRead(
    PI2CHID_FDO ext,
    PUCHAR data,
    ULONG len
    );

static VOID
I2cHw_BuildMultitouchReport5(
    PUCHAR buf,
    UCHAR contactCount,
    const USHORT x[MT_MAX_CONTACTS],
    const USHORT y[MT_MAX_CONTACTS],
    const UCHAR  id[MT_MAX_CONTACTS],
    const UCHAR  flags[MT_MAX_CONTACTS]
    );


static VOID I2cHw_QueueMouseReport(
    PI2CHID_FDO ext,
    UCHAR btnMask,
    CHAR dx,
    CHAR dy
    );

static VOID I2cHw_AnalyzeGestures(
    PI2CHID_FDO ext,
    const USHORT x[MT_MAX_CONTACTS],
    const USHORT y[MT_MAX_CONTACTS],
    UCHAR count
    );

/* -----------------------------------------------------------------------
   Public ISR/DPC plumbing
   ----------------------------------------------------------------------- */

/* ISR: scheduled by the platform interrupt; queues DPC for input processing */
BOOLEAN
I2CHID_InterruptServiceRoutine(
    PKINTERRUPT Interrupt,
    PVOID ServiceContext
    );

/* Connect/disconnect interrupt */
NTSTATUS
I2cHw_ConnectInterrupt(
    PI2CHID_FDO ext
    );

NTSTATUS
I2cHw_DisconnectInterrupt(
    PI2CHID_FDO ext
    );

/* -----------------------------------------------------------------------
   Controller/power management
   ----------------------------------------------------------------------- */

/* Controller enable (no-op for auto-interrupt devices) */
NTSTATUS
I2cHw_EnableController(
    PI2CHID_FDO ext
    );

/* Power transitions */
NTSTATUS
I2cHw_DeviceResume(
    PI2CHID_FDO ext
    );

NTSTATUS
I2cHw_DeviceSuspend(
    PI2CHID_FDO ext
    );

NTSTATUS
I2cHw_DeviceIdle(
    PI2CHID_FDO ext
    );

/* Wake configuration (programs device wake and masks) */
NTSTATUS
I2cHw_EnableWake(
    PI2CHID_FDO ext,
    BOOLEAN enable
    );

/* Descriptor/programming touch-ups (placeholder for re-init paths) */
NTSTATUS
I2cHw_ReprogramDescriptor(
    PI2CHID_FDO ext
    );

#endif /* _I2CHID_HW_SHIM_H_ */
