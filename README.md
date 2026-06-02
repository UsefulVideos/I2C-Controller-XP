# **I²C Controller Driver (`i2cctrl.sys`) — Accurate Technical Description**

**Version 1.0.0**  
**Windows XP / Windows Server 2003 (x86/x64)**  
**LPSS‑aware, PCI‑enumerated I²C Host Controller Driver with Unified Power/Reset Quirk Engine**

`i2cctrl.sys` is a modern NT5‑compatible I²C host controller driver for Intel/AMD SoCs.  
It implements a complete PnP bus‑type FDO, a DW‑I²C hardware abstraction layer, a unified LPSS/PMC power‑up quirk engine, and a synthetic HID‑over‑I²C child PDO for XP systems that cannot enumerate ACPI I²C devices.

The driver includes a **persistent on‑disk debug log** at:

> **`%SystemRoot%\System32\i2cctrl.log`**

This log records every major transition, register dump, quirk, and failure path, making it possible to diagnose power‑gated controllers and LPSS bring‑up issues on XP.

---

## 🔧 **Driver Architecture**

### **PnP Bus‑Type FDO**
Implements the full NT5 PnP lifecycle:

- **IRP_MN_START_DEVICE**  
- **IRP_MN_STOP_DEVICE**  
- **IRP_MN_SURPRISE_REMOVAL**  
- **IRP_MN_REMOVE_DEVICE**  

During START, the driver:

- Parses PCI/ACPI translated resources  
- Maps BAR0 (DW‑I²C registers)  
- Maps LPSS private registers (PWRMBASE + offset)  
- Maps **PWRMBASE** for PMC/LPSS power sequencing  
- Connects the interrupt  
- Initializes the DW‑I²C ops table  
- Applies unified quirks (clock gate, reset, DMA disable, broken gate, etc.)

All of these steps are logged to:

> **`%SystemRoot%\System32\i2cctrl.log`**

with timestamps and register snapshots.

### **Idempotent Restart Model**
`I2cCtrl_StopDevice` and `I2cCtrl_StartDevice` are **idempotent**, enabling:

- Safe disable/enable cycles  
- Recovery from failed START  
- No Code 10 after restart  
- Deterministic teardown of all mappings and queues  

---

## ⚡ **LPSS / PMC Power & Reset Engine**

Modern Intel LPSS controllers often boot in **D3cold** with:

- BAR0 = all **0x00000000**  
- PWRMBASE = all **0xFFFFFFFF**  
- LPSS private registers = **0xFFFFFFFF**

`i2cctrl.sys` includes a **unified quirk engine** that:

- Clears LPSS clock gates  
- Deasserts LPSS reset  
- Enables functional clocks  
- Applies silicon‑specific workarounds  
- Forces PIO mode when DMA is unsupported  
- Performs extra reset cycles when required  

Every step is logged to:

> **`%SystemRoot%\System32\i2cctrl.log`**

including:

- `PMC4 BEFORE/AFTER`  
- `PW_FORCE_ON BEFORE/AFTER`  
- `PW_STS FINAL`  
- `LPSS CLK_CTL / RST_CTL / RST_STS`  
- BAR0 dumps before/after enable  
- Quirk names applied  

### **DW‑I²C Enable Path**
The driver calls:

```
Ops->Enable(TRUE)
```

Then polls the enable bit using:

- `WaitForEnableState(targetOn=1)`  
- Timeout‑protected loops  
- BAR0 re‑reads before/after enable  

If the controller remains unpowered:

- `HardwareFailure = TRUE`  
- All mappings are torn down  
- Failure is logged  
- PnP START returns failure  

---

## 🧩 **DW‑I²C Hardware Abstraction Layer**

The driver uses a runtime ops table:

- Register read/write  
- FIFO fill/drain  
- Interrupt cause decoding  
- Enable/disable sequences  
- Status polling  
- Transfer state machine  

This supports:

- Intel LPSS DW‑I²C  
- AMD I²C  
- ACPI‑described controllers  
- PCI‑enumerated controllers  

---

## 🖐 **HID‑over‑I²C Child PDO (XP‑specific)**

Windows XP cannot enumerate ACPI I²C devices (`PNP0C50`).  
Therefore, the driver synthesizes a **universal HID‑over‑I²C PDO**:

- Hardware ID: **`ACPI\PNP0C50`**  
- Compatible ID: **`PNP0C50`**  
- Instance ID: **`0000`**

This PDO is:

- Created during START  
- Returned in `BusRelations`  
- Removed only when the FDO is removed  

Creation and removal are logged to:

> **`%SystemRoot%\System32\i2cctrl.log`**

---

## 📑 **I/O & Transfer Engine**

### **Synchronous I²C/SMBus Transfers**
The driver exposes a safe, serialized transfer engine:

- Internal request queue  
- Worker‑thread execution (XP‑safe)  
- Timeout‑protected synchronous completion  
- SMBus wrappers for legacy clients  

### **Interrupt / DPC Pipeline**
- ISR acknowledges causes  
- DPC drains RX FIFO, fills TX FIFO  
- Updates transfer state machine  
- Completes IRPs in order  

If no IRQ is usable, the driver falls back to **polling mode**.

All ISR/DPC transitions are logged to:

> **`%SystemRoot%\System32\i2cctrl.log`**

---

## 📦 **ACPI & PCI Integration**

### **PCI Path**
For PCI controllers (e.g., **`PCI\VEN_8086&DEV_9DE9`**):

- BAR0 is mapped as DW‑I²C registers  
- PWRMBASE is derived from platform policy  
- LPSS private registers are mapped at `PWRMBASE + offset`  
- Interrupts are connected via translated resources  

### **ACPI Path**
For ACPI controllers (e.g., **`INT3446`**, **`INT3447`**):

- ACPI handle is opened (if present)  
- ACPI methods may be evaluated for power/reset  
- XP‑safe ACPI teardown is used  

### **Child Enumeration**
`I2cCtrl_QueryDeviceRelations` returns:

- The HID PDO  
- Any ACPI‑enumerated children (if present)  

---

## 🛠 **Diagnostics & Safety**

### **Persistent On‑Disk Logging**
All major driver events are written to:

> **`%SystemRoot%\System32\i2cctrl.log`**

Including:

- PnP transitions  
- BAR0 dumps  
- PWRMBASE/LPSS register snapshots  
- Quirk application  
- ISR/DPC activity  
- ACPI enumeration failures  
- HardwareFailure detection  
- START/STOP/SURPRISE/REMOVE sequences  

This log is the primary diagnostic tool for power‑gated LPSS controllers on XP.

### **Robust XP/2003 Teardown**
All teardown paths:

- Cancel timers  
- Drain queues  
- Disconnect interrupts  
- Unmap MMIO  
- Free child PDOs  
- Zero internal state  

This prevents:

- Double‑unmap  
- Double‑free  
- Stale pointers  
- Code 10 errors  
- BSODs during SURPRISE/REMOVE  

---

## 📁 **INF Overview**

`i2cctrl.inf`:

- Targets XP 5.1 and XP/2003 5.2  
- Registers the service:

```
ServiceBinary = %12%\i2cctrl.sys
ImagePath     = \SystemRoot\System32\drivers\i2cctrl.sys
```

- Enumerates supported ACPI and PCI IDs  
- Creates the HID‑over‑I²C PDO  
- Installs registry‑based quirks  
- Enables verbose logging  

---

# 🎯 **Summary**

`i2cctrl.sys` is a **modern LPSS‑aware I²C controller driver for XP/2003** that:

- Handles PCI/ACPI I²C controllers  
- Implements a full NT5 PnP bus driver  
- Includes a unified LPSS/PMC power‑up quirk engine  
- Uses a DW‑I²C ops table for hardware abstraction  
- Synthesizes a HID‑over‑I²C PDO for XP  
- Provides a safe, serialized transfer engine  
- Recovers cleanly from failed START due to power‑gated hardware  
- Logs every detail to **`%SystemRoot%\System32\i2cctrl.log`**  
- Avoids Code 10 and stale state across restarts  