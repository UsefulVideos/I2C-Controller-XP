# **I²C Controller Driver (`i2cctrl.sys`) — Accurate Technical Description**

**Version 1.0.0**  
**Windows XP / Windows Server 2003 (x86/x64)**  
**LPSS‑aware, PCI‑enumerated I²C Host Controller Driver with Unified Power/Reset Quirk Engine**

`i2cctrl.sys` is a full NT5‑era I²C host controller driver designed for modern Intel/AMD SoCs whose I²C controllers are exposed through PCI or ACPI.  
It implements a complete PnP bus‑type FDO, a DW‑I²C hardware abstraction layer, a unified LPSS power/reset quirk engine, and a synthetic HID‑over‑I²C child PDO for XP systems that cannot enumerate ACPI I²C devices.

---

## 🔧 **Driver Architecture**

### **PnP Bus‑Type FDO**
The driver implements the full NT5 PnP lifecycle:

- **IRP_MN_START_DEVICE**  
- **IRP_MN_STOP_DEVICE**  
- **IRP_MN_SURPRISE_REMOVAL**  
- **IRP_MN_REMOVE_DEVICE**  

During START, the driver:

- Parses PCI/ACPI translated resources  
- Maps BAR0 (DW‑I²C registers)  
- Maps LPSS private registers (BAR2‑style offset from PWRMBASE)  
- Maps **PWRMBASE** for PMC/LPSS power sequencing  
- Connects the interrupt  
- Initializes the DW‑I²C ops table  
- Applies unified quirks (clock gate, reset, DMA disable, broken gate, etc.)

During STOP/REMOVE:

- All mappings are torn down  
- Interrupts disconnected  
- Queues drained  
- State cleared deterministically  

### **Idempotent Restart Model**
`I2cCtrl_StopDevice` and `I2cCtrl_StartDevice` are designed to be **idempotent**, allowing:

- Disable/enable cycles without Code 10  
- Safe recovery from failed START  
- Graceful handling of power‑gated controllers  
- Re‑initialization after quirk application  

---

## ⚡ **LPSS / PMC Power & Reset Engine**

Modern Intel LPSS controllers often boot in **D3cold** with:

- BAR0 returning all **0x00000000**  
- PWRMBASE returning **0xFFFFFFFF**  
- LPSS private registers returning **0xFFFFFFFF**

`i2cctrl.sys` includes a **unified quirk engine** that:

- Clears LPSS clock gates  
- Deasserts LPSS reset  
- Enables functional clocks  
- Applies platform‑specific workarounds  
- Forces PIO mode when DMA is unsupported  
- Handles broken clock‑gate silicon  
- Performs extra reset cycles when required  

This engine runs **after** MMIO mapping and **before** DW‑I²C enable.

### **DW‑I²C Enable Path**
The driver calls:

```
Ops->Enable(TRUE)
```

Then polls the DW‑I²C enable bit using:

- `WaitForEnableState(targetOn=1)`  
- Timeout‑protected loops  
- BAR0 re‑reads before/after enable  

If the controller remains unpowered (all zeros), the driver:

- Marks `HardwareFailure = TRUE`  
- Tears down all mappings  
- Returns failure to PnP  
- Still enumerates the HID child PDO (XP requirement)

---

## 🧩 **DW‑I²C Hardware Abstraction Layer**

The driver uses a runtime ops table:

- Register read/write helpers  
- FIFO fill/drain  
- Interrupt cause decoding  
- Enable/disable sequences  
- Status polling  
- Transfer engine state machine  

This allows support for:

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

This allows XP to load HID‑over‑I²C touchpad drivers normally.

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

### **Verbose Debugging**
The driver logs:

- BAR0 dumps before/after enable  
- PWRMBASE/LPSS register snapshots  
- Quirk application steps  
- ISR/DPC transitions  
- PnP state transitions  
- ACPI enumeration failures  

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

# 🎯 **Summary (Accurate to Reality)**

`i2cctrl.sys` is a **modern LPSS‑aware I²C controller driver for XP/2003** that:

- Handles PCI/ACPI I²C controllers  
- Implements a full NT5 PnP bus driver  
- Includes a unified LPSS/PMC power‑up quirk engine  
- Uses a DW‑I²C ops table for hardware abstraction  
- Synthesizes a HID‑over‑I²C PDO for XP  
- Provides a safe, serialized transfer engine  
- Recovers cleanly from failed START due to power‑gated hardware  
- Avoids Code 10 and stale state across restarts  