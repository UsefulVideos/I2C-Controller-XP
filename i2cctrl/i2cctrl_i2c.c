/* i2cctrl_i2c.c */
#include "i2cctrl_i2c.h"
#include "I2cCtrl_Isr.h"
#include "i2cctrl_bsod.h"

/* Register accessors (BSOD-safe, teardown-safe) */
__forceinline ULONG RReg(PI2CCTRL_FDO fdoExt, ULONG Off)
{
    if (fdoExt == NULL) {
        I2cCtrl_Log("RReg NULL fdoExt\n");
        return 0;
    }

    return I2cCtrl_ReadRegisterSafe(fdoExt, Off);
}

__forceinline VOID WReg(PI2CCTRL_FDO fdoExt, ULONG Off, ULONG Val)
{
    if (fdoExt == NULL) {
        I2cCtrl_Log("WReg NULL fdoExt\n");
        return;
    }

    I2cCtrl_WriteRegisterSafe(fdoExt, Off, Val);
}


/* -----------------------------------------------------------------------
 * WaitStatusBits - bounded wait on HAL status flags
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_WaitStatusBits(
    PI2CCTRL_FDO fdoExt,
    ULONG        Mask,
    BOOLEAN      Set,
    ULONG        SpinMax
    )
{
    ULONG          spins;
    NTSTATUS       st;
    I2C_HW_STATUS  hwst;

    if (fdoExt == NULL || fdoExt->Ops == NULL || fdoExt->Ops->GetStatus == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(&hwst, sizeof(hwst));

    for (spins = 0U; spins < SpinMax; ++spins) {
        st = fdoExt->Ops->GetStatus(fdoExt, &hwst);
        if (!NT_SUCCESS(st)) {
            break; /* hardware read failed */
        }

        if (Set) {
            if ((hwst.StatusReg & Mask) == Mask) {
                return STATUS_SUCCESS;
            }
        } else {
            if ((hwst.StatusReg & Mask) == 0U) {
                return STATUS_SUCCESS;
            }
        }

        KeStallExecutionProcessor(I2C_STALL_US);
    }

    return STATUS_IO_TIMEOUT;
}

/* ---------------------------------------------------------------------------
 * I2cCtrl_PowerOnTouchpad - ACPI power-on helper (_PS0 / _ON)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_PowerOnTouchpad(PDEVICE_OBJECT Pdo)
{
    NTSTATUS  status;
    PI2CCTRL_PDO  childDx;
    PI2CCTRL_FDO  parentFdo;
    PI2CCTRL_ACPI_EVAL_OUTPUT_BUFFER outBuf;
    ULONG    outLen;

    /* C89 init */
    status    = STATUS_UNSUCCESSFUL;
    childDx   = NULL;
    parentFdo = NULL;
    outBuf    = NULL;
    outLen    = sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER) + 256U;

    if (Pdo == NULL) {
        I2cCtrl_Log("TouchpadPowerOn: invalid Pdo\n");
        return STATUS_INVALID_PARAMETER;
    }

    I2CCTRL_REQUIRE_PASSIVE();

    childDx = (PI2CCTRL_PDO)Pdo->DeviceExtension;
    if (childDx == NULL) {
        I2cCtrl_Log("TouchpadPowerOn: missing childDx\n");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    parentFdo = childDx->ParentFdo;
    if (parentFdo == NULL) {
        I2cCtrl_Log("TouchpadPowerOn: missing parentFdo\n");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (childDx->AcpiHandle == NULL || parentFdo->AcpiDeviceObject == NULL) {
        I2cCtrl_Log("TouchpadPowerOn: no ACPI handle available\n");
        return STATUS_NOT_SUPPORTED;
    }

    /* Ensure ACPI interface is open on parent */
    status = I2cCtrl_AcpiOpen(parentFdo);
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("TouchpadPowerOn: AcpiOpen failed (0x%08lx)\n", status);
        return STATUS_NOT_SUPPORTED;
    }

    I2cCtrl_Log("TouchpadPowerOn: trying _PS0\n");

    /* ---- Try _PS0 on CHILD handle via controller ACPI PDO ---- */
    status = I2cCtrl_AcpiEvalMethod(
                 parentFdo->AcpiDeviceObject,
                 childDx->AcpiHandle,
                 "_PS0",
                 outBuf,
                 outLen
             );

    if (!NT_SUCCESS(status)) {

        I2cCtrl_Log("TouchpadPowerOn: _PS0 failed (0x%08lx), trying _ON\n", status);

        /* Free buffer from _PS0 attempt, if any */
        if (outBuf != NULL) {
            ExFreePoolWithTag(outBuf, 'Acpi');
            outBuf = NULL;
        }

        /* ---- Fallback: try _ON ---- */
        status = I2cCtrl_AcpiEvalMethod(
                     parentFdo->AcpiDeviceObject,
                     childDx->AcpiHandle,
                     "_ON",
                     outBuf,
                     outLen
                 );
    }

    /* Free ACPI output buffer */
    if (outBuf != NULL) {
        ExFreePoolWithTag(outBuf, 'Acpi');
        outBuf = NULL;
    }

    I2cCtrl_Log("TouchpadPowerOn: final status = 0x%08lx\n", status);

    return NT_SUCCESS(status) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}


/* ---------------------------------------------------------------------------
 * I2cCtrl_PowerOffTouchpad - ACPI power-off helper (_PS3 / _OFF)
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cCtrl_PowerOffTouchpad(PDEVICE_OBJECT Pdo)
{
    NTSTATUS  status;
    PI2CCTRL_PDO  childDx;
    PI2CCTRL_FDO  parentFdo;
    PI2CCTRL_ACPI_EVAL_OUTPUT_BUFFER outBuf;
    ULONG    outLen;

    /* C89 init */
    status    = STATUS_UNSUCCESSFUL;
    childDx   = NULL;
    parentFdo = NULL;
    outBuf    = NULL;
    outLen    = sizeof(I2CCTRL_ACPI_EVAL_OUTPUT_BUFFER) + 256U;

    if (Pdo == NULL) {
        I2cCtrl_Log("TouchpadPowerOff: invalid Pdo\n");
        return STATUS_INVALID_PARAMETER;
    }

    I2CCTRL_REQUIRE_PASSIVE();

    childDx = (PI2CCTRL_PDO)Pdo->DeviceExtension;
    if (childDx == NULL) {
        I2cCtrl_Log("TouchpadPowerOff: missing childDx\n");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    parentFdo = childDx->ParentFdo;
    if (parentFdo == NULL) {
        I2cCtrl_Log("TouchpadPowerOff: missing parentFdo\n");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (childDx->AcpiHandle == NULL || parentFdo->AcpiDeviceObject == NULL) {
        I2cCtrl_Log("TouchpadPowerOff: no ACPI handle available\n");
        return STATUS_NOT_SUPPORTED;
    }

    /* Ensure ACPI interface is open on parent */
    status = I2cCtrl_AcpiOpen(parentFdo);
    if (!NT_SUCCESS(status)) {
        I2cCtrl_Log("TouchpadPowerOff: AcpiOpen failed (0x%08lx)\n", status);
        return STATUS_NOT_SUPPORTED;
    }

    I2cCtrl_Log("TouchpadPowerOff: trying _PS3\n");

    /* ---- Try _PS3 on CHILD handle via controller ACPI PDO ---- */
    status = I2cCtrl_AcpiEvalMethod(
                 parentFdo->AcpiDeviceObject,
                 childDx->AcpiHandle,
                 "_PS3",
                 outBuf,
                 outLen
             );

    if (!NT_SUCCESS(status)) {

        I2cCtrl_Log("TouchpadPowerOff: _PS3 failed (0x%08lx), trying _OFF\n", status);

        if (outBuf != NULL) {
            ExFreePoolWithTag(outBuf, 'Acpi');
            outBuf = NULL;
        }

        /* ---- Fallback: try _OFF ---- */
        status = I2cCtrl_AcpiEvalMethod(
                     parentFdo->AcpiDeviceObject,
                     childDx->AcpiHandle,
                     "_OFF",
                     outBuf,
                     outLen
                 );
    }

    if (outBuf != NULL) {
        ExFreePoolWithTag(outBuf, 'Acpi');
        outBuf = NULL;
    }

    I2cCtrl_Log("TouchpadPowerOff: final status = 0x%08lx\n", status);

    return NT_SUCCESS(status) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}


/* Optional timing calculator */
VOID I2cHwComputeTimings(ULONG FclkHz, I2C_SPEED_MODE Speed, ULONG* Hcnt, ULONG* Lcnt)
{
    ULONG period_ns;
    ULONG tHigh_ns, tLow_ns;

    if (Hcnt == NULL || Lcnt == NULL || FclkHz == 0U) return;

    period_ns = 1000000000UL / FclkHz;

    if (Speed == I2cSpeedStandard100k) {
        tHigh_ns = 4000;  /* 4.0 µs */
        tLow_ns  = 4700;  /* 4.7 µs */
    } else {
        tHigh_ns = 1000;  /* 1.0 µs */
        tLow_ns  = 1500;  /* 1.5 µs */
    }

    *Hcnt = (tHigh_ns + period_ns - 1U) / period_ns;
    *Lcnt = (tLow_ns  + period_ns - 1U) / period_ns;

    if (Speed == I2cSpeedStandard100k) {
        *Hcnt += 16U;
        *Lcnt += 24U;
    } else {
        *Hcnt += 8U;
        *Lcnt += 12U;
    }
}

/* ---------------------------------------------------------------------------
   I2cHwEnable - XP/2003 BSOD-safe, C89-compliant (HAL-universal)
   Purpose:
     - Enable or disable the I²C controller safely
     - Use HAL ops only (no direct register macros)
     - Wait for bus idle when disabling
   --------------------------------------------------------------------------- */
NTSTATUS
I2cHwEnable(
    PI2CCTRL_FDO fdoExt,
    BOOLEAN      Enable
    )
{
    NTSTATUS st = STATUS_SUCCESS;
    I2C_HW_STATUS hwst;
    ULONG spins;

    if (fdoExt == NULL || fdoExt->Ops == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Enable) {
        if (fdoExt->Ops->Enable) {
            (VOID)fdoExt->Ops->Enable(fdoExt, TRUE);
        }
        fdoExt->Enabled = TRUE;
    } else {
        if (fdoExt->Ops->Enable) {
            (VOID)fdoExt->Ops->Enable(fdoExt, FALSE);
        }
        fdoExt->Enabled = FALSE;

        /* Wait until controller reports idle (activity cleared) */
        if (fdoExt->Ops->GetStatus) {
            RtlZeroMemory(&hwst, sizeof(hwst));
            for (spins = 0U; spins < 4000U; ++spins) {
                if (NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                    if (!hwst.ControllerActive) {
                        break; /* bus idle */
                    }
                }
                KeStallExecutionProcessor(1);
            }
        }
    }

    return st;
}


/* -----------------------------------------------------------------------
 * I2cHwSetSpeed - configure bus speed via HAL ops
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant
 * ----------------------------------------------------------------------- */
NTSTATUS
I2cHwSetSpeed(
    PI2CCTRL_FDO     fdoExt,
    I2C_SPEED_MODE   Speed
    )
{
    NTSTATUS status;
    ULONG    speedHz;

    /* Defensive initialization */
    status  = STATUS_SUCCESS;
    speedHz = 0U;

    /* Validate context and ops table */
    if (fdoExt == NULL || fdoExt->Ops == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Disable controller before reconfiguring */
    if (fdoExt->Ops->Enable != NULL) {
        (VOID)fdoExt->Ops->Enable(fdoExt, FALSE);
    }

    /* Map abstract speed mode to Hz */
    switch (Speed) {
    case I2cSpeedStandard100k:
        speedHz = 100000U;
        break;
    case I2cSpeedFast400k:
        speedHz = 400000U;
        break;
    case I2cSpeedHigh3_4M:
        speedHz = 3400000U;
        break;
    default:
        /* Fallback: use active bus speed if available, else safe default */
        if (fdoExt->ActiveBusSpeedHz != 0U) {
            speedHz = fdoExt->ActiveBusSpeedHz;
        } else {
            speedHz = 100000U; /* safe baseline */
        }
        break;
    }

    /* Check HAL capabilities before programming */
    if (fdoExt->Ops->Caps.SupportsStandard100k == FALSE &&
        Speed == I2cSpeedStandard100k) {
        return STATUS_NOT_SUPPORTED;
    }
    if (fdoExt->Ops->Caps.SupportsFast400k == FALSE &&
        Speed == I2cSpeedFast400k) {
        return STATUS_NOT_SUPPORTED;
    }
    if (fdoExt->Ops->Caps.SupportsHigh3_4M == FALSE &&
        Speed == I2cSpeedHigh3_4M) {
        return STATUS_NOT_SUPPORTED;
    }

    /* Program bus speed via HAL */
    if (fdoExt->Ops->SetBusSpeedHz != NULL) {
        status = fdoExt->Ops->SetBusSpeedHz(fdoExt, speedHz);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    } else {
        return STATUS_NOT_SUPPORTED;
    }

    /* Clear interrupt mask via HAL (safe default) */
    if (fdoExt->Ops->MaskInterrupts != NULL) {
        fdoExt->Ops->MaskInterrupts(fdoExt, 0U);
    }

    /* Record chosen mode safely */
    fdoExt->SpeedMode = Speed;

    /* Re-enable controller */
    if (fdoExt->Ops->Enable != NULL) {
        status = fdoExt->Ops->Enable(fdoExt, TRUE);
    } else {
        status = STATUS_NOT_SUPPORTED;
    }

    return status;
}


/* ---------------------------------------------------------------------------
 * I2cHwMapAndInit - map resources and initialize controller via HAL
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cHwMapAndInit(
    PI2CCTRL_FDO     fdoExt,
    PHYSICAL_ADDRESS BarPa,
    ULONG            BarLength,
    I2C_SPEED_MODE   Speed
    )
{
    NTSTATUS       status;
    I2C_HW_STATUS  hwst;

    /* Defensive initialization */
    status = STATUS_SUCCESS;
    RtlZeroMemory(&hwst, sizeof(hwst));

    if (fdoExt == NULL || BarLength == 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    if (BarPa.QuadPart >= (1ULL << 32)) {
        return STATUS_NOT_SUPPORTED;
    }

    RtlZeroMemory(fdoExt, sizeof(*fdoExt));
    fdoExt->PhysAddr   = BarPa;
    fdoExt->MmioLength = BarLength;
    I2CCTRL_INIT_LOCK(&fdoExt->HwLock);

    /* Map resources via HAL */
    if (fdoExt->Ops != NULL && fdoExt->Ops->MapResources != NULL) {
        status = fdoExt->Ops->MapResources(fdoExt, NULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    } else {
        return STATUS_NOT_SUPPORTED;
    }

    /* Probe controller safely via HAL GetStatus */
    __try {
        if (fdoExt->Ops != NULL && fdoExt->Ops->GetStatus != NULL) {
            status = fdoExt->Ops->GetStatus(fdoExt, &hwst);
            if (!NT_SUCCESS(status)) {
                /* Unmap resources on failure */
                if (fdoExt->Ops->UnmapResources != NULL) {
                    fdoExt->Ops->UnmapResources(fdoExt);
                }
                return STATUS_DEVICE_NOT_READY;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (fdoExt->Ops != NULL && fdoExt->Ops->UnmapResources != NULL) {
            fdoExt->Ops->UnmapResources(fdoExt);
        }
        return STATUS_DEVICE_NOT_READY;
    }

    /* Ack any stale interrupts via HAL */
    if (fdoExt->Ops != NULL && fdoExt->Ops->AckInterrupts != NULL) {
        fdoExt->Ops->AckInterrupts(
            fdoExt,
            I2C_INT_RX_UNDER | I2C_INT_RX_OVER | I2C_INT_TX_OVER |
            I2C_INT_TX_ABORT  | I2C_INT_STOP_DETECTED | I2C_INT_START_DETECTED |
            I2C_INT_GEN_CALL | I2C_INT_ACTIVITY | I2C_INT_RX_DONE |
            I2C_INT_RD_REQ
        );
    }

    /* Configure initial bus speed */
    return I2cHwSetSpeed(fdoExt, Speed);
}

VOID I2cHwUnmapAndDeinit(PI2CCTRL_FDO fdoExt)
{
    if (fdoExt == NULL) return;
    (VOID)I2cHwEnable(fdoExt, FALSE);
    if (fdoExt->Mmio != NULL) {
        MmUnmapIoSpace(fdoExt->Mmio, fdoExt->MmioLength);
        fdoExt->Mmio = NULL;
    }
    fdoExt->MmioLength = 0U;
}

/* ---------------------------------------------------------------------------
 * I2cHwWrite - transmit a buffer to a 7-bit slave via HAL ops
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cHwWrite(
    PI2CCTRL_FDO fdoExt,
    UCHAR        Slave7,
    const UCHAR* Buf,
    ULONG        Len,
    BOOLEAN      SendStop
    )
{
    ULONG     i, spins;
    NTSTATUS  st;
    I2C_HW_STATUS hwst;

    if (fdoExt == NULL || fdoExt->Ops == NULL || Buf == NULL || Len == 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!fdoExt->Enabled) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Program target address via HAL */
    if (fdoExt->Ops->SetTarget7bit) {
        st = fdoExt->Ops->SetTarget7bit(fdoExt, Slave7);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }

    RtlZeroMemory(&hwst, sizeof(hwst));

    for (i = 0U; i < Len; ++i) {
        /* Poll until TX FIFO has space */
        for (spins = 0U; spins < I2C_POLL_SPINS_MAX; ++spins) {
            if (fdoExt->Ops->GetStatus &&
                NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                if (hwst.TxFifoNotFull || hwst.TxFifoLevel < fdoExt->Caps->TxFifoDepth) {
                    break;
                }
            }
            KeStallExecutionProcessor(1);
        }
        if (spins == I2C_POLL_SPINS_MAX) {
            return STATUS_IO_TIMEOUT;
        }

        /* Enqueue byte via HAL */
        if (fdoExt->Ops->IssueWriteByte) {
            st = fdoExt->Ops->IssueWriteByte(fdoExt, Buf[i]);
            if (!NT_SUCCESS(st)) {
                return st;
            }
        }

        /* Optional: emit restart on first byte */
        if (i == 0U && fdoExt->Ops->EmitRestartIfNeeded) {
            (VOID)fdoExt->Ops->EmitRestartIfNeeded(fdoExt);
        }

        /* Optional: emit stop on last byte if requested */
        if (SendStop && i == (Len - 1U) && fdoExt->Ops->EmitStopIfNeeded) {
            (VOID)fdoExt->Ops->EmitStopIfNeeded(fdoExt);
        }

        /* Check for abort via HAL status */
        if (fdoExt->Ops->GetStatus &&
            NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
            if (hwst.TxAborted) {
                if (fdoExt->Ops->AckInterrupts) {
                    fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_TX_ABORT);
                }
                return I2cCtrl_MapAbortSource(hwst.RawIntr);
            }
        }
    }

    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------------------
 * I2cHwRead - receive a buffer from a 7-bit slave via HAL ops
 * XP/2003 BSOD-safe, HAL-generic, C89-compliant
 * --------------------------------------------------------------------------- */
NTSTATUS
I2cHwRead(
    PI2CCTRL_FDO fdoExt,
    UCHAR        Slave7,
    UCHAR*       Buf,
    ULONG        Len,
    BOOLEAN      SendStop
    )
{
    ULONG          i, spins, spinsStop;
    NTSTATUS       st;
    I2C_HW_STATUS  hwst;

    if (fdoExt == NULL || fdoExt->Ops == NULL || Buf == NULL || Len == 0U) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!fdoExt->Enabled) {
        return STATUS_DEVICE_NOT_READY;
    }

    /* Program target address via HAL */
    if (fdoExt->Ops->SetTarget7bit) {
        st = fdoExt->Ops->SetTarget7bit(fdoExt, Slave7 & 0x7F);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }

    RtlZeroMemory(&hwst, sizeof(hwst));

    /* Issue read requests */
    for (i = 0U; i < Len; ++i) {
        /* Wait until TX FIFO has space */
        for (spins = 0U; spins < I2C_POLL_SPINS_MAX; ++spins) {
            if (fdoExt->Ops->GetStatus &&
                NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                if (hwst.TxFifoNotFull || hwst.TxFifoLevel < fdoExt->Caps->TxFifoDepth) {
                    break;
                }
            }
            KeStallExecutionProcessor(I2C_STALL_US);
        }
        if (spins == I2C_POLL_SPINS_MAX) {
            return STATUS_IO_TIMEOUT;
        }

        if (fdoExt->Ops->IssueReadToken) {
            st = fdoExt->Ops->IssueReadToken(fdoExt);
            if (!NT_SUCCESS(st)) {
                return st;
            }
        }

        /* Optional: emit restart on first token */
        if (i == 0U && fdoExt->Ops->EmitRestartIfNeeded) {
            (VOID)fdoExt->Ops->EmitRestartIfNeeded(fdoExt);
        }

        /* Optional: emit stop on last token if requested */
        if (SendStop && i == (Len - 1U) && fdoExt->Ops->EmitStopIfNeeded) {
            (VOID)fdoExt->Ops->EmitStopIfNeeded(fdoExt);
        }

        /* Check for abort via HAL status */
        if (fdoExt->Ops->GetStatus &&
            NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
            if (hwst.TxAborted) {
                if (fdoExt->Ops->AckInterrupts) {
                    fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_TX_ABORT);
                }
                return STATUS_IO_DEVICE_ERROR;
            }
        }
    }

    /* Drain RX FIFO */
    for (i = 0U; i < Len; ++i) {
        for (spins = 0U; spins < I2C_POLL_SPINS_MAX; ++spins) {
            if (fdoExt->Ops->GetStatus &&
                NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                if (hwst.RxFifoNotEmpty || hwst.RxFifoLevel > 0U) {
                    break;
                }
            }
            KeStallExecutionProcessor(I2C_STALL_US);
        }
        if (spins == I2C_POLL_SPINS_MAX) {
            return STATUS_IO_TIMEOUT;
        }

        if (fdoExt->Ops->ReadRxByte) {
            st = fdoExt->Ops->ReadRxByte(fdoExt, &Buf[i]);
            if (!NT_SUCCESS(st)) {
                return st;
            }
        } else if (fdoExt->Ops->ReadRxByteSafe) {
            st = fdoExt->Ops->ReadRxByteSafe(fdoExt, &Buf[i]);
            if (!NT_SUCCESS(st)) {
                return st;
            }
        }
    }

    /* Optionally wait for STOP detect */
    if (SendStop) {
        for (spinsStop = 0U; spinsStop < I2C_POLL_SPINS_MAX; ++spinsStop) {
            if (fdoExt->Ops->GetStatus &&
                NT_SUCCESS(fdoExt->Ops->GetStatus(fdoExt, &hwst))) {
                if (hwst.StopDetected) {
                    if (fdoExt->Ops->AckInterrupts) {
                        fdoExt->Ops->AckInterrupts(fdoExt, I2C_INT_STOP_DETECTED);
                    }
                    break;
                }
            }
            KeStallExecutionProcessor(I2C_STALL_US);
        }
    }

    return STATUS_SUCCESS;
}
