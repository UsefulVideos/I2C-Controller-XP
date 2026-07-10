/* =========================================================================
   I2CHID_contacts.h
   -------------------------------------------------------------------------
   Header for HID-over-I²C contact and report format constants.

   Contains:
     - Multitouch emulation report layout (fixed-size buffer)
     - Mouse emulation report size (compatibility)
     - Controller-side packet format maximum size
     - HID Feature Report constants (ReportID 5, field count, size)

   This header is included by i2cctrl_ext.h and other driver modules
   to provide consistent definitions for contact parsing and feature
   report handling.
   ========================================================================= */

#ifndef I2CHID_CONTACTS_H_
#define I2CHID_CONTACTS_H_

/* -----------------------------------------------------------------------
   Multitouch emulation report format (fixed-size buffer)
   Layout (little-endian; absolute coords 16-bit; up to 5 contacts):
   Byte 0: ContactCount (0..5)
   Per contact (6 bytes): Flags, Xlo, Xhi, Ylo, Yhi, ID
   ----------------------------------------------------------------------- */
#define MT_MAX_CONTACTS     5
#define MT_CONTACT_BYTES    6
#define MT_REPORT_SIZE      (1 + MT_MAX_CONTACTS * MT_CONTACT_BYTES) /* 31 bytes */

/* Mouse emulation report (compatibility): [buttons, dx, dy] */
#define MOUSE_REPORT_SIZE   3

/* Controller-side packet format (example; device-specific) */
#define I2C_INPUT_PACKET_MAX 32

/* -----------------------------------------------------------------------
   HID Feature Report format (ReportID 5)
   Fields: ScrollScale, ZoomScale, TapTimeMs, TapDistance,
           PalmThreshold, SwipeScale, RotateScale, PollIntervalMs
   ----------------------------------------------------------------------- */
#define I2CHID_FEATURE_FIELDS 8U
#define I2CHID_FEATURE_SIZE   (1U + (I2CHID_FEATURE_FIELDS * 4U))
#define I2CHID_FEATURE_RID    5

#endif /* I2CHID_CONTACTS_H_ */
