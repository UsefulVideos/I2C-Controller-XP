#ifndef _I2CCTRL_H_
#define _I2CCTRL_H_

#include <ntddk.h>
#include "i2cctrl_spinlock_fix.h"
#include "i2cctrl_backport.h"
#include "i2cctrl_wdm_compat.h"
#include "i2cctrl_spbcx.h"   /* SPBCX_SEQUENCE_HDR, SPBCX_COMPAT_CONTEXT */
#include "i2cctrl_ioctl.h"   /* IOCTLs and transfer descriptors */
#include "i2cctrl_hw.h"      /* DW_apb_i2c registers and bit masks */
#include "i2cctrl_ext.h"     /* I2CCTRL_GLOBAL, PI2CCTRL_FDO */
#include "i2cctrl_detect.h"

/* ============================================================================
   Driver entry points
   ============================================================================ */
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD     DriverUnload;
DRIVER_ADD_DEVICE I2cCtrl_AddDevice;

DRIVER_DISPATCH   I2cCtrl_DispatchPnP;
DRIVER_DISPATCH   I2cCtrl_DispatchPower;
DRIVER_DISPATCH   I2cCtrl_DispatchIoctl;
DRIVER_DISPATCH   I2cCtrl_InvalidIrp;

NTSTATUS I2cCtrl_AddDevice(
    PDRIVER_OBJECT  DriverObject,
    PDEVICE_OBJECT  PhysicalDeviceObject
    );

/* ============================================================================
   Public detection / helper APIs
   ============================================================================ */
NTSTATUS I2cCtrl_ReadAndValidateHidDescriptor(
    PI2CCTRL_FDO devctx,
    UCHAR                   addr,
    PUCHAR                  outBuf,
    ULONG                   outBufLen,
    PHID_I2C_DESCRIPTOR_V10 parsed
    );

NTSTATUS I2cCtrl_ReadHidRegister(
    PI2CCTRL_FDO devctx,
    UCHAR                   addr,
    UCHAR                   reg,
    PUCHAR                  buffer,
    ULONG                   length,
    ULONG                   timeoutUs
    );

/* ============================================================================
   Child PDO creation/deletion + PDO dispatch
   ============================================================================ */

NTSTATUS I2cCtrl_PdoDispatch(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    );

NTSTATUS I2cCtrl_FdoDispatch(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    );

/* ============================================================================
   Debug / test helpers
   ============================================================================ */
VOID I2cCtrl_ForceCrash(PI2CCTRL_FDO devctx, NTSTATUS reason);

/* ============================================================================
   Controller register helpers
   ============================================================================ */
ULONG I2cCtrl_ReadReg (PI2CCTRL_FDO devctx, ULONG offset);

/* ============================================================================
   ISR and DPC (bottom-half and queue DPC)
   ============================================================================ */

VOID I2cCtrl_DpcRoutine(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2
    );

VOID I2cCtrl_QueueDpcRoutine(PKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1, PVOID SystemArgument2);

/* ============================================================================
   SMBus queue and request processing
   ============================================================================ */
NTSTATUS I2cCtrl_QueueInsert(
    PI2CCTRL_FDO devctx,
    PSMBUS_REQUEST          req
    );

VOID I2cCtrl_QueueKick(
    PI2CCTRL_FDO devctx
    );

VOID I2cCtrl_ProcessSmbusRequest(
    PI2CCTRL_FDO devctx,
    PSMBUS_REQUEST          req
    );

/* Not defined in XP WDK - define manually */
#ifndef IRP_MN_QUERY_LEGACY_BUS_INFORMATION
#define IRP_MN_QUERY_LEGACY_BUS_INFORMATION 0x18
#endif

#endif /* _I2CCTRL_H_ */
