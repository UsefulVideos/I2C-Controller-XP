/* i2cctrl_ext.h
 * Shared extension structures for the I2C controller (bus) driver
 * and the HID-over-I2C function driver. C89 compliant.
 */

#ifndef _I2CCTRL_EXT_H_
#define _I2CCTRL_EXT_H_

#include <ntddk.h>
#include <hidport.h>
#include <acpiioct.h>
#include "..\i2chid\i2chid_contacts.h"
#include "i2cctrl_sal.h"
#include "i2cctrl_extra.h"
#include "i2cctrl_hal.h"
#include "i2cctrl_hal_ops.h"
#include "i2cctrl_hal_caps.h"
#include "i2cctrl_hw.h"
#include "i2cctrl_zw.h"

struct _SPBCX_COMPAT_CONTEXT;
typedef struct _SPBCX_COMPAT_CONTEXT* PSPBCX_COMPAT_CONTEXT;

/* ---------------------------------------------------------------------------
   Forward prototypes (that do NOT depend on I2CCTRL_FDO)
   --------------------------------------------------------------------------- */

/* KDPC callback used by the bus driver’s interrupt-driven transfers */
VOID
I2cCtrl_DpcRoutine(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArg1,
    PVOID SystemArg2
    );

/* Cancel routine for a pending IRP (registered via IoSetCancelRoutine) */
VOID
I2cCtrl_CancelPending(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
    );

VOID
I2cCtrl_CancelRoutine(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP           Irp
    );

/* -----------------------------------------------------------------------
 * Driver-wide globals used for multi-controller bookkeeping
 * XP/2003 BSOD-safe, C89-compliant
 * ----------------------------------------------------------------------- */
typedef struct _I2CCTRL_GLOBAL {
    LONG       NextControllerId;
    KSPIN_LOCK GlobalLock;       /* protects global controller list */
    LIST_ENTRY ControllerList;   /* list of all FDOs/controllers */

    /* Lifecycle helpers */
    NTSTATUS (*StopDevice)(struct _I2CCTRL_FDO *fdoExt);
    NTSTATUS (*RestartDevice)(struct _I2CCTRL_FDO *fdoExt);
    NTSTATUS (*DetectTouchpad)(struct _I2CCTRL_FDO *fdoExt,
                               struct _I2CCTRL_DETECT_RESULT *result);

    BOOLEAN   TouchpadPresent;
} I2CCTRL_GLOBAL, *PI2CCTRL_GLOBAL;

/* Single instance of driver-wide globals */
extern I2CCTRL_GLOBAL g_I2cCtrlGlobal;

/* ---------------------------------------------------------------------------
   HID-over-I2C function driver device extension (FDO) - fixed, C89-compliant
   ASCII-only strings in comments
   --------------------------------------------------------------------------- */

#ifndef _I2CHID_FDO_DEFINED
#define _I2CHID_FDO_DEFINED

#include "i2cctrl_spinlock_fix.h"
#include "i2cctrl_DPI.h"

/* Forward declaration for bus context pointer */
struct _I2CCTRL_FDO;

/* ---------------------------------------------------------------------------
   Gesture and palm configuration used by the driver
   --------------------------------------------------------------------------- */
#ifndef _I2CHID_CONFIG_DEFINED
#define _I2CHID_CONFIG_DEFINED

typedef struct _I2CHID_CONFIG {
    LONG  PollIntervalMs;      /* poll interval for fallback mode */
    LONG  ScrollScale;         /* scale factor for scroll reporting */
    LONG  PalmThreshold;       /* threshold for palm rejection */
    LONG  TapTimeMs;           /* max interval for tap recognition */
    LONG  TapSizeThreshold;    /* maximum size for a tap */
    LONG  ScrollSizeThreshold; /* minimum size for scroll detection */
    ULONG TapDistance;         /* max movement distance for tap gesture */
    ULONG ZoomScale;           /* threshold for pinch zoom accumulation */
    LONG  RotateScale;         /* threshold for rotation accumulation */
    LONG  SwipeScale;          /* threshold for swipe detection */
} I2CHID_CONFIG;

#endif /* _I2CHID_CONFIG_DEFINED */

/* Gesture state for scroll/zoom/tap detection */

typedef struct _I2CHID_GESTURE_STATE {
    USHORT lastX[MT_MAX_CONTACTS];
    USHORT lastY[MT_MAX_CONTACTS];
    LONG   accumScroll;   /* accumulated scroll delta */
    LONG   accumZoom;     /* accumulated zoom delta */
    LONG   accumRotate;   /* accumulated rotation ticks */
    ULONG  lastTickMs;    /* last gesture timestamp */
} I2CHID_GESTURE_STATE;

/* Support larger HID input reports for modern touchpads */
#define HID_REPORT_MAX_LEN 1024U

/* ---------------------------------------------------------------------------
   FDO extension for I2chid.sys
   --------------------------------------------------------------------------- */
typedef struct _I2CHID_FDO {

    /* PnP state */
    PDEVICE_OBJECT  Self;
    PDEVICE_OBJECT  PhysicalDevice;
    PDEVICE_OBJECT  LowerDevice;
    BOOLEAN         Started;
    BOOLEAN         Removed;

    /* Power management */
    SYSTEM_POWER_STATE  SystemState;
    DEVICE_POWER_STATE  DeviceState;

    /* Idle/power timers */
    KTIMER          IdleTimer;
    KDPC            IdleDpc;
    ULONG           IdleTimeoutMs;
    volatile LONG   IdleArmed;
    BOOLEAN         WakeEnabled;

    /* Interrupt resources */
    PKINTERRUPT     InterruptObject;
    BOOLEAN         InterruptConnected;
    ULONG           InterruptVector;
    KIRQL           InterruptIrql;
    KAFFINITY       InterruptAffinity;
    KINTERRUPT_MODE InterruptMode;
    BOOLEAN         InterruptSharable;

    /* ISR to DPC handoff */
    KDPC            InterruptDpc;
    volatile LONG   PendingInputFlag;

    /* Optional polling fallback */
    KDPC            PollDpc;
    KTIMER          PollTimer;

    /* Read queue for pending HID read IRPs */
    LIST_ENTRY      ReadQueue;
    KSPIN_LOCK      ReadQueueLock;

    /* HID static descriptors */
    struct {
        HID_DESCRIPTOR HidDesc;
        PUCHAR         ReportDesc;
        USHORT         ReportDescLength;
    } HidStatic;

    /* Gesture/palm configuration */
    I2CHID_CONFIG   Cfg;

    /* HID attributes */
    USHORT VendorId;
    USHORT ProductId;
    USHORT VersionNumber;

    /* Last input state (for debouncing, relative motion, etc.) */
    LONG    PrevX;
    LONG    PrevY;
    BOOLEAN LastBtnLeft;
    BOOLEAN LastBtnRight;
    BOOLEAN LastBtnMiddle;
    BOOLEAN LastBtnX1;
    BOOLEAN LastBtnX2;

    /* I2C address of the HID device (7-bit) */
    UCHAR   I2cAddr7Bit;

    /* Registry path (for parameters) */
    UNICODE_STRING RegistryPath;

    /* HID parentage and bus linkage */
    PDEVICE_OBJECT        ParentFdo;   /* link back to bus driver device object */
    struct _I2CCTRL_FDO*  BusCtx;      /* pointer to bus driver context */

    /* Raw descriptor pointers (if used alongside HidStatic) */
    PVOID   HidDescriptor;
    PVOID   ReportDescriptor;

    /* Pass-through / PT additions */
    IO_REMOVE_LOCK RemoveLock;          /* safe remove handling */
    LIST_ENTRY     PendingReads;        /* legacy async read queue */
    KSPIN_LOCK     InputLock;           /* protects PendingReads */
    KEVENT         ReadEvent;           /* signals read availability */

    /* Controller and interface */
    HANDLE         ControllerHandle;    /* opened I2C controller handle */
    UNICODE_STRING Symlink;             /* symbolic link for user-mode access */

    /* Device-specific defaults */
    ULONG MaxX;
    ULONG MaxY;
    ULONG Sensitivity;

    /* MMIO mapping (if applicable) */
    PVOID   MmioBase;                   /* virtual address returned by MmMapIoSpace */
    ULONG   MmioLength;                 /* length of the mapped region in bytes */

    /* DPI context copied from bus (and overridden by registry) */
    I2CCTRL_DPI    Dpi;

    /* New members for ACPI/HID-over-I2C */
    ULONG           HidDescriptorAddress;   /* HID descriptor register address from ACPI _DSM */
    ULONG           InputRegisterAddress;   /* input register base (if provided by ACPI) */
    ULONG           CommandRegisterAddress; /* command register base (optional) */
    ULONG           DataRegisterAddress;    /* data register base (optional) */
    ULONG           ResetRegisterAddress;   /* reset register base (optional) */

    BOOLEAN         AcpiParsed;             /* flag: ACPI resources successfully parsed */
    NTSTATUS        LastStartStatus;        /* diagnostic: last start-device status */
    NTSTATUS        LastStopStatus;         /* diagnostic: last stop-device status */

    UNICODE_STRING  DeviceId;               /* ACPI device ID string (PNP0C50 or vendor-specific) */
    UNICODE_STRING  HardwareIds;            /* ACPI hardware IDs list */
    UNICODE_STRING  CompatibleIds;          /* ACPI compatible IDs list */

    /* Gesture timing */
    LARGE_INTEGER   LastTapTime;

    /* Open handle tracking */
    ULONG           OpenCount;

    /* -----------------------------------------------------------------------
       Missing members added
       ----------------------------------------------------------------------- */

    /* Gesture state for scroll/zoom/tap detection */
	I2CHID_GESTURE_STATE Gest;
	ULONG LastContactCount;
	UCHAR LedState; 
	UCHAR DebugLevel;   /* debug verbosity level */
	UCHAR LastReport[HID_REPORT_MAX_LEN];
} I2CHID_FDO, *PI2CHID_FDO;

#endif /* _I2CHID_FDO_DEFINED */


/* ---------------------------------------------------------------------------
   I2C controller transfer structures (shared)
   --------------------------------------------------------------------------- */

typedef struct _I2C_PHASE {
    PUCHAR  Buffer;     /* Pointer to buffer for this phase */
    ULONG   Length;     /* Total bytes to transfer */
    BOOLEAN IsRead;     /* TRUE = read, FALSE = write */
} I2C_PHASE, *PI2C_PHASE;

#define MAX_PHASES 4

typedef enum _I2C_DIR {
    I2cDirWrite = 0,
    I2cDirRead  = 1
} I2C_DIR;

#define I2C_DIR_WRITE I2cDirWrite
#define I2C_DIR_READ  I2cDirRead

/* ---------------------------------------------------------------------------
   Transfer context: tracks a single I²C transaction, its buffers, retries,
   arbitration state, and completion. XP-safe, WinDDK-compiler-safe, C89-compliant.
   --------------------------------------------------------------------------- */
typedef struct _I2C_TRANSFER_CONTEXT {
    /* -----------------------------------------------------------------------
       Transaction phase sequencing
       ----------------------------------------------------------------------- */
    I2C_PHASE Phases[MAX_PHASES];
    ULONG     NumPhases;
    ULONG     CurrentPhase;
    ULONG     Position;

    /* -----------------------------------------------------------------------
       IRP and buffer tracking
       ----------------------------------------------------------------------- */
    LIST_ENTRY ListEntry;
    PIRP       Irp;
    PUCHAR     Buffer;
    USHORT     Length;
    USHORT     TxIndex;
    USHORT     RxIndex;
    UCHAR      Address7Bit;
    I2C_DIR    Direction;
    NTSTATUS   Status;
    BOOLEAN    StopSeen;

    /* -----------------------------------------------------------------------
       Timeout and retry support
       ----------------------------------------------------------------------- */
    ULONG      RetryCount;
    KTIMER     TimeoutTimer;
    KDPC       TimeoutDpc;

    /* -----------------------------------------------------------------------
       Multi‑master arbitration support
       ----------------------------------------------------------------------- */
    BOOLEAN       MultiMasterEnabled;
    ULONG         ArbAttempt;
    ULONG         ArbMaxRetries;
    ULONG         ArbBackoffBaseUs;
    ULONG         ArbBackoffMaxUs;
    ULONG         ArbBackoffJitterUs;
    ULONG         ArbLostCount;        /* total arbitration losses observed */
    ULONG         ArbConsecutiveLost;  /* consecutive losses counter */
    LARGE_INTEGER LastArbLossTime;

    /* -----------------------------------------------------------------------
       Bus ownership token
       ----------------------------------------------------------------------- */
    LONG          BusOwnerToken;

    /* -----------------------------------------------------------------------
       High‑speed I²C (3.4 MHz) mode support
       ----------------------------------------------------------------------- */
    BOOLEAN       HsEnabled;
    ULONG         HsSclHighCnt;
    ULONG         HsSclLowCnt;
    UCHAR         HsMasterCode;
    ULONG         HsAttempt;
    ULONG         HsErrorCount;
    LARGE_INTEGER LastHsUseTime;

    /* -----------------------------------------------------------------------
       HAL status snapshot for GetStatus calls
       ----------------------------------------------------------------------- */
    I2C_HW_STATUS HwStatus;

    /* -----------------------------------------------------------------------
       Legacy aliases expected by older code paths
       ----------------------------------------------------------------------- */
    BOOLEAN IsRead;       /* mirror of Direction == I2C_DIR_READ */
    ULONG   BufferLen;    /* mirror of Length, widened to ULONG */
	LONG    Completed;
	ULONG Errors;
} I2C_TRANSFER_CONTEXT, *PI2C_TRANSFER_CONTEXT;

typedef struct _I2C_READ_PARAMS {
    UCHAR  Address7Bit;
    UCHAR  Prefix[8];
    ULONG  PrefixLen;
    ULONG  ReadLen;
} I2C_READ_PARAMS, *PI2C_READ_PARAMS;

typedef struct _I2C_WRITE_PARAMS {
    UCHAR  Address7Bit;
    ULONG  Length;
    UCHAR  Data[1];
} I2C_WRITE_PARAMS, *PI2C_WRITE_PARAMS;

/* ---------------------------------------------------------------------------
   I2C controller device extension (bus FDO / PDO shared, improved)
   + SMBus queued IRP support, timeouts/retries, extensions
   --------------------------------------------------------------------------- */

/* QoS priority levels for multi-queue scheduling */
typedef enum _I2CCTRL_QOS_PRIORITY {
    I2C_QOS_HIGH   = 0,   /* urgent requests */
    I2C_QOS_NORMAL = 1,   /* default priority */
    I2C_QOS_LOW    = 2    /* background/low priority */
} I2CCTRL_QOS_PRIORITY;

/* Extended SMBus/I2C request structure with QoS */
typedef struct _SMBUS_REQUEST {
    LIST_ENTRY      ListEntry;        /* linked list entry for queue */
    PIRP            Irp;              /* associated IRP */
    LARGE_INTEGER   DueTime;          /* scheduled time for timeout/retry */
    UCHAR           RetriesRemaining; /* retry counter */
    UCHAR           SlaveAddress;     /* 7-bit address */
    UCHAR           Command;          /* SMBus command/register */
    UCHAR           OpCode;           /* internal opcode */
    UCHAR           PecMode;          /* 0 = off, 1 = on */
    UCHAR           Length;           /* payload length */
    UCHAR           Flags;            /* request flags (PEC, process call, etc.) */
    UCHAR           Buffer[34];       /* payload buffer (≤32 + cmd + PEC) */

    I2CCTRL_QOS_PRIORITY Priority;    /* NEW: QoS priority for multi-queue scheduling */
} SMBUS_REQUEST, *PSMBUS_REQUEST;


/* ---------------------------------------------------------------------------
   I2C controller FDO extension (per controller instance) - genericized
   For multi-controller support.
   Feature-complete: registry-driven policy, timing caches, arbitration/backoff,
   SMBus PEC, HID defaults, power/wake, and compatibility aliases.
   --------------------------------------------------------------------------- */

#ifndef _I2CCTRL_FDO_DEFINED
#define _I2CCTRL_FDO_DEFINED

#include <ntddk.h>
#include "i2cctrl_hal.h"   /* brings in I2C_HW_CAPS, I2C_HW_STATUS, I2C_HW_OPS */

/* ---------------------------------------------------------------------------
   Bus speed mode enum (shared with HAL)
   --------------------------------------------------------------------------- */
typedef enum _I2CCTRL_SPEED_MODE {
    I2cSpeedStandard = 0,
    I2cSpeedFast     = 1,
    I2cSpeedHigh     = 2
} I2CCTRL_SPEED_MODE;

/* ---------------------------------------------------------------------------
 * FDO extension struct
 * --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_FDO {
    /* Identity */
    ULONG             ControllerId;

    /* Device stack */
    PDEVICE_OBJECT    Self;
    PDEVICE_OBJECT    Fdo;             /* legacy alias */
    PDEVICE_OBJECT    ParentFdo;
    PDEVICE_OBJECT    LowerDevice;
    PDEVICE_OBJECT    PhysicalDevice;
    UNICODE_STRING    SymbolicLink;

    /* Resources and hardware mapping */
    PHYSICAL_ADDRESS  MmioPhys;
    PHYSICAL_ADDRESS  PhysAddr;
    PUCHAR            Mmio;
    ULONG             MmioLength;
    PUCHAR            MmioBase;
    PCM_RESOURCE_LIST RawResources;
    PCM_RESOURCE_LIST TranslatedResources;

    PKINTERRUPT       InterruptObject;
    ULONG             IrqVector;
    KIRQL             IrqLevel;
    KAFFINITY         IrqAffinity;
    BOOLEAN           IrqLatched;
    KINTERRUPT_MODE   IrqMode;
    BOOLEAN           IrqSharable;

    /* Concurrency */
    LIST_ENTRY        PendingIrpList; 
    KSPIN_LOCK        BusLock;
    KSPIN_LOCK        QueueLock;
    KSPIN_LOCK        PendingIrpLock;
    KSPIN_LOCK        IoLock;
    KSPIN_LOCK        CancelLock;
    KSPIN_LOCK        HwLock;

    /* Request queue */
    LIST_ENTRY        RequestQueue;
    BOOLEAN           ActiveBusy;
    SMBUS_REQUEST     ActiveRequest;

    /* Pending IRP */
    PIRP              PendingIrp;

    /* Tracing */
    ULONG             TraceFlagsEnableMask;
    UCHAR             MinLevelByFlag[32];

    /* SPB/ACPI integration */
    PVOID             SpbCxHandle;
    PVOID             AcpiHandle;
    BOOLEAN           AcpiBound;
    PDEVICE_OBJECT    AcpiDeviceObject;
    PFILE_OBJECT      AcpiFileObject;

    /* Power state */
    DEVICE_POWER_STATE CurrentDevicePowerState;
    SYSTEM_POWER_STATE SystemPowerState;
    DEVICE_CAPABILITIES Capabilities;
    KEVENT            PowerEvent;
    BOOLEAN           WakeCapable;
    BOOLEAN           WakeArmed;
    ULONG             GpioPin;
    BOOLEAN           GpioActiveLow;
    BOOLEAN           ClockEnabled;

    /* Timeout DPC and timer */
    KDPC              TimeoutDpc;
    KTIMER            TimeoutTimer;

    /* ISR/DPC objects */
    KDPC              IsrDpc;
    KDPC              QueueDpc;
    BOOLEAN           DpcInitialized;

    /* Transfer context */
    I2C_TRANSFER_CONTEXT XferCtx;
    KEVENT            TransferEvent;

    /* Stop/remove coordination */
    KEVENT            StopEvent;

    /* State flags */
    BOOLEAN           Started;
    BOOLEAN           Stopping;
    BOOLEAN           Removed;
    BOOLEAN           InitDone;

    /* Child PDOs */
    LIST_ENTRY        ChildList;
    KSPIN_LOCK        ChildLock;
    BOOLEAN           RescanPending;
    ULONG             NumChildren;

    /* ACPI/PNP strings */
    UNICODE_STRING    HardwareId;
    UNICODE_STRING    InstanceId;
    UNICODE_STRING    RegPath;

    /* Work items */
    PIO_WORKITEM      SelfTestWorkItem;
    ULONG             SelfTestDelayMs;
    UCHAR             SelfTestSlave;
    BOOLEAN           SelfTestEnable;

    /* Interrupt mask (cached) */
    ULONG             IntrMask;

    /* Saved context for power transitions */
    ULONG             SavedBusAddress;
    ULONG             SavedBusSpeed;
    ULONG             SavedTimingHighNs;
    ULONG             SavedTimingLowNs;

    /* Active operating parameters */
    USHORT            TargetAddress;
    ULONG             ClockFrequencyHz;
    ULONG             InputClockFrequencyHz;
    ULONG             CurrentBusSpeed;

    /* Retry/timeout policy */
    ULONG             TransactionTimeoutMs;
    UCHAR             MaxRetries;

    /* Diagnostics */
    LONG              ForceCrashOnError;
    ULONG             ErrorCount;
    ULONG             Signature;

    /* Feature flags */
    BOOLEAN           AlertResponseEnabled;
    BOOLEAN           BlockProcessEnabled;

    /* Bus speed configuration */
    ULONG             SsSclHighCnt;
    ULONG             SsSclLowCnt;
    ULONG             FsSclHighCnt;
    ULONG             FsSclLowCnt;
    ULONG             HsSclHighCnt;
    ULONG             HsSclLowCnt;
    BOOLEAN           HsEnabled;
    UCHAR             HsMasterCode;
    I2CCTRL_SPEED_MODE SpeedMode;
    BOOLEAN           Enabled;

    /* Multi-master arbitration policy */
    BOOLEAN           MultiMasterEnabled;
    ULONG             ArbBackoffBaseUs;
    ULONG             ArbBackoffMaxUs;
    ULONG             ArbBackoffJitterUs;
    ULONG             ArbMaxRetries;
    ULONG             ArbLossCount;
    ULONG             ArbConsecutiveLost;
    LARGE_INTEGER     LastArbLossTime;

    /* Registry-driven policy */
    ULONG             PolicyEnableHighSpeed;
    ULONG             PolicyBusSpeedHz;
    ULONG             PolicyMaxRetries;
    ULONG             PolicyRetryDelayUs;
    ULONG             PolicyTxnTimeoutMs;
    ULONG             PolicyBackoffOnBusy;
    ULONG             PolicyBackoffInitialUs;
    ULONG             PolicyBackoffMaxUs;
    ULONG             PolicyUsePec;
    ULONG             PolicyGpioActiveLow;
    ULONG             PolicyForce10Bit;
    ULONG             PolicyCrashOnError;

    /* Derived runtime state */
    ULONG             ActiveBusSpeedHz;
    BOOLEAN           Use10BitAddrDefault;
    ULONG             BackoffCurrentUs;

    /* PEC and SMBus helpers */
    BOOLEAN           PecEnabled;
    UCHAR             PecLastComputed;
    UCHAR             PecLastReceived;

    /* Controller mode overrides */
    USHORT            AppliedSsHcnt;
    USHORT            AppliedSsLcnt;
    USHORT            AppliedFsHcnt;
    USHORT            AppliedFsLcnt;
    USHORT            AppliedHsHcnt;
    USHORT            AppliedHsLcnt;

    /* Legacy aliases */
    ULONG             Vector;
    KIRQL             Irql;
    KAFFINITY         Affinity;

    /* Robust transfer engine additions */
    ULONG             TxFifoDepth;
    ULONG             RxFifoDepth;
    ULONG             TxWatermark;
    ULONG             RxWatermark;
    ULONG             TransferTimeoutUs;
    ULONG             StallIntervalUs;
    NTSTATUS          LastErrorStatus;
    BOOLEAN           StopPending;
    BOOLEAN           RestartPending;
    BOOLEAN           InterruptsEnabled;
    struct _I2CCTRL_QUEUE* Queue;
    IO_REMOVE_LOCK    RemoveLock;
    BOOLEAN           RemoveLockDrained;

    /* Legacy alias for current IRP */
    PIRP              CurrentIrp;

    ULONG             IntMask;
    IO_CSQ            Csq;
    ULONG             NextChildId;
    BOOLEAN           ChildrenInit;
    ULONG             MaxTransferLen;
    ULONG             MaxMessages;
    ULONG             IrqFlags;
    ULONG             IrqShare;
    BOOLEAN           HardwareFailure;

    /* ACPI capability flags */
    BOOLEAN           AcpiIs20Plus;
    BOOLEAN           SupportsD1;
    BOOLEAN           SupportsD2;
    BOOLEAN           TouchpadPresent;
    
    /* HAL backend linkage */
    const I2C_HW_OPS* Ops;     /* pointer to ops table */
    const I2C_HW_CAPS* Caps;   /* pointer to capabilities struct */
    PVOID             HwPriv;  /* optional backend-private cookie */
    LIST_ENTRY        HighQueue;      /* NEW: High priority queue */
    LIST_ENTRY        NormalQueue;    /* NEW: Normal priority queue */
    LIST_ENTRY        LowQueue;       /* NEW: Low priority queue */

    ULONG             BurstHigh;      /* NEW: fairness counters */
    ULONG             BurstNormal;
    ULONG             BurstLow;

    ULONG             BurstHighMax;   /* NEW: configurable limits (registry or defaults) */
    ULONG             BurstNormalMax;
    ULONG             BurstLowMax;
    BOOLEAN           HotplugPending;   /* NEW: flag set on SURPRISE_REMOVAL */
    BOOLEAN           RebindAllowed;    /* NEW: policy gate for rebind attempts */
    BOOLEAN           ChildrenStale;    /* NEW: mark child PDOs stale for re-enum */

    PIO_WORKITEM      RebindWorkItem; /* NEW: queued work item for rebind */

    /* NEW: ACPI ID cache for child PDO creation */
    BOOLEAN           IdsCached;
    struct {
        WCHAR Hid[64];
        WCHAR Uid[32];
    } CachedIds;

    /* NEW: Child PDO lifecycle hooks */
    VOID (*DeleteChildrenFn)(struct _I2CCTRL_FDO* devctx);
    NTSTATUS (*EnumerateChildrenFn)(PDEVICE_OBJECT Fdo,
                                    struct _I2CCTRL_FDO* devctx,
                                    PULONG ChildCount);
    PIRP    WaitWakeIrp;   // Track issued WaitWake IRP for cancel/disarm
    ULONG   WakeSourceMask; // Bitmask or register snapshot for wake source
	BOOLEAN        ForcePioMode;    /* force PIO path if DMA disabled */
    ULONG          BsodQuirks;      /* BSOD-tweak-workarounds bitmask */
    ULONG TimeoutCount;   /* counts transfer timeouts */
    ULONG HidErrorCount;  /* counts HID-specific errors */
	BOOLEAN SurpriseRemoved;
	
	struct _I2CCTRL_PDO* TouchpadPdo;
	
	PCWSTR PnpId;
	PUCHAR LpssBar2;
    PHYSICAL_ADDRESS LpssBar2Phys;
    ULONG LpssBar2Length;
	
	//
	// LPSS2 register offsets (BAR0)
	//
	ULONG RegCtrl;        // Control register
	ULONG RegStatus;      // Status register
	ULONG RegReset;       // Soft reset
	ULONG RegClkCtl;      // Clock gate control
	ULONG RegClkDiv;      // Clock divider
	ULONG RegClkUpdate;   // Clock update trigger
	ULONG RegIntrMask;    // Interrupt mask
	ULONG ControllerIndex;   /* Index into g_I2cControllers[] */
	PHYSICAL_ADDRESS PwrmBase;   /* Whiskey Lake / CNP-LP PMC PWRMBASE */
    ULONG            PwrmLength;
    BOOLEAN          HavePwrm;

} I2CCTRL_FDO, *PI2CCTRL_FDO;

#endif /* _I2CCTRL_FDO_DEFINED */

NTSTATUS
I2cCtrlIdentifyAndInitController(
    PI2CCTRL_FDO devctx
    );

VOID
I2cCtrl_InstallBackend(
    PI2CCTRL_FDO devctx
    );


/* ---------------------------------------------------------------------------
   Per-handle target binding (FileObject->FsContext)
   Each open handle can bind to a specific slave/target with its own speed/flags.
   --------------------------------------------------------------------------- */
typedef struct _I2CCTRL_TARGET {
    USHORT  Address;        /* 7-bit or 10-bit address (masked to 10 bits) */
    USHORT  Reserved0;      /* alignment/padding */

    ULONG   SpeedHz;        /* requested bus speed for this target */
    ULONG   Flags;          /* I2CCTRL_FLAG_* (10-bit, PEC, etc.) */

    BOOLEAN Bound;          /* TRUE if target has been set */
    UCHAR   Reserved1[3];   /* alignment/padding */

    /* Extended diagnostics / policy */
    ULONG   LastError;      /* last error code for this target */
    ULONG   RetryCount;     /* retries attempted on last transfer */
    ULONG   PecEnabled;     /* 0/1: PEC enforced for this target */

    /* Timing snapshot (applied when bound) */
    USHORT  AppliedHcnt;
    USHORT  AppliedLcnt;
} I2CCTRL_TARGET, *PI2CCTRL_TARGET;

/* ---------------------------------------------------------------------------
   Prototypes that depend on I2CCTRL_FDO
   --------------------------------------------------------------------------- */

/* ACPI child enumeration and teardown */
NTSTATUS
I2cCtrl_EnumerateAcpiChildren(
    PDEVICE_OBJECT Fdo,
    PI2CCTRL_FDO   DevCtx,
    PULONG         ChildCountOut
    );

NTSTATUS
I2cCtrl_CreateChildPdo(
    PDEVICE_OBJECT ParentFdo,
    PI2CCTRL_FDO   fdoExt,
    PWSTR          hidBuf,
    PWSTR          uidBuf
    );

VOID
I2cCtrl_DeleteChildPdos(
    PI2CCTRL_FDO devctx
    );

/* --- Queued SMBus prototypes --- */
NTSTATUS
I2cCtrl_EnqueueSmbusIrp(
    PI2CCTRL_FDO DevCtx,
    PIRP         Irp,
    UCHAR        SlaveAddress,
    UCHAR        Command,
    UCHAR        OpCode,
    PUCHAR       Buffer,
    UCHAR        Length,
    UCHAR        PecMode
    );

/* Bus speed configuration (registry‑driven) */
NTSTATUS
I2cCtrl_SetBusSpeed(
    PI2CCTRL_FDO Dx,
    int          Mode
    );

/* Extended SMBus opcodes */
#define I2CCTRL_OPCODE_BLOCK_PROCESS_CALL   0x0B  /* write block + read block */

/* Request flags used internally by SMBUS_REQUEST */
#define SMBUS_REQ_FLAG_PEC           0x01  /* PEC calculation/validation required */
#define SMBUS_REQ_FLAG_PROCESS_CALL  0x02  /* block process call sequence */

/* Queue processor */
NTSTATUS
I2cCtrl_ProcessQueue(
    PI2CCTRL_FDO DevCtx
    );

/* Cancel routine for queued IRPs */
VOID
I2cCtrl_CancelQueuedIrp(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    );

/* Start transfer (shared between core and SPBCx compatibility layer) */
NTSTATUS
I2cCtrl_StartTransfer(
    PI2CCTRL_FDO          DevCtx,
    PSPBCX_COMPAT_CONTEXT Compat
    );

/* ---------------------------------------------------------------------------
   Registry‑driven policy helpers
   --------------------------------------------------------------------------- */

/* Load registry parameters into FDO policy fields */
NTSTATUS
I2cCtrl_LoadRegistryPolicy(
    PI2CCTRL_FDO Dx
    );

/* Apply controller policy (speed, addressing, PEC, backoff) */
VOID
I2cCtrl_ApplyControllerPolicy(
    PI2CCTRL_FDO Dx,
    PI2CCTRL_TARGET TgtOpt
    );

/* Run transfer with retries/backoff/timeout policy */
NTSTATUS
I2cCtrl_RunTransferWithPolicy(
    PI2CCTRL_FDO Dx
    );

/* Backoff helper for busy FIFO/bus */
VOID
I2cCtrl_Backoff(
    PI2CCTRL_FDO Dx,
    ULONG       *DelayUs
    );

/* PEC calculation helper (SMBus CRC‑8, polynomial 0x07, initial 0) */
UCHAR
I2cCtrl_ComputePec(
    IN const UCHAR* bytes,
    IN SIZE_T       count
    );

/* ---------------------------------------------------------------------------
   SMBus PEC (CRC-8) helpers
   --------------------------------------------------------------------------- */

/* CRC-8 for SMBus PEC (poly 0x07, init 0x00) */
UCHAR
Smbus_Crc8(
    const UCHAR* data,
    size_t len
    );

/* Build write frame for PEC and append computed PEC.
 * Frame for PEC calculation: [Addr<<1 | 0] [Command] [Payload...]
 * Emits [Command][Payload...][PEC] into outBuf; outLen returns total bytes.
 * Returns TRUE on success.
 */
BOOLEAN
Smbus_BuildWriteFrameAndAppendPec(
    UCHAR addr7,
    UCHAR command,
    const UCHAR* payload,
    UCHAR payloadLen,
    UCHAR* outBuf,
    UCHAR* outLen
    );

/* Validate read frame PEC.
 * Frame for PEC calculation:
 *   [Addr<<1 | 0] [Command] [Addr<<1 | 1] [Payload...]
 * payloadWithPec must be [Payload...][PEC]; payloadLenWithPec includes PEC.
 * Returns TRUE if PEC matches, FALSE otherwise.
 */
BOOLEAN
Smbus_BuildReadFrameAndValidatePec(
    UCHAR addr7,
    UCHAR command,
    const UCHAR* payloadWithPec,
    UCHAR payloadLenWithPec
    );

// ---------------------------------------------------------------------------
// Child device description (parsed from ACPI _HID/_CID/_ADR/_UID/_STA)
// ---------------------------------------------------------------------------
typedef struct _I2C_CHILD_DESC {
    UNICODE_STRING HardwareId;    // e.g., "ACPI\\PNP0C50" or "I2C\\VID_xxxx&PID_yyyy"
    UNICODE_STRING CompatibleId;  // optional compatible ID
    ULONG          Address;       // I2C 7-bit address or ACPI _ADR value
    ULONG          UniqueId;      // from _UID if present
    BOOLEAN        Present;       // from _STA
	UNICODE_STRING InstanceId;
	UNICODE_STRING      DeviceId;
UNICODE_STRING      HardwareIds;
UNICODE_STRING      CompatibleIds;

GUID                IfGuid;
BOOLEAN             IfEnabled;

} I2C_CHILD_DESC, *PI2C_CHILD_DESC;

// ---------------------------------------------------------------------------
// PDO extension (per child device) - genericized
// ---------------------------------------------------------------------------
typedef enum _I2CCTRL_PDO_TYPE {
    I2CCTRL_PDO_TYPE_GENERIC = 0,
    I2CCTRL_PDO_TYPE_HID     = 1,
    I2CCTRL_PDO_TYPE_CHILD   = 2
} I2CCTRL_PDO_TYPE;

typedef struct _I2CCTRL_PDO {

    //
    // --- Common header (must come first) ---
    //
    ULONG              Signature;      // 'PDOI'
    I2CCTRL_PDO_TYPE   Type;           // identifies which fields are valid
    LIST_ENTRY         ListEntry;      // linked list entry in FDO->ChildList
    PDEVICE_OBJECT     Pdo;            // child PDO object
    PI2CCTRL_FDO       ParentFdo;      // back-pointer to parent FDO extension

    //
    // --- ACPI/PNP descriptor info (generic children) ---
    //
    I2C_CHILD_DESC     Desc;
    BOOLEAN            Present;
    BOOLEAN            Reported;
    BOOLEAN            Started;
    BOOLEAN            Removed;
    BOOLEAN            SurpriseRemoved;
    BOOLEAN            Enumerated;

    DEVICE_POWER_STATE CurrentPowerState;
    DEVICE_CAPABILITIES Capabilities;
    SYSTEM_POWER_STATE  SystemPowerState;

    UNICODE_STRING     HardwareId;
    UNICODE_STRING     InstanceId;
    UNICODE_STRING     CompatibleId;
    UNICODE_STRING     InstancePath;
    UNICODE_STRING     Location;

    PWSTR              HardwareIdsMultiSz;
    PWSTR              CompatibleIdsMultiSz;

    ULONG              ErrorCount;
    ULONG              LastErrorCode;

    ULONG              SavedBusAddress;
    ULONG              SavedBusSpeed;
    ULONG              SavedTimingHighNs;
    ULONG              SavedTimingLowNs;

    ULONG              TransactionTimeoutMs;
    UCHAR              MaxRetries;
    ULONG              RetryDelayUs;

    BOOLEAN            AlertResponseEnabled;
    BOOLEAN            BlockProcessEnabled;
    BOOLEAN            MultiMasterCapable;
    BOOLEAN            WakeCapable;

    PVOID              AcpiHandle;
    BOOLEAN            Stopping;

    //
    // --- HID-specific fields (HID-over-I2C devices only) ---
    //
    USHORT             HidSlaveAddress;

    ULONG              HidDescRegister;
    ULONG              HidInputRegister;
    ULONG              HidOutputRegister;
    ULONG              HidCommandRegister;
    ULONG              HidDataRegister;

    ULONG              HidReportDescLen;
    ULONG              HidMaxInputLen;
    ULONG              HidMaxOutputLen;
    ULONG              HidMaxFeatureLen;

    BOOLEAN            HidHasGpioInt;
    ULONG              HidGpioPin;
    BOOLEAN            HidGpioActiveLow;

    HID_DESCRIPTOR        HidDesc;
    PUCHAR                HidReportDesc;
    HID_DEVICE_ATTRIBUTES HidAttrs;

    PIRP               HidPendingReadIrp;
    UCHAR              HidInputQueue[1024];
    ULONG              HidInputHead;
    ULONG              HidInputTail;
    KSPIN_LOCK         HidInputLock;
    BOOLEAN            HidReady;

    ULONG              HidErrorCount;
    LARGE_INTEGER      HidLastReportTime;
    BOOLEAN            HidSuspended;
    KEVENT             HidReportEvent;

    //
    // --- CHILD_PDO_EXT fields (legacy child type) ---
    //
    ULONG              ChildId;
    BOOLEAN            ChildInitialized;

    UNICODE_STRING     ChildDeviceId;
    UNICODE_STRING     ChildHardwareIds;
    UNICODE_STRING     ChildCompatibleIds;
    UNICODE_STRING     ChildInstanceId;

    UNICODE_STRING     ChildIfSymLink;
    GUID               ChildIfGuid;
    BOOLEAN            ChildIfEnabled;

    LIST_ENTRY         ChildLink;
    PI2CCTRL_FDO       ChildParentCtx;
    PDEVICE_OBJECT     ChildSelf;

/* --- Missing HID fields --- */

PIRP                PendingHidReadIrp;
USHORT              SlaveAddress;
ULONG               DataRegister;
BOOLEAN             HidHasDsmHidDescriptor;  /* TRUE if _DSM HID descriptor was used */

//
// ACPI-derived GPIO interrupt info
//
ULONG   GpioLevel;      /* synthetic IRQ level for PDO */
ULONG   GpioVector;     /* synthetic IRQ vector for PDO */
BOOLEAN GpioActiveLow;  /* polarity from ACPI flags */
BOOLEAN GpioValid;      /* TRUE if GPIO descriptor was parsed */

/*  last HID input report (raw bytes) */
UCHAR              LastReport[HID_REPORT_MAX_LEN];
ULONG              LastReportLen;

BOOLEAN IsTouchpad;              // HID_FLAG_TOUCHPAD

ULONG   HidExtraDelayUs;         // ELAN, Silead, ASUS timing quirks
BOOLEAN HidNeedsAlignmentFix;    // ELAN alignment quirk
BOOLEAN HidSynapticsFix;         // Synaptics packet alignment
ULONG   HidPacketHeaderSize;     // Goodix/Raydium/Himax header size
BOOLEAN HidDebounceFix;          // ASUS / Chicony debounce
BOOLEAN HidSlowRead;             // Goodix slow-read mode
BOOLEAN HidRaydiumMode;          // Raydium 2-byte header
BOOLEAN HidScaleCoordinates;     // FocalTech scaling
BOOLEAN HidFilterInterrupts;     // Cypress interrupt filter
BOOLEAN HidHimaxMode;            // Himax packet mode
BOOLEAN HidPixartChecksum;       // PixArt checksum quirk
BOOLEAN HidSileadMode;           // Silead timing quirk
BOOLEAN HidAtmelHeaderFix;       // Atmel header quirk
BOOLEAN HidPrimaxMode;           // Primax mode

} I2CCTRL_PDO, *PI2CCTRL_PDO;

typedef struct _I2CCTRL_IRP_CONTEXT {
	IO_CSQ_IRP_CONTEXT CsqContext; /* must be first */
	LIST_ENTRY        ListEntry;       /* linked into pending queue */
    PIRP              Irp;             /* owning IRP */
    BOOLEAN           Canceled;        /* set by cancel routine */
    LARGE_INTEGER     Deadline;        /* absolute timeout deadline */

    /* Extended async/cancel support */
    ULONG             Opcode;          /* which IOCTL/operation this IRP represents */
    ULONG             BytesCompleted;  /* number of bytes transferred so far */
    NTSTATUS          CompletionStatus;/* final status to report */
    PVOID             UserBuffer;      /* pointer to METHOD_BUFFERED system buffer */

    /* Additional telemetry for support */
    LARGE_INTEGER     EnqueueTime;     /* when IRP was queued */
    LARGE_INTEGER     StartTime;       /* when hardware execution began */
    LARGE_INTEGER     EndTime;         /* when IRP completed */
    ULONG             RetryCount;      /* number of retries attempted */
    BOOLEAN           TimedOut;        /* TRUE if deadline expired */
    BOOLEAN           Completed;       /* TRUE once IoCompleteRequest called */
} I2CCTRL_IRP_CONTEXT, *PI2CCTRL_IRP_CONTEXT;


typedef struct _I2CCTRL_QUEUE {
    LIST_ENTRY    PendingIrps;
    KSPIN_LOCK    Lock;
    KEVENT        WorkEvent;
    HANDLE        WorkerHandle;
    BOOLEAN       Running;

    ULONG         Depth;        // number of IRPs currently queued
    ULONG         CancelCount;  // number of IRPs canceled
    ULONG         TimeoutCount; // number of IRPs timed out
    LARGE_INTEGER LastActivity; // timestamp of last dequeue
} I2CCTRL_QUEUE, *PI2CCTRL_QUEUE;


VOID I2cCtrl_AcpiCloseChild(PI2CCTRL_PDO ChildDx);

/* Power/wake helpers */
VOID I2cCtrl_CompleteWakeIfArmed(PI2CCTRL_FDO devctx);

/* ============================================================================
   Start/Stop device lifecycle
   ============================================================================ */
NTSTATUS I2cCtrl_StartDevice(PI2CCTRL_FDO fdoExt, PIRP Irp);
NTSTATUS I2cCtrl_QueryDeviceRelations(PI2CCTRL_FDO fdoExt, PIRP Irp);
NTSTATUS I2cCtrl_StopDevice(PI2CCTRL_FDO fdoExt);
NTSTATUS I2cCtrl_RestartDevice(PI2CCTRL_FDO fdoExt);
VOID     I2cCtrl_AcpiCloseHandle(PVOID AcpiHandle);

NTSTATUS
I2cCtrl_ReadBlock(
    PI2CCTRL_FDO fdo,
    USHORT       slaveAddr,
    ULONG        reg,
    PUCHAR       buffer,
    ULONG        length
    );

NTSTATUS
I2cCtrl_WriteBlock(
    PI2CCTRL_FDO fdo,
    USHORT       slaveAddr,
    ULONG        reg,
    const PUCHAR buffer,
    ULONG        length
    );

/* ---------------------------------------------------------------------------
 * FIFO and transfer engine helpers
 * --------------------------------------------------------------------------- */
NTSTATUS I2cCtrl_InitFifos(PI2CCTRL_FDO fdoExt);
NTSTATUS I2cCtrl_CheckAckAndClear(PI2CCTRL_FDO fdoExt);

NTSTATUS I2cCtrl_WriteBurstPolled(
    PI2CCTRL_FDO fdoExt,
    USHORT       slaveAddr,
    const UCHAR *buf,
    ULONG        len,
    BOOLEAN      issueRestart,
    BOOLEAN      issueStop
    );

NTSTATUS I2cCtrl_ReadBurstPolled(
    PI2CCTRL_FDO fdoExt,
    USHORT       slaveAddr,
    PUCHAR       buf,
    USHORT       len,
    BOOLEAN      issueRestart,
    BOOLEAN      issueStop
    );

typedef struct _I2CHID_PT_DEVEXT {
    /* Device objects */
    PDEVICE_OBJECT      Self;
    PDEVICE_OBJECT      LowerDevice;
    UNICODE_STRING      Symlink;
    IO_REMOVE_LOCK      RemoveLock;

    /* I2C HID descriptor and report */
    USHORT              I2cAddress;        /* device I2C address */
    HID_DESCRIPTOR      HidDesc;
    PUCHAR              ReportDesc;
    ULONG               ReportDescLen;

    /* Input threading/queue */
    KSPIN_LOCK          InputLock;
    LIST_ENTRY          PendingReads;
    KEVENT              ReadEvent;

    /* State for emulation */
    BOOLEAN             LastPrimaryDown;
    USHORT              lastX;
    USHORT              lastY;

    /* Tap timing (very naive) */
    LARGE_INTEGER       LastDownTime;
    UCHAR               LastDownContacts;

    /* Feature buffer (no real features on XP) */
    UCHAR               FeatureBuffer[16];

    /* Scaling */
    USHORT              MaxX;
    USHORT              MaxY;
    UCHAR               Sensitivity;       /* scale factor for relative movement */

    /* Read queue for IOCTL_HID_READ_REPORT */
    LIST_ENTRY          ReadQueue;
    KSPIN_LOCK          ReadQueueLock;

    /* Handle to I2C controller */
    HANDLE              ControllerHandle;

    /* Touchpad gesture state */
    LONG                LastPinchDistSq;   /* squared distance for pinch/zoom tracking */
	
	
    /* -----------------------------------------------------------------------
       Added members for PT sample caching
       ----------------------------------------------------------------------- */
    BOOLEAN             SampleValid;       /* flag: sample contains valid data */
    KSPIN_LOCK          SampleLock;        /* protects LastSample/SampleValid */
	struct _I2CHID_FDO *FdoExt; 
} I2CHID_PT_DEVEXT, *PI2CHID_PT_DEVEXT;



/* ---------------------------------------------------------------------------
 * Interrupt helpers
 * --------------------------------------------------------------------------- */
VOID I2cCtrl_EnableXferInterrupts(PI2CCTRL_FDO fdoExt);
BOOLEAN I2cCtrl_Isr(PKINTERRUPT Interrupt, PVOID ServiceContext);

/* IRP major function handlers */
NTSTATUS I2cCtrlCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2cCtrlClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2cCtrlCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2cCtrlDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* Buffer validation helper */
NTSTATUS ValidateBufferedInput(PIRP Irp, size_t required);

/* --- SMBus helper enums and structs (METHOD_BUFFERED) --------------------- */

/* Enumerated SMBus operation types */
typedef enum _I2CCTRL_SMBUS_OP {
    SmBusQuickCommand = 0,
    SmBusSendByte,
    SmBusReceiveByte,
    SmBusWriteByte,
    SmBusReadByte,
    SmBusWriteWord,
    SmBusReadWord,
    SmBusBlockWrite,
    SmBusBlockRead,
    SmBusBlockProcessCall,   /* new */
    SmBusAlertResponse       /* new */
} I2CCTRL_SMBUS_OP;

/* -----------------------------------------------------------------------
 * Generic SMBus command descriptor
 * Used for Quick, SendByte, ReceiveByte, WriteByte, ReadByte,
 * WriteWord, ReadWord, BlockWrite, BlockRead, BlockProcessCall,
 * AlertResponse.
 * ----------------------------------------------------------------------- */
typedef struct _I2CCTRL_SMBUS_CMD {
    UCHAR  Address7Bit;   /* 0..127 */
    UCHAR  Command;       /* SMBus command code (if applicable) */
    UCHAR  PecMode;       /* 0=off, nonzero=on */
    UCHAR  Reserved;      /* must be 0 */
    ULONG  Flags;         /* READ/STOP/RESTART */
    UCHAR  Data[0];       /* payload placeholder (0..32 bytes) */
} I2CCTRL_SMBUS_CMD, *PI2CCTRL_SMBUS_CMD;

/* -----------------------------------------------------------------------
 * SMBus block operation descriptor
 * Used for BlockWrite, BlockRead, BlockProcessCall.
 * ----------------------------------------------------------------------- */
typedef struct _I2CCTRL_SMBUS_BLOCK {
    UCHAR  Address7Bit;   /* 0..127 */
    UCHAR  Command;       /* SMBus command code */
    UCHAR  PecMode;       /* 0=off, nonzero=on */
    UCHAR  Length;        /* 1..32 */
    ULONG  Flags;         /* STOP/RESTART; READ ignored for block read */
    UCHAR  Data[0];       /* payload (Length bytes for write; output for read) */
} I2CCTRL_SMBUS_BLOCK, *PI2CCTRL_SMBUS_BLOCK;


/* Context for SMBus read/write retry wrapper */
typedef struct _SMBUS_RW_CTX {
    PI2CCTRL_FDO fdo;   /* device context */
    USHORT       addr;  /* 7-bit slave address */
    PUCHAR       buf;   /* pointer to data buffer */
    ULONG        len;   /* number of bytes */
    BOOLEAN      restart; /* issue repeated start */
    BOOLEAN      stop;    /* issue stop */
    BOOLEAN      isRead;  /* TRUE = read, FALSE = write */
} SMBUS_RW_CTX, *PSMBUS_RW_CTX;

NTSTATUS
I2cCtrl_AcpiOpen(
    PI2CCTRL_FDO fdoExt
    );

NTSTATUS
I2cCtrl_EnumerateAcpiNamespace(
    PDEVICE_OBJECT AcpiPdo,
    PVOID          ParentHandle,
    ULONG          Depth,
    PI2CCTRL_FDO   FdoExt
    );

//
// Minimal description of one ACPI device entry returned by
// IOCTL_ACPI_GET_DEVICE_INFORMATION (hybrid traversal root walk).
//
typedef struct _I2CCTRL_ACPI_ENUM_ENTRY {
    ULONG NextRequest;     // IN: index to request (0,1,2,...)
    PVOID DeviceHandle;    // OUT: opaque ACPI handle
    ULONG DeviceStatus;    // OUT: _STA bits (if ACPI fills them)
} I2CCTRL_ACPI_ENUM_ENTRY, *PI2CCTRL_ACPI_ENUM_ENTRY;

NTSTATUS
I2cCtrl_AcpiGetDeviceInformation(
    PDEVICE_OBJECT             AcpiPdo,
    PVOID                      DeviceHandle,   // reserved for future use
    PI2CCTRL_ACPI_ENUM_ENTRY   Info,
    ULONG                      InfoLength
    );

//
// Private copy of the front of ACPI_DEVICE_INFORMATION as used by
// IOCTL_ACPI_GET_DEVICE_INFORMATION. We only care about a few fields.
//
typedef struct _I2CCTRL_ACPI_DEVICE_INFORMATION_WIRE {
    ULONG Signature;       // filled by ACPI, ignored here
    ULONG Length;          // total length of this structure
    ULONG NextRequest;     // IN: index to request
    PVOID DeviceHandle;    // OUT: opaque handle
    ULONG DeviceStatus;    // OUT: _STA bits
    // The real structure has more fields; we don't need them.
} I2CCTRL_ACPI_DEVICE_INFORMATION_WIRE, *PI2CCTRL_ACPI_DEVICE_INFORMATION_WIRE;

/* FIFO helpers used by surprise-remove quiesce */
VOID I2cCtrl_DrainRxFifoBounded(PI2CCTRL_FDO FdoExt);
VOID I2cCtrl_FlushTxFifoBounded(PI2CCTRL_FDO FdoExt);

/* Queue cleanup helper used by surprise-remove quiesce */
VOID I2cCtrl_CancelAllQueuedTransfers(PI2CCTRL_FDO FdoExt);

/* FIFO predicates and operations */
BOOLEAN  I2cCtrl_IsTxFifoNotEmpty(PI2CCTRL_FDO FdoExt);
BOOLEAN  I2cCtrl_IsRxFifoNotEmpty(PI2CCTRL_FDO FdoExt);
VOID I2cCtrl_DiscardTxEntrySafe(PI2CCTRL_FDO FdoExt);
NTSTATUS I2cCtrl_ReadRxByteSafe(PI2CCTRL_FDO FdoExt, PUCHAR ByteOut);

VOID
I2cCtrl_QuiesceFifos(
    PI2CCTRL_FDO FdoExt
    );

NTSTATUS
I2cCtrl_WaitForEnableState(
    PI2CCTRL_FDO fdoExt,
    BOOLEAN      targetOn,
    ULONG        timeout
    );

NTSTATUS
I2cCtrl_DetectTouchpadRedirect(
    struct _I2CCTRL_FDO *fdoExt,
    struct _I2CCTRL_DETECT_RESULT *result
    );

//
// Lookup table entry
//
typedef struct _I2CCTRL_DEVICE_ID {
    PCWSTR PciId;              /* Hardware ID string to match */

    /* DW-I2C functional register offsets (BAR0) */
    ULONG  ControlOffset;      /* IC_CON or equivalent */
    ULONG  StatusOffset;       /* IC_STATUS */
    ULONG  DataOffset;         /* IC_DATA_CMD */
    ULONG  ClockOffset;        /* Timing registers */

    /* LPSS power-on register offsets (BAR2) */
    ULONG  LpssClkGateOffset;  /* Clock gate control */
    ULONG  LpssResetOffset;    /* Reset control */
    ULONG  LpssFuncClkOffset;  /* Functional clock enable */
    ULONG  LpssMiscOffset;     /* Misc / status */

    ULONG  Quirks;             /* Functional quirks bitmask */
    ULONG  BsodQuirks;         /* BSOD-tweak-workarounds bitmask */
} I2CCTRL_DEVICE_ID, *PI2CCTRL_DEVICE_ID;


/* Extern declarations */
extern const I2CCTRL_DEVICE_ID g_I2cControllers[];
extern const ULONG g_I2cControllersCount;

//
// HID-over-I2C device quirks
//
typedef enum _I2CHID_QUIRKS {
    HID_QUIRK_NONE        = 0x00000000,
    HID_QUIRK_ELAN        = 0x00000001,
    HID_QUIRK_SYNAPTICS   = 0x00000002,
    HID_QUIRK_ASUS        = 0x00000004,
    HID_QUIRK_GOODIX      = 0x00000008,
    HID_QUIRK_RAYDIUM     = 0x00000010,
    HID_QUIRK_FOCALTECH   = 0x00000020,
    HID_QUIRK_CYPRESS     = 0x00000040,
    HID_QUIRK_HIMAX       = 0x00000080,
    HID_QUIRK_PIXART      = 0x00000100,
    HID_QUIRK_SILEAD      = 0x00000200,
    HID_QUIRK_ATMEL       = 0x00000400,
    HID_QUIRK_PRIMAX      = 0x00000800,
    HID_QUIRK_CHICONY     = 0x00001000
} I2CHID_QUIRKS;

//
// HID-over-I2C device flags
//
typedef enum _I2CHID_FLAGS {
    HID_FLAG_GENERIC      = 0x00000001,
    HID_FLAG_TOUCHPAD     = 0x00000002,
    HID_FLAG_TOUCHSCREEN  = 0x00000004
} I2CHID_FLAGS;

//
// HID-over-I2C device ID entry
//
typedef struct _I2CHID_DEVICE_ID {
    PCWSTR HidId;      /* ACPI HID string */
    ULONG  Quirks;     /* HID_QUIRK_* bitmask */
    ULONG  Flags;      /* HID_FLAG_* bitmask */
} I2CHID_DEVICE_ID, *PI2CHID_DEVICE_ID;

//
// Extern declarations for HID-over-I2C device table
//
extern const I2CHID_DEVICE_ID g_I2cHidDevices[];
extern const ULONG g_I2cHidDevicesCount;

//
// Lookup function
//
const I2CHID_DEVICE_ID*
I2cCtrl_FindHidMatch(
    PCWSTR HidId
    );

//
// Register map structure
//
typedef struct _I2CCTRL_REGMAP {
    ULONG ControlReg;   /* Register offset for control */
    ULONG StatusReg;    /* Register offset for status */
    ULONG DataReg;      /* Register offset for data */
    ULONG ClockReg;     /* Register offset for clock */
    ULONG Quirks;       /* Functional quirks bitmask */
    ULONG BsodQuirks;   /* BSOD-tweak-workarounds bitmask */
} I2CCTRL_REGMAP, *PI2CCTRL_REGMAP;


I2CCTRL_REGMAP g_CurrentRegMap;

EXTERN_C const GUID GUID_I2CCTRL_CHILD_IFACE;

EXTERN_C const GUID GUID_BUS_TYPE_I2C;

EXTERN_C const GUID GUID_NULL;

//
// Abstract helpers for I²C controller power/timing
//

/* Enable or disable the controller core */
NTSTATUS
I2cCtrl_EnableController(
    PI2CCTRL_FDO devctx,
    BOOLEAN      enable
    );

/* Mask or unmask controller interrupts */
VOID
I2cCtrl_MaskInterrupts(
    PI2CCTRL_FDO devctx,
    BOOLEAN      mask
    );

/* Apply abstract bus timing (convert ns + speed into register values) */
VOID
I2cCtrl_ApplyBusTiming(
    PI2CCTRL_FDO devctx,
    ULONG        highNs,
    ULONG        lowNs,
    ULONG        busSpeedHz
    );

/* Query current high period timing (ns) */
ULONG
I2cCtrl_QueryTimingHigh(
    PI2CCTRL_FDO devctx
    );

/* Query current low period timing (ns) */
ULONG
I2cCtrl_QueryTimingLow(
    PI2CCTRL_FDO devctx
    );

/* Enum for snapshot fields */
typedef enum _I2CCTRL_SNAPSHOT_FIELD {
    I2cSnapshot_BusAddress = 0,
    I2cSnapshot_BusSpeedHz,
    I2cSnapshot_TimingHighNs,
    I2cSnapshot_TimingLowNs,
    I2cSnapshot_PowerState,
    I2cSnapshot_SpeedMode
} I2CCTRL_SNAPSHOT_FIELD;

VOID I2cCtrl_AckInterrupt(PI2CCTRL_FDO devctx, ULONG mask);

/* Update elapsed time in milliseconds since 'start' */
#define UPDATE_ELAPSED(start,freq,elapsedMs,now) \
    do { \
        (now) = KeQueryPerformanceCounter(NULL); \
        (elapsedMs) = (ULONG)(((now).QuadPart - (start).QuadPart) * 1000 / (freq).QuadPart); \
    } while (0)

/* Check timeout and break out if exceeded */
#define CHECK_TIMEOUT_BREAK(timeoutMs,start,freq,elapsedMs,now,status) \
    do { \
        UPDATE_ELAPSED(start,freq,elapsedMs,now); \
        if ((elapsedMs) > (timeoutMs)) { \
            (status) = STATUS_IO_TIMEOUT; \
            goto Exit; \
        } \
    } while (0)

/* One finger contact */
typedef struct _PT_RAW_CONTACT {
    USHORT X;
    USHORT Y;
    UCHAR  Buttons;    /* bitfield: tip switch, etc. */
    UCHAR  ContactId;  /* optional: finger ID */
	UCHAR  Flags;
} PT_RAW_CONTACT, *PPT_RAW_CONTACT;

/* A full raw sample: up to 3 contacts */
typedef struct _PT_RAW_SAMPLE {
    UCHAR          ContactCount;        /* number of valid contacts, 0..3 */
    PT_RAW_CONTACT Contacts[3];         /* up to 3 contacts maximum */
	ULONGLONG Timestamp;
} PT_RAW_SAMPLE, *PPT_RAW_SAMPLE;

NTSTATUS
I2Cctrl_Transfer(PDEVICE_OBJECT DevObj,
                 HANDLE ControllerHandle,
                 USHORT SlaveAddress,
                 ULONG  Direction,
                 PUCHAR Buffer,
                 ULONG  Length,
                 PULONG BytesTransferred);

NTSTATUS I2CctrlHw_EnableController(PDEVICE_OBJECT DevObj, PSMBUS_REQUEST Request, PI2CCTRL_QUEUE Queue);

/* Optional IOCTL to set request priority (XP‑safe structure) */
typedef struct _I2CCTRL_SET_PRIORITY {
    I2CCTRL_QOS_PRIORITY Priority; /* 0=High, 1=Normal, 2=Low */
} I2CCTRL_SET_PRIORITY, *PI2CCTRL_SET_PRIORITY;

VOID
I2cCtrl_RebindWorkerRoutine(
    PDEVICE_OBJECT DeviceObject,
    PVOID Context
    );

VOID
I2cCtrl_SaveFifoState(
    PI2CCTRL_FDO devctx
    );

VOID
I2cCtrl_RestoreFifoState(
    PI2CCTRL_FDO devctx
    );

VOID
I2cCtrl_SaveQueueState(
    PI2CCTRL_FDO devctx
    );

VOID
I2cCtrl_RestoreQueueState(
    PI2CCTRL_FDO devctx
    );

VOID
I2cCtrl_SaveArbCounters(
    PI2CCTRL_FDO devctx
    );

VOID
I2cCtrl_RestoreArbCounters(
    PI2CCTRL_FDO devctx
    );

VOID
I2cCtrl_ArmWake(
    PI2CCTRL_FDO devctx
    );

VOID
I2cCtrl_DisarmWake(
    PI2CCTRL_FDO devctx
    );

VOID
I2cHal_EnableWakeSource(
    PI2CCTRL_FDO devctx,
    BOOLEAN      enable
    );

VOID
I2cCtrl_PerformReset(
    PI2CCTRL_FDO devctx
    );

VOID
I2cCtrl_HidCancelRead(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    );

NTSTATUS
I2cCtrl_StartCompletion(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
    );

/* File-scope prototype (optional in C89 if function appears earlier) */
NTSTATUS
I2CCTRL_ReleaseLockCompletion(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
    );

NTSTATUS I2cCtrl_PdoDispatchPower(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2cCtrl_FdoDispatchPower(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* ============================================================================
   Power helpers
   ============================================================================ */
NTSTATUS I2cCtrl_SetDevicePowerD0(PI2CCTRL_FDO devctx);
VOID     I2cCtrl_SetDevicePowerD1(PI2CCTRL_FDO devctx);
VOID     I2cCtrl_SetDevicePowerD2(PI2CCTRL_FDO devctx);
VOID     I2cCtrl_SetDevicePowerD3(PI2CCTRL_FDO devctx);


NTSTATUS
I2cCtrl_SystemPowerCompletion(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp,
    PVOID          Context
    );

VOID
I2cCtrl_QuiesceHardware(
    PI2CCTRL_FDO devctx
    );

VOID
I2cCtrl_CancelWakeIrp(
    PDEVICE_OBJECT DeviceObject,
    PIRP           Irp
    );

VOID
I2cCtrl_EnableWakeSignal(
    PI2CCTRL_FDO devctx
    );

VOID
I2cHidApplyQuirks(
    PI2CCTRL_PDO childDx,
    const I2CHID_DEVICE_ID* hidMatch
    );

NTSTATUS
I2cCtrlInitHidChildIds(
    PI2CCTRL_PDO          childDx,
    ULONG                 childId,
    const I2CHID_DEVICE_ID* hidMatch
    );

NTSTATUS
I2cCtrl_RemoveDevice(
    PI2CCTRL_FDO fdoExt,
    PIRP         Irp   /* optional, not completed here */
	);

VOID
I2cCtrl_Log(
    PCSTR Format,
    ...
    );
	
const I2CCTRL_DEVICE_ID*
I2cCtrl_FindControllerId(
    PCWSTR PnpId
    );

NTSTATUS
I2cCtrl_Lpss2PowerOn(
    PI2CCTRL_FDO devctx
    );

NTSTATUS
I2cCtrl_Lpss2PowerOff(
    PI2CCTRL_FDO devctx
    );

BOOLEAN
I2cCtrl_ParseCrsForI2cSerialBus(
    const UCHAR *buf,
    ULONG        len,
    PUCHAR       addrOut,
    PULONG       speedOut,
    PBOOLEAN     tenBitOut
    );

#define HID_I2C_DSM_REVISION 1
#define HID_I2C_DSM_GET_DESCRIPTOR 1
#define HID_I2C_DSM_GET_REPORT     2

typedef struct _HID_I2C_DSM_GUID {
    UCHAR Bytes[16];
} HID_I2C_DSM_GUID;

#define HID_I2C_DSM_GUID_PTR ((PUCHAR)g_HidI2cDsmGuid)

/* HID-over-I2C _DSM GUID: 3CDFF6F7-4267-4555-AD05-B30A3D8938DE */
static const UCHAR g_HidI2cDsmGuid[16] = {
    0xF7,0xF6,0xDF,0x3C, 0x67,0x42, 0x55,0x45,
    0xAD,0x05, 0xB3,0x0A,0x3D,0x89,0x38,0xDE
};

NTSTATUS
I2cCtrl_AcpiGetHidDescriptorViaDsm(
    PI2CCTRL_FDO            devctx,
    PVOID                   handle,
    PUCHAR                  outBuf,
    PULONG                  outLen
    );

VOID     I2cCtrlApplyQuirks(PI2CCTRL_FDO devctx);

#ifndef IOCTL_ACPI_GET_DEVICE_INFORMATION
#define IOCTL_ACPI_GET_DEVICE_INFORMATION \
    CTL_CODE(FILE_DEVICE_ACPI, 0x0003, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#define ACPI_DEVICE_INFORMATION_SIGNATURE 'IPCA'

//
// Modern ACPI 2.0+ input buffer for complex method evaluation
// (used by patched ACPI.sys on modern hardware)
//
typedef struct _I2CCTRL_ACPI_EVAL_INPUT_BUFFER_COMPLEX {
    ULONG  Signature;
    CHAR   MethodName[4];
    ULONG  Size;
    ULONG  ArgumentCount;
    ULONG  Data[ANYSIZE_ARRAY];   // GUID + integers, packed
} I2CCTRL_ACPI_EVAL_INPUT_BUFFER_COMPLEX,
  *PI2CCTRL_ACPI_EVAL_INPUT_BUFFER_COMPLEX;


//
// Modern ACPI 2.0+ output buffer for method evaluation
//
typedef struct _I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER {
    ULONG  Signature;
    ULONG  Length;
    ULONG  Count;                 // number of ACPI_METHOD_ARGUMENTs
    UCHAR  Data[ANYSIZE_ARRAY];   // packed arguments
} I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER,
  *PI2CCTRL_ACPI_EVAL_OUTPUT_BUFFER;


//
// Modern ACPI 2.0+ method argument layout
//
typedef struct _I2CCTRL_ACPI_METHOD_ARGUMENT {
    USHORT Type;
    USHORT DataLength;
    ULONG  Argument;              // inline or offset
} I2CCTRL_ACPI_METHOD_ARGUMENT,
  *PI2CCTRL_ACPI_METHOD_ARGUMENT;

NTSTATUS
I2cCtrl_AcpiEvalMethod(
    PDEVICE_OBJECT            AcpiPdo,
    PVOID                     AcpiHandle,
    PCSTR                     MethodName,
    PI2CCTRL_ACPI_EVAL_OUTPUT_BUFFER  OutBuf,
    ULONG                     OutBufLen
    );

NTSTATUS
I2cCtrl_EnablePmcPowerWellWhiskeyLake(
    _In_ PHYSICAL_ADDRESS PwrmBase
    );

NTSTATUS
I2cCtrl_AcpiEvalInteger(
    PDEVICE_OBJECT AcpiDevice,
    PCSTR MethodName,
    PULONG64 OutValue
);

#endif /* _I2CCTRL_EXT_H_ */