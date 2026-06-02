/* -----------------------------------------------------------------------
   uartctrl_ext.h – device extension for UART controller driver
   ----------------------------------------------------------------------- */

#pragma once
#include <ntddk.h>
#include <ntstrsafe.h>          /* RtlStringCbVPrintfA */
#include "uartctrl_ioctl.h"

/* -----------------------------------------------------------------------
   UARTCTRL_FDO – device extension structure for UART controller (FDO)
   ----------------------------------------------------------------------- */
typedef struct _UARTCTRL_FDO {

    //
    // PnP / power state
    //
    PDEVICE_OBJECT LowerDevice;     // attached lower device
    IO_REMOVE_LOCK RemoveLock;      // synchronizes remove
    UNICODE_STRING Symlink;         // optional symbolic link
    BOOLEAN Started;                // device started
    BOOLEAN Removed;                // device removed

    //
    // Resources
    //
    PVOID  MmioBase;                // mapped register base
    ULONG  MmioLength;              // length of MMIO region
    PKINTERRUPT InterruptObject;    // interrupt object
    BOOLEAN InterruptConnected;     // interrupt connected flag

    //
    // UART configuration and state
    //
    UARTCTRL_CONFIG Config;         // current line config
    ULONG ClockHz;                  // input clock frequency
    volatile ULONG RxErrors;        // RX error counter
    volatile ULONG TxErrors;        // TX error counter

    //
    // Ring buffers
    //
    PUCHAR RxBuf;                   // RX buffer
    ULONG  RxSize;
    volatile ULONG RxHead;
    volatile ULONG RxTail;

    PUCHAR TxBuf;                   // TX buffer
    ULONG  TxSize;
    volatile ULONG TxHead;
    volatile ULONG TxTail;

    KSPIN_LOCK RxLock;              // protects RX buffer
    KSPIN_LOCK TxLock;              // protects TX buffer

    //
    // Pending read IRPs
    //
    LIST_ENTRY ReadQueue;           // queue of pending read IRPs
    KSPIN_LOCK ReadQueueLock;       // protects ReadQueue

    //
    // Optional polling fallback
    //
    KTIMER PollTimer;               // timer for polling fallback
    KDPC   PollDpc;                 // DPC for polling fallback
    BOOLEAN Polling;                // polling enabled flag

    //
    // Additional bookkeeping
    //
    LONG OpenCount;                 // number of active handles
    IO_CSQ ReadCsq;                 // cancel‑safe queue for read IRPs
    KSPIN_LOCK CsqLock;             // protects cancel‑safe queue
    LIST_ENTRY PendingIrpList;      // list of pending IRPs
    KSPIN_LOCK PendingIrpLock;      // protects PendingIrpList
    DEVICE_POWER_STATE PowerState;  // current device power state
    SYSTEM_POWER_STATE SysPower;    // current system power state
	LIST_ENTRY ListEntry;


} UARTCTRL_FDO, *PUARTCTRL_FDO;


/* -----------------------------------------------------------------------
   UARTCTRL_PDO – device extension structure for UART PDO
   ----------------------------------------------------------------------- */
typedef struct _UARTCTRL_PDO {

    //
    // Identity
    //
    PDEVICE_OBJECT Self;            // this PDO
    PDEVICE_OBJECT ParentFdo;       // parent FDO

    //
    // Hardware IDs / instance IDs
    //
    UNICODE_STRING HardwareId;      // e.g. "ACME\\UART1234"
    UNICODE_STRING CompatibleId;    // optional
    UNICODE_STRING InstanceId;      // unique per device

    //
    // PnP state
    //
    BOOLEAN Present;                // TRUE if device is present
    BOOLEAN ReportedMissing;        // TRUE after IRP_MN_REMOVE_DEVICE
    BOOLEAN Started;                // TRUE after IRP_MN_START_DEVICE
    BOOLEAN Removed;                // TRUE after IRP_MN_REMOVE_DEVICE

    DEVICE_POWER_STATE PowerState;  // current device power state
    SYSTEM_POWER_STATE SysPower;    // current system power state

    //
    // Remove lock for PDO
    //
    IO_REMOVE_LOCK RemoveLock;

    //
    // Optional: PDO capabilities
    //
    ULONG SerialNumber;             // unique per UART instance
    ULONG Flags;                    // custom flags for bus/FDO use

} UARTCTRL_PDO, *PUARTCTRL_PDO;


/* -----------------------------------------------------------------------
 * Driver-wide globals used for multi-UART bookkeeping
 * XP/2003 BSOD-safe, C89-compliant
 * ----------------------------------------------------------------------- */
typedef struct _UARTCTRL_GLOBAL {
    LONG       NextControllerId;   /* monotonically increasing controller ID */
    KSPIN_LOCK GlobalLock;         /* protects global controller list */
    LIST_ENTRY ControllerList;     /* list of all UART FDOs */

    /* Lifecycle helpers (correct signatures) */
    NTSTATUS (*StartDevice)(
        struct _UARTCTRL_FDO *fdoExt,
        PCM_RESOURCE_LIST raw,
        PCM_RESOURCE_LIST translated
    );

    NTSTATUS (*StopDevice)(
        struct _UARTCTRL_FDO *fdoExt
    );

    /* UART-specific state */
    BOOLEAN   AnyUartPresent;      /* TRUE if at least one UART started */
} UARTCTRL_GLOBAL, *PUARTCTRL_GLOBAL;

/* Single instance of driver-wide globals */
extern UARTCTRL_GLOBAL g_UartCtrlGlobal;


/* -----------------------------------------------------------------------
   Extension helper prototypes
   ----------------------------------------------------------------------- */
NTSTATUS UARTCTRL_ExtResetHardware(PUARTCTRL_FDO ext);
NTSTATUS UARTCTRL_ExtAllocateBuffers(PUARTCTRL_FDO ext, ULONG rxSize, ULONG txSize);
VOID     UARTCTRL_ExtFreeBuffers(PUARTCTRL_FDO ext);

/* Forward declaration of DPC routine */
VOID
UARTCTRL_DpcRoutine(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArg1,
    PVOID SystemArg2
    );

VOID
UartCtrl_Log(
    PCSTR Format,
    ...
    );

VOID
UartCtrl_LogIsr(
    PCSTR Format,
    ...
    );