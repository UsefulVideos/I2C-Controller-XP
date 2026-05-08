# I²C Controller Driver (`i2cctrl.sys`)

**Version 1.0.0**  
**Windows XP / Windows Server 2003 (x86/x64)**  
**ACPI/PCI‑aware I²C Host Controller Bus Driver**

`i2cctrl.sys` is a fully‑functional I²C host controller bus driver for Windows XP/2003.  
It implements a complete PnP lifecycle, exposes a universal HID‑over‑I²C child PDO, and provides a safe, restart‑resilient I²C/SMBus transfer engine compatible with legacy NT5 kernels.

---

## 🔧 Core Driver Framework

### Full PnP lifecycle (FDO)
The driver implements the complete NT5 PnP stack for a bus‑type FDO:

- `IRP_MN_START_DEVICE`
- `IRP_MN_STOP_DEVICE`
- `IRP_MN_SURPRISE_REMOVAL`
- `IRP_MN_REMOVE_DEVICE`

During START, the driver:

- Parses translated resource lists  
- Maps MMIO regions  
- Connects interrupts  
- Initializes transfer queues and hardware state  

During STOP/REMOVE, it:

- Cancels timers  
- Drains queues  
- Completes pending IRPs  
- Disconnects interrupts  
- Unmaps MMIO  

### XP/2003‑safe restart model
`I2cCtrl_StopDevice` is **idempotent**, ensuring:

- Disable/enable cycles do not leave stale state  
- Surprise removal does not crash the system  
- Reinstall cycles avoid Code 10  
- All hardware state is torn down deterministically  

### Controller identification
Supports both ACPI‑described and PCI‑enumerated controllers:

- ACPI: `INT3446`, `INT3447`, `AMD0010`, `AMDI0010`
- PCI: Common Intel/AMD I²C host controller VEN/DEV IDs

---

## 📑 IOCTL Interface

### Raw I²C / SMBus transfers
The driver exposes a synchronous I²C/SMBus transfer engine:

- Internal serialized request queue  
- `SMBUS_REQUEST` wrappers  
- Worker‑thread execution for XP safety  
- Timeout‑protected synchronous completion  

### XP‑friendly interface
The IOCTL surface is HAL‑neutral and designed for:

- HID‑over‑I²C stacks  
- Sensor stacks  
- Custom I²C clients  

Higher‑level protocols sit cleanly above the bus layer.

---

## ⚡ Interrupt & FIFO Engine

### ISR/DPC pipeline
- ISR acknowledges interrupt causes  
- Schedules DPC for heavy work  
- DPC drains RX FIFO, fills TX FIFO, updates transfer context  
- Completes IRPs in order  

### Interrupt‑optional design
If no usable IRQ exists:

- Driver falls back to reduced/polling mode  
- Still functional for simple devices  

### Locking discipline
Spinlocks protect all shared state:

- `HwLock`  
- `QueueLock`  
- `PendingIrpLock`  
- `BusLock`  

This ensures ISR/DPC/worker paths remain race‑free.

---

## 🖐 HID‑over‑I²C Child PDO

### Universal child PDO
The driver synthesizes a single HID‑over‑I²C child:

- Hardware ID: `ACPI\PNP0C50`
- Compatible ID: `PNP0C50`
- Instance ID: `0000`

### PDO lifecycle
The PDO:

- Responds to `IRP_MN_QUERY_ID`  
- Is created once per controller  
- Is removed only when the FDO is removed  

### XP‑specific behavior
Windows XP cannot enumerate I²C ACPI children.  
Therefore, the bus driver:

- Hardcodes a `PNP0C50` PDO  
- Returns it via `BusRelations`  
- Allows HID‑over‑I²C stacks to load normally  

---

## 📦 ACPI & PCI Integration

### ACPI helpers
- Optional per‑child ACPI handle (`AcpiHandle`)  
- Safe teardown via `I2cCtrl_AcpiCloseChild`  
- Evaluation helpers for controller/child ACPI methods  

### PCI/ACPI resource handling
- Uses translated resource lists  
- Locates MMIO + IRQ  
- Validates MMIO length  
- Maps via `MmMapIoSpace`  
- Unmaps on STOP/REMOVE  

### Child enumeration
`I2cCtrl_QueryDeviceRelations` returns:

- The active child list  
- Including the HID PDO  

---

## 🛠 Diagnostics & Safety

### XP/2003 BSOD‑resilience
STOP/SURPRISE/REMOVE paths:

- Drain queues  
- Cancel timers  
- Complete pending IRPs  
- Disconnect interrupts  
- Unmap MMIO  

All teardown paths are **idempotent**, preventing:

- Double‑free  
- Double‑unmap  
- Stale pointers  
- Code 10 errors  

### Debugging support
- `KdPrint` / `TraceEvents` around all major transitions  
- Assertions ensure child list emptiness at FDO REMOVE  
- Verbose tracing for Start/Stop, child creation, and PnP flow  

### C89 / WDK‑style codebase
- C89‑compliant initialization  
- Explicit zeroing  
- HAL‑neutral register access via ops table (`Ops`)  

---

## 📁 INF Overview

The driver is installed via `i2cctrl.inf`, which:

- Targets XP 5.1 and XP/2003 5.2 (x86/x64)  
- Registers the service as:

ServiceBinary = %12%\i2cctrl.sys
ImagePath     = \SystemRoot\System32\drivers\i2cctrl.sys


- Enumerates supported ACPI and PCI hardware IDs  
- Creates the HID‑over‑I²C child PDO  
- Registers event logging  
- Installs registry‑based quirks and policies  

---

## 🎯 Summary

`i2cctrl.sys` **v1.0.0** is an **XP/2003‑oriented, ACPI/PCI‑aware I²C host controller bus driver** that:

- Implements a complete, restart‑safe PnP lifecycle  
- Exposes a universal HID‑over‑I²C child PDO (`ACPI\PNP0C50`)  
- Manages MMIO, interrupts, ISR/DPC, and request queues safely  
- Cleans up deterministically on STOP/SURPRISE/REMOVE  
- Avoids Code 10 and stale device state across disable/enable cycles  

It is a fully functional NT5‑era I²C bus driver suitable for HID‑over‑I²C devices, sensors, and custom I²C clients.

---

## 📂 Project Structure

