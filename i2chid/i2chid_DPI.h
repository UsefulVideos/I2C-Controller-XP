/* -----------------------------------------------------------------------
   I2CHID_DPI.h - HID-over-I2C Function Driver DPI helpers (final clean header)
   ----------------------------------------------------------------------- */

#ifndef _I2CHID_DPI_H_
#define _I2CHID_DPI_H_

#include "I2CHID_spinlock_fix.h"
#include "..\i2cctrl\i2cctrl_DPI.h"

/*
 * Pool tag used for HID DPI allocations
 */
#define I2CHID_DPI_TAG 'hDPI'

/*
 * Initialize HID function driver DPI context from bus-provided DPI block.
 * Copies all relevant fields into the HID DPI structure.
 */
NTSTATUS
I2CHID_DpiInitializeFromBus(
    IN PI2CCTRL_DPI BusDpi,
    OUT PI2CCTRL_DPI HidDpi
    );

/*
 * Apply registry policy overrides to HID DPI context.
 * Reads DWORD values from Parameters key and applies them.
 */
NTSTATUS
I2CHID_DpiApplyRegistryPolicy(
    IN OUT PI2CCTRL_DPI HidDpi,
    IN PUNICODE_STRING RegistryPath
    );

/*
 * Register HID interface for function driver.
 * Uses GUID_DEVINTERFACE_HID from hidclass.h.
 */
NTSTATUS
I2CHID_DpiRegisterInterface(
    IN PDEVICE_OBJECT DeviceObject,
    OUT PUNICODE_STRING SymbolicLinkName
    );

/*
 * Unregister HID interface.
 * Disables the previously registered HID interface.
 */
VOID
I2CHID_DpiUnregisterInterface(
    IN PUNICODE_STRING SymbolicLinkName
    );

/*
 * Fetch DPI from bus PDO.
 * Retrieves DPI context from the bus/child PDO.
 */
NTSTATUS
I2CHID_FetchBusDpi(
    IN PDEVICE_OBJECT PhysicalDeviceObject,
    OUT PI2CCTRL_DPI DpiOut
    );

#endif /* _I2CHID_DPI_H_ */
