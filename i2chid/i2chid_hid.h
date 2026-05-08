/* i2chid_hid.h
 * Public interface for the I2C HID FDO driver.
 * C89 compliant.
 */

#ifndef _I2CHID_HID_H_
#define _I2CHID_HID_H_

/* Core kernel/HID headers */
#include <ntddk.h>
#include <hidclass.h>
#include <hidport.h>

/* HID driver internal headers */
#include "i2chid_ext.h"    /* PI2CHID_FDO */
#include "i2chid.h"        /* I2CHID_REPORT, HID descriptors, HID helpers */
#include "i2chid_spbcx.h"  /* façade IOCTL codes and HID‑side prototypes */

/* Pool tag for allocations */
#define I2CHID_POOL_TAG 'Hc2I'

/* ---------------------------------------------------------------------------
   Bring in bus driver definitions (IOCTL contract and façade types)
   --------------------------------------------------------------------------- */
#include "..\i2cctrl\i2cctrl_ioctl.h"
#include "..\i2cctrl\i2cctrl_ext.h"
#include "..\i2cctrl\i2cctrl_spbcx.h"

/* Dispatch prototypes */
NTSTATUS I2CHID_DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS I2CHID_DispatchInternalIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* Interrupt helpers */
BOOLEAN I2CHID_InterruptServiceRoutine(PKINTERRUPT Interrupt, PVOID ServiceContext);
VOID    I2CHID_InterruptDpc(PKDPC Dpc,
                            PVOID DeferredContext,
                            PVOID Arg1,
                            PVOID Arg2);

/* Input helpers */
BOOLEAN I2CHID_ReadInput(PI2CHID_FDO Ext, PUCHAR Buf, ULONG BufLen);
VOID    I2CHID_CompleteQueuedReads(PI2CHID_FDO Ext, PUCHAR Data, ULONG Len);

/* I²C helper to issue a read down to the lower i2cctrl device */
NTSTATUS I2CHID_IoctlRead(PI2CHID_FDO Ext,
                          PI2C_READ_PARAMS Params,
                          PUCHAR Buffer,
                          ULONG Length);

/* HID façade helpers (bridge to bus PDO) */
NTSTATUS I2CHID_IoctlSetTarget(PI2CHID_FDO Ext,
                               PVOID InBuf,
                               ULONG InLen);

NTSTATUS I2CHID_IoctlReadReport(PI2CHID_FDO Ext,
                                PVOID OutBuf,
                                ULONG OutLen);

NTSTATUS I2CHID_IoctlWriteReport(PI2CHID_FDO Ext,
                                 PVOID InBuf,
                                 ULONG InLen);

/* -----------------------------------------------------------------------
   Precision Touchpad-like HID report descriptor (XP/2003-friendly, C89)
   - Application 1: Digitizers / Touch Pad
       • Input Report ID 1: Contact Count + 5 Finger logical collections
       • Feature Report ID 2: Contact Count Maximum
       • Feature Report ID 3: Surface dimensions
       • Physical buttons: 3 zones mapped as button inputs
   - Application 2: Mouse
       • Buttons (3) + relative X/Y + wheel
   - Application 3: Keyboard (modifiers only)
       • Modifier byte includes Ctrl/Shift/Alt/GUI
   ----------------------------------------------------------------------- */

extern const UCHAR g_HidReportDesc[];
extern const size_t g_HidReportDescSize;

/* HID report send helpers */
VOID
I2CHID_SendMouseReport(
    PI2CHID_PT_DEVEXT dev,
    UCHAR buttons,
    CHAR dx,
    CHAR dy,
    CHAR wheelV,
    CHAR wheelH
    );

VOID
I2CHID_SendKeyboardModifier(
    PI2CHID_PT_DEVEXT dev,
    UCHAR modifierBits
    );

VOID
I2CHID_PT_EmulateTouchpad(
    PI2CHID_PT_DEVEXT dev,
    const PT_RAW_SAMPLE* s,
    UCHAR* buttons,
    CHAR* dxRel,
    CHAR* dyRel,
    CHAR* wheelVRel,
    CHAR* pinchZoomRel
    );

#endif /* _I2CHID_HID_H_ */
