/* gpioctrl_ext.h
 * Shared header for GPIO Controller Driver (gpioctrl.sys)
 * WinDDK 7.1.0 – XP/2003 build environment – C89 compliant
 *
 * Provides:
 * - IOCTL codes and payload structures
 * - Device extension definitions (FDO/PDO)
 * - Public driver prototypes for use across compilation units
 *
 * Notes:
 * - Adjust register offsets and capabilities to match actual hardware.
 * - Keep declarations at the top of blocks in C files for C89 compliance.
 */

#ifndef _GPIOCTRL_EXT_H_
#define _GPIOCTRL_EXT_H_

#include <ntddk.h>
#include "gpioctrl_quirks.h"

/* ---------------------------------------------------------------------------
   Device and IOCTL definitions
   --------------------------------------------------------------------------- */
#define GPIOCTRL_DEVICE_TYPE    FILE_DEVICE_UNKNOWN
#define GPIOCTRL_IOCTL_INDEX    0x800

#define IOCTL_GPIO_READ_PIN       CTL_CODE(GPIOCTRL_DEVICE_TYPE, GPIOCTRL_IOCTL_INDEX + 0x01, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GPIO_WRITE_PIN      CTL_CODE(GPIOCTRL_DEVICE_TYPE, GPIOCTRL_IOCTL_INDEX + 0x02, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GPIO_CONFIGURE_PIN  CTL_CODE(GPIOCTRL_DEVICE_TYPE, GPIOCTRL_IOCTL_INDEX + 0x03, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GPIO_QUERY_CAPS     CTL_CODE(GPIOCTRL_DEVICE_TYPE, GPIOCTRL_IOCTL_INDEX + 0x04, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_GPIO_FORCE_CRASH    CTL_CODE(GPIOCTRL_DEVICE_TYPE, GPIOCTRL_IOCTL_INDEX + 0x05, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ---------------------------------------------------------------------------
   Example register map offsets (relative to MMIO base)
   Adjust to match the target GPIO controller.
   --------------------------------------------------------------------------- */
#define REG_DATA_IN_OFFSET    0x000  /* RO: bit-per-pin */
#define REG_DATA_OUT_OFFSET   0x004  /* R/W: bit-per-pin */
#define REG_DIR_OFFSET        0x008  /* R/W: 0=input, 1=output */
#define REG_PULL_OFFSET       0x00C  /* R/W: 0=none, 1=pull-up (example) */
#define REG_INT_STAT_OFFSET   0x010  /* R/W1C: write 1 to clear */
#define REG_INT_EN_OFFSET     0x014  /* R/W: enable */
#define REG_INT_TYPE_OFFSET   0x018  /* R/W: 0=level, 1=edge */
#define REG_INT_POL_OFFSET    0x01C  /* R/W: 0=low/falling, 1=high/rising */

/* ---------------------------------------------------------------------------
   IOCTL payload structures (METHOD_BUFFERED)
   --------------------------------------------------------------------------- */
typedef struct _GPIOCTRL_CAPS {
    ULONG PinCount;
    ULONG SupportsPull;
    ULONG SupportsInterrupts;
    ULONG SupportsDebounce;
} GPIOCTRL_CAPS, *PGPIOCTRL_CAPS;

typedef struct _GPIO_READ_PIN {
    ULONG Pin; /* 0..PinCount-1 */
} GPIO_READ_PIN, *PGPIO_READ_PIN;

typedef struct _GPIO_WRITE_PIN {
    ULONG Pin;
    ULONG Value; /* 0 or 1 */
} GPIO_WRITE_PIN, *PGPIO_WRITE_PIN;

typedef struct _GPIO_CONFIGURE_PIN {
    ULONG Pin;
    ULONG Direction; /* 0=input, 1=output */
    ULONG Pull;      /* 0=none, 1=pull-up (example) */
    ULONG IntEnable; /* 0/1 */
    ULONG IntType;   /* 0=level, 1=edge */
    ULONG IntPol;    /* 0=low/falling, 1=high/rising */
} GPIO_CONFIGURE_PIN, *PGPIO_CONFIGURE_PIN;

#define GPIO_LOG_SIZE 256

/* ---------------------------------------------------------------------------
   Device extensions
   --------------------------------------------------------------------------- */
typedef struct _GPIOCTRL_PDO_EXT {
    PDEVICE_OBJECT  Self;
    PDEVICE_OBJECT  ParentFdo;
    UNICODE_STRING  SymbolicLink;
    BOOLEAN         Started;
    BOOLEAN         Removed;
    ULONG           PinIndex;        /* if exposing per-pin PDOs; otherwise unused */
} GPIOCTRL_PDO_EXT, *PGPIOCTRL_PDO_EXT;

typedef enum _GPIOCTRL_CONTROLLER_TYPE {
    GpioctrlControllerUnknown = 0,
    GpioctrlControllerLpss,
    GpioctrlControllerCannonLake
} GPIOCTRL_CONTROLLER_TYPE;


typedef struct _GPIOCTRL_FDO_EXT {
    PDEVICE_OBJECT     Self;
    PDEVICE_OBJECT     LowerDevice;

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

    // ISR/DPC circular log buffer
    CHAR               IsrLog[GPIO_LOG_SIZE][64];
    ULONG              IsrLogHead;
    ULONG              IsrLogTail;
    KSPIN_LOCK         IsrLogLock;

    // LPSS power domain control
    PHYSICAL_ADDRESS   PmcBasePa;
    ULONG              PmcLength;
    PUCHAR             PmcBase;

    // New: controller classification
    GPIOCTRL_CONTROLLER_TYPE ControllerType;

} GPIOCTRL_FDO_EXT, *PGPIOCTRL_FDO_EXT;

#define GPIOCTRL_FDO_SIGNATURE  0x474F5041UL /* "GOPA" */

/* ---------------------------------------------------------------------------
   Public driver prototypes
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
VOID     GpioCtrl_LoadRegistryPolicy(PGPIOCTRL_FDO_EXT Ext, PUNICODE_STRING RegistryPath);

/* MMIO helpers */
ULONG    GpioRegRead(PGPIOCTRL_FDO_EXT Ext, ULONG Offset);
VOID     GpioRegWrite(PGPIOCTRL_FDO_EXT Ext, ULONG Offset, ULONG Value);

/* IOCTL handlers */
NTSTATUS GpioCtrl_IoctlReadPin(PGPIOCTRL_FDO_EXT Ext, PIRP Irp);
NTSTATUS GpioCtrl_IoctlWritePin(PGPIOCTRL_FDO_EXT Ext, PIRP Irp);
NTSTATUS GpioCtrl_IoctlConfigurePin(PGPIOCTRL_FDO_EXT Ext, PIRP Irp);
NTSTATUS GpioCtrl_IoctlQueryCaps(PGPIOCTRL_FDO_EXT Ext, PIRP Irp);
VOID GpioCtrl_Log(IN PCCHAR Format, ...);
VOID GpioCtrl_LogIsr(
    PGPIOCTRL_FDO_EXT Ext,
    PCCHAR Format,
    ULONG Value
    );

NTSTATUS
GpioCtrl_EnableLpssPower(
    PGPIOCTRL_FDO_EXT Ext
    );

NTSTATUS
GpioCtrl_EnableLpssClockAndReset(
    PGPIOCTRL_FDO_EXT Ext
    );

VOID
GpioCtrl_FlushIsrLog(
    PGPIOCTRL_FDO_EXT Ext
    );

typedef struct _GPIOCTRL_LOG_WORK {
    CHAR Buffer[512];
    PIO_WORKITEM Item;
} GPIOCTRL_LOG_WORK, *PGPIOCTRL_LOG_WORK;

typedef struct _GPIOCTRL_GLOBAL {
    NTSTATUS (*StartDevice)(PDEVICE_OBJECT DeviceObject, PIRP Irp);
    NTSTATUS (*StopDevice)(PDEVICE_OBJECT DeviceObject, PIRP Irp);

    KSPIN_LOCK GlobalLock;
    LIST_ENTRY DeviceList;
    LONG NextId;

} GPIOCTRL_GLOBAL, *PGPIOCTRL_GLOBAL;

/* Global instance */
extern GPIOCTRL_GLOBAL g_GpioCtrlGlobal;

//
// Lookup table entry for GPIO controllers
//
typedef struct _GPIOCTRL_DEVICE_ID {
    PCWSTR PciId;              /* Hardware ID string to match */

    /* GPIO functional register offsets (BAR0) */
    ULONG  ControlOffset;      /* GPIO control register */
    ULONG  StatusOffset;       /* GPIO status register */
    ULONG  DataOffset;         /* GPIO data register */
    ULONG  MiscOffset;         /* Timing / misc registers */

    /* LPSS power-on register offsets (BAR2) */
    ULONG  LpssClkGateOffset;  /* Clock gate control */
    ULONG  LpssResetOffset;    /* Reset control */
    ULONG  LpssFuncClkOffset;  /* Functional clock enable */
    ULONG  LpssMiscOffset;     /* Misc / status */

    ULONG  Quirks;             /* Functional quirks bitmask */
    ULONG  BsodQuirks;         /* BSOD-tweak-workarounds bitmask */
} GPIOCTRL_DEVICE_ID, *PGPIOCTRL_DEVICE_ID;


/* Extern declarations */
extern const GPIOCTRL_DEVICE_ID g_GpioControllers[];
extern const ULONG g_GpioControllersCount;


#endif /* _GPIOCTRL_EXT_H_ */
