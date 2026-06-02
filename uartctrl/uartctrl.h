/* -----------------------------------------------------------------------
   uartctrl.h – prototypes for UART controller driver
   ----------------------------------------------------------------------- */

#pragma once

#include <ntddk.h>
#include "uartctrl_ext.h"
#include "uartctrl_ioctl.h"
#include "uartctrl_hw.h"

/* Driver entry points */
DRIVER_UNLOAD  UARTCTRL_DriverUnload;
DRIVER_ADD_DEVICE UARTCTRL_AddDevice;
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);

/* Dispatch routines */
NTSTATUS UARTCTRL_DispatchPnP(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS UARTCTRL_DispatchPower(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS UARTCTRL_DispatchIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS UARTCTRL_PT_DispatchPass(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* Lifecycle helpers */
NTSTATUS UARTCTRL_StartDevice(
    PUARTCTRL_FDO Ext,
    PCM_RESOURCE_LIST RawResources,
    PCM_RESOURCE_LIST TranslatedResources
    );

NTSTATUS UARTCTRL_StopDevice(PUARTCTRL_FDO Ext);
NTSTATUS UARTCTRL_RemoveDevice(PDEVICE_OBJECT DeviceObject);

/* Interrupt/DPC */
BOOLEAN UARTCTRL_InterruptServiceRoutine(
    PKINTERRUPT Interrupt,
    PVOID ServiceContext
    );

VOID UARTCTRL_DpcRoutine(
    PKDPC Dpc,
    PVOID DeferredContext,
    PVOID SystemArg1,
    PVOID SystemArg2
    );

/* Ring buffer helpers */
BOOLEAN RingPut(
    PUCHAR buf,
    ULONG size,
    volatile ULONG* head,
    volatile ULONG* tail,
    UCHAR v
    );

BOOLEAN RingGet(
    PUCHAR buf,
    ULONG size,
    volatile ULONG* head,
    volatile ULONG* tail,
    UCHAR* v
    );

ULONG RingAvail(
    PUCHAR buf,
    ULONG size,
    volatile ULONG head,
    volatile ULONG tail
    );

ULONG RingFree(
    PUCHAR buf,
    ULONG size,
    volatile ULONG head,
    volatile ULONG tail
    );

VOID RingReset(volatile ULONG* head, volatile ULONG* tail);

/* -----------------------------------------------------------------------
   Dispatch prototypes
   ----------------------------------------------------------------------- */
NTSTATUS UARTCTRL_DispatchCreateClose(PDEVICE_OBJECT DevObj, PIRP Irp);
NTSTATUS UARTCTRL_DispatchRead(PDEVICE_OBJECT DevObj, PIRP Irp);
NTSTATUS UARTCTRL_DispatchWrite(PDEVICE_OBJECT DevObj, PIRP Irp);
VOID UARTCTRL_ReadCancelRoutine(PDEVICE_OBJECT DevObj, PIRP Irp);