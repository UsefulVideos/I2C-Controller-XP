// I2CHID_mouse.h
// Private header for HID-over-I2C (PNP0C50) integration with mouclass.sys
// Provides internal IOCTLs and CONNECT_DATA structure missing from ntddmou.h

#ifndef _I2C_MOUSE_H_
#define _I2C_MOUSE_H_

#include <ntddk.h>
#include <ntddmou.h>

/*
 * Internal IOCTLs used to connect/disconnect a port driver to mouclass.sys
 */
#define IOCTL_INTERNAL_MOUSE_CONNECT    CTL_CODE(FILE_DEVICE_MOUSE, 0x0F0, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_INTERNAL_MOUSE_DISCONNECT CTL_CODE(FILE_DEVICE_MOUSE, 0x0F1, METHOD_NEITHER, FILE_ANY_ACCESS)

/*
 * Structure passed with IOCTL_INTERNAL_MOUSE_CONNECT
 * ClassService is actually a PMOUSE_CLASS_SERVICE_CALLBACK
 */
typedef struct _CONNECT_DATA {
    PDEVICE_OBJECT ClassDeviceObject;
    PVOID          ClassService;
} CONNECT_DATA, *PCONNECT_DATA;

/*
 * Prototype for the class service callback
 */
typedef VOID (*PMOUSE_CLASS_SERVICE_CALLBACK)(
    IN PDEVICE_OBJECT DeviceObject,
    IN PMOUSE_INPUT_DATA InputDataStart,
    IN PMOUSE_INPUT_DATA InputDataEnd,
    IN OUT PULONG InputDataConsumed
    );

#endif /* _I2C_MOUSE_H_ */
