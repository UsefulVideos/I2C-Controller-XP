/* -----------------------------------------------------------------------
   uartctrl_ext.h – device extension for UART controller driver
   ----------------------------------------------------------------------- */

#pragma once
#include <ntddk.h>
#include "uartctrl_ioctl.h"

/* -----------------------------------------------------------------------
   UARTCTRL_DEVEXT – device extension structure for UART controller
   ----------------------------------------------------------------------- */
typedef struct _UARTCTRL_DEVEXT {
    //
    // PnP / power state
    //
    PDEVICE_OBJECT LowerDevice;   // attached lower device
    IO_REMOVE_LOCK RemoveLock;    // synchronizes remove
    UNICODE_STRING Symlink;       // optional symbolic link
    BOOLEAN Started;              // device started
    BOOLEAN Removed;              // device removed

    //
    // Resources
    //
    PVOID  MmioBase;              // mapped register base
    ULONG  MmioLength;            // length of MMIO region
    PKINTERRUPT InterruptObject;  // interrupt object
    BOOLEAN InterruptConnected;   // interrupt connected flag

    //
    // UART configuration and state
    //
    UARTCTRL_CONFIG Config;       // current line config
    ULONG ClockHz;                // input clock frequency for baud calculations
    volatile ULONG RxErrors;      // RX error counter
    volatile ULONG TxErrors;      // TX error counter

    //
    // Ring buffers
    //
    PUCHAR RxBuf;                 // RX buffer
    ULONG  RxSize;
    volatile ULONG RxHead;
    volatile ULONG RxTail;

    PUCHAR TxBuf;                 // TX buffer
    ULONG  TxSize;
    volatile ULONG TxHead;
    volatile ULONG TxTail;

    KSPIN_LOCK RxLock;            // protects RX buffer
    KSPIN_LOCK TxLock;            // protects TX buffer

    //
    // Pending read IRPs
    //
    LIST_ENTRY ReadQueue;         // queue of pending read IRPs
    KSPIN_LOCK ReadQueueLock;     // protects ReadQueue

    //
    // Optional polling fallback
    //
    KTIMER PollTimer;             // timer for polling fallback
    KDPC   PollDpc;               // DPC for polling fallback
    BOOLEAN Polling;              // polling enabled flag

    //
    // Additional bookkeeping
    //
    LONG OpenCount;               // number of active handles
    IO_CSQ ReadCsq;               // cancel‑safe queue for read IRPs
    KSPIN_LOCK CsqLock;           // protects cancel‑safe queue
    LIST_ENTRY PendingIrpList;    // list of pending IRPs (read/write/ioctl)
    KSPIN_LOCK PendingIrpLock;    // protects PendingIrpList
    DEVICE_POWER_STATE PowerState;// current device power state
    SYSTEM_POWER_STATE SysPower;  // current system power state

} UARTCTRL_DEVEXT, *PUARTCTRL_DEVEXT;


/* -----------------------------------------------------------------------
   Extension helper prototypes
   ----------------------------------------------------------------------- */
NTSTATUS UARTCTRL_ExtResetHardware(PUARTCTRL_DEVEXT ext);
NTSTATUS UARTCTRL_ExtAllocateBuffers(PUARTCTRL_DEVEXT ext, ULONG rxSize, ULONG txSize);
VOID     UARTCTRL_ExtFreeBuffers(PUARTCTRL_DEVEXT ext);

/* Forward declaration of DPC routine */
VOID
UARTCTRL_DpcRoutine(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArg1,
    PVOID SystemArg2
    );
