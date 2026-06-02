/* gpioctrl_ext.h
 * Internal header for GPIO Controller Driver (gpioctrl.sys)
 * WinDDK 7.1.0 - XP/2003 build environment - C89 compliant
 *
 * INTERNAL CONTENTS ONLY:
 *  - FDO/PDO extensions
 *  - controller profiles
 *  - quirks
 *  - MMIO offsets
 *  - ISR/DPC prototypes
 *  - internal helpers
 *
 * PUBLIC API IS IN gpioctrl_public.h
 */

#ifndef _GPIOCTRL_EXT_H_
#define _GPIOCTRL_EXT_H_

#include <ntddk.h>

/* Public API (IOCTLs + public structs) */
#include "gpioctrl_public.h"

/* Internal quirks (not visible to i2cctrl.sys) */
#include "gpioctrl_quirks.h"

/* ---------------------------------------------------------------------------
   INTERNAL register map offsets (relative to MMIO base)
   --------------------------------------------------------------------------- */
#define REG_DATA_IN_OFFSET    0x000
#define REG_DATA_OUT_OFFSET   0x004
#define REG_DIR_OFFSET        0x008
#define REG_PULL_OFFSET       0x00C
#define REG_INT_STAT_OFFSET   0x010
#define REG_INT_EN_OFFSET     0x014
#define REG_INT_TYPE_OFFSET   0x018
#define REG_INT_POL_OFFSET    0x01C

#define GPIOCTRL_LOG_SIZE 256

/* ---------------------------------------------------------------------------
   INTERNAL PDO extension
   --------------------------------------------------------------------------- */
typedef struct _GPIOCTRL_PDO {
    PDEVICE_OBJECT  Self;
    PDEVICE_OBJECT  ParentFdo;
    UNICODE_STRING  SymbolicLink;
    BOOLEAN         Started;
    BOOLEAN         Removed;
    ULONG           PinIndex;
} GPIOCTRL_PDO, *PGPIOCTRL_PDO;

/* ---------------------------------------------------------------------------
   INTERNAL controller classification
   --------------------------------------------------------------------------- */
typedef enum _GPIOCTRL_CONTROLLER_TYPE {
    GpioctrlControllerUnknown = 0,
    GpioctrlControllerLpss,
    GpioctrlControllerCannonLake
} GPIOCTRL_CONTROLLER_TYPE;

/* ---------------------------------------------------------------------------
   INTERNAL controller profile table
   --------------------------------------------------------------------------- */
typedef struct _GPIOCTRL_DEVICE_ID {
    PCWSTR PciId;

    /* GPIO functional register offsets (BAR0) */
    ULONG  ControlOffset;
    ULONG  StatusOffset;
    ULONG  DataOffset;
    ULONG  MiscOffset;

    /* LPSS power-on register offsets (BAR2) */
    ULONG  LpssClkGateOffset;
    ULONG  LpssResetOffset;
    ULONG  LpssFuncClkOffset;
    ULONG  LpssMiscOffset;

    ULONG  Quirks;
    ULONG  BsodQuirks;
} GPIOCTRL_DEVICE_ID, *PGPIOCTRL_DEVICE_ID;

extern const GPIOCTRL_DEVICE_ID g_GpioControllers[];
extern const ULONG g_GpioControllersCount;

/* ---------------------------------------------------------------------------
   INTERNAL FDO extension
   --------------------------------------------------------------------------- */
typedef struct _GPIOCTRL_FDO {
    PDEVICE_OBJECT     Self;
    PDEVICE_OBJECT     LowerDevice;
    PDEVICE_OBJECT     Pdo;

    /* MMIO resources */
    PHYSICAL_ADDRESS   MmioBasePa;
    ULONG              MmioLength;
    PUCHAR             MmioBase;

    /* Interrupt resources */
    PKINTERRUPT        InterruptObject;
    ULONG              Vector;
    KIRQL              Irql;
    KIRQL              SynchIrql;
    KAFFINITY          Affinity;
    KINTERRUPT_MODE    InterruptMode;
    BOOLEAN            InterruptConnected;

    /* Capabilities and policy */
    ULONG              PinCount;
    ULONG              SupportsPull;
    ULONG              SupportsInterrupts;
    ULONG              SupportsDebounce;
    ULONG              DebounceDefaultMs;
    ULONG              CrashOnError;

    /* Synchronization */
    KSPIN_LOCK         RegLock;

    /* ISR/DPC state */
    KDPC               IsrDpc;
    volatile ULONG     PendingIntMask;

    /* PnP state */
    BOOLEAN            Started;
    BOOLEAN            Removed;

    /* Diagnostics */
    ULONG              ErrorCount;
    ULONG              Signature;

    /* ISR/DPC circular log buffer */
    CHAR               IsrLog[GPIOCTRL_LOG_SIZE][64];
    ULONG              IsrLogHead;
    ULONG              IsrLogTail;
    KSPIN_LOCK         IsrLogLock;

    /* LPSS power domain control */
    PHYSICAL_ADDRESS   PmcBasePa;
    ULONG              PmcLength;
    PUCHAR             PmcBase;

    /* Controller classification + profile */
    GPIOCTRL_CONTROLLER_TYPE      ControllerType;
    const GPIOCTRL_DEVICE_ID*     ControllerProfile;
    ULONG              QuirkFlags;
    ULONG              BsodPolicy;
	
	    /* Wait-for-interrupt support */
    KEVENT            WaitEvent;
    ULONG             LastFiredPin;
    KSPIN_LOCK        WaitLock;

} GPIOCTRL_FDO, *PGPIOCTRL_FDO;

#define GPIOCTRL_FDO_SIGNATURE  0x474F5041UL /* "GOPA" */

/* ---------------------------------------------------------------------------
   INTERNAL driver prototypes
   --------------------------------------------------------------------------- */
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD     GpioCtrl_Unload;
DRIVER_ADD_DEVICE GpioCtrl_AddDevice;

DRIVER_DISPATCH   GpioCtrl_DispatchPnp;
DRIVER_DISPATCH   GpioCtrl_DispatchPower;
DRIVER_DISPATCH   GpioCtrl_DispatchIoctl;
DRIVER_DISPATCH   GpioCtrl_DispatchCreateClose;

/* ISR/DPC */
BOOLEAN GpioCtrl_Isr(PKINTERRUPT Interrupt, PVOID ServiceContext);
VOID    GpioCtrl_Dpc(PKDPC Dpc, PVOID DeferredContext, PVOID Arg1, PVOID Arg2);

/* PnP helpers */
NTSTATUS GpioCtrl_StartDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS GpioCtrl_StopDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS GpioCtrl_RemoveDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* Registry policy */
VOID     GpioCtrl_LoadRegistryPolicy(PGPIOCTRL_FDO Ext, PUNICODE_STRING RegistryPath);

/* MMIO helpers */
ULONG    GpioRegRead(PGPIOCTRL_FDO Ext, ULONG Offset);
VOID     GpioRegWrite(PGPIOCTRL_FDO Ext, ULONG Offset, ULONG Value);

/* IOCTL handlers (internal side) */
NTSTATUS GpioCtrl_IoctlReadPin(PGPIOCTRL_FDO Ext, PIRP Irp);
NTSTATUS GpioCtrl_IoctlWritePin(PGPIOCTRL_FDO Ext, PIRP Irp);
NTSTATUS GpioCtrl_IoctlConfigurePin(PGPIOCTRL_FDO Ext, PIRP Irp);
NTSTATUS GpioCtrl_IoctlQueryCaps(PGPIOCTRL_FDO Ext, PIRP Irp);

/* Logging */
VOID GpioCtrl_Log(IN PCCHAR Format, ...);
VOID GpioCtrl_LogIsr(PGPIOCTRL_FDO Ext, PCCHAR Format, ULONG Value);

/* LPSS power/clock */
NTSTATUS GpioCtrl_EnableLpssPower(PGPIOCTRL_FDO Ext);
NTSTATUS GpioCtrl_EnableLpssClockAndReset(PGPIOCTRL_FDO Ext);

/* ISR log flush */
VOID GpioCtrl_FlushIsrLog(PGPIOCTRL_FDO Ext);

/* internal IOCTL handler prototype */
NTSTATUS GpioCtrl_IoctlWaitForInterrupt(PGPIOCTRL_FDO Ext, PIRP Irp);

/* Deferred logging work item */
typedef struct _GPIOCTRL_LOG_WORK {
    CHAR Buffer[512];
    PIO_WORKITEM Item;
} GPIOCTRL_LOG_WORK, *PGPIOCTRL_LOG_WORK;

/* Global instance */
typedef struct _GPIOCTRL_GLOBAL {
    NTSTATUS (*StartDevice)(PDEVICE_OBJECT DeviceObject, PIRP Irp);
    NTSTATUS (*StopDevice)(PDEVICE_OBJECT DeviceObject, PIRP Irp);

    KSPIN_LOCK GlobalLock;
    LIST_ENTRY DeviceList;
    LONG NextId;

} GPIOCTRL_GLOBAL, *PGPIOCTRL_GLOBAL;

extern GPIOCTRL_GLOBAL g_GpioCtrlGlobal;

#endif /* _GPIOCTRL_EXT_H_ */
