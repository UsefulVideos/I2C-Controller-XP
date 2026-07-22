#ifndef _I2CCTRL_ISR_H_
#define _I2CCTRL_ISR_H_

#include <ntddk.h>
#include "i2cctrl_ext.h"

/* ---------------------------------------------------------------------------
   ISR: top-half - acknowledges interrupts and queues DPC
   --------------------------------------------------------------------------- */
BOOLEAN I2cCtrl_Isr(PKINTERRUPT Interrupt, PVOID ServiceContext);


/* ---------------------------------------------------------------------------
   DPC: bottom-half - progresses transfer, handles arbitration, completes IRP
   --------------------------------------------------------------------------- */
VOID I2cCtrl_DpcRoutine(PKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1, PVOID SystemArgument2);


/* ---------------------------------------------------------------------------
   Optional: polled-mode worker if no IRQ resource was provided
   --------------------------------------------------------------------------- */
VOID
I2cCtrl_PollWorker(
    PI2CCTRL_FDO Dx
    );

/* ---------------------------------------------------------------------------
   Decode TX_ABRT_SOURCE into meaningful NTSTATUS
   --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_MapAbortSource(
    ULONG abrtSrc
    );

/* ---------------------------------------------------------------------------
   Timeout DPC - completes IRP if transfer stalls
   --------------------------------------------------------------------------- */
VOID
I2cCtrl_TimeoutDpcRoutine(
    IN PKDPC Dpc,
    IN PVOID DeferredContext,
    IN PVOID SystemArg1,
    IN PVOID SystemArg2
    );

/* ---------------------------------------------------------------------------
   Multi-master arbitration helpers
   --------------------------------------------------------------------------- */

/* Detect arbitration lost (TX_ABRT/STOP_DET) */
BOOLEAN
I2cCtrl_IsArbitrationLost(
    PI2CCTRL_FDO Dx,
    ULONG IntrSnapshot
    );

/* Handle arbitration loss: stats, clear sticky bits, optional reset */
VOID
I2cCtrl_HandleArbitrationLost(
    PI2CCTRL_FDO Dx,
    PI2C_TRANSFER_CONTEXT Xc
    );

VOID
I2cCtrl_IsrDpc(
    PKDPC  Dpc,
    PVOID  DeferredContext,
    PVOID  SystemArg1,
    PVOID  SystemArg2
    );

#endif /* _I2CCTRL_ISR_H_ */
