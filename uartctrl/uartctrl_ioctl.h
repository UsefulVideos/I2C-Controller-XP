/* -----------------------------------------------------------------------
   uartctrl_ioctl.h – IOCTL codes and structures for UART controller
   ----------------------------------------------------------------------- */

#pragma once
#include <ntddk.h>

//
// Custom device type for UART controller
//
#define FILE_DEVICE_UARTCTRL  0x8333

//
// IOCTL definitions
//
#define IOCTL_UART_OPEN              CTL_CODE(FILE_DEVICE_UARTCTRL, 0x001, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_UART_CLOSE             CTL_CODE(FILE_DEVICE_UARTCTRL, 0x002, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_UART_SET_CONFIG        CTL_CODE(FILE_DEVICE_UARTCTRL, 0x003, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_UART_GET_CONFIG        CTL_CODE(FILE_DEVICE_UARTCTRL, 0x004, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_UART_WRITE             CTL_CODE(FILE_DEVICE_UARTCTRL, 0x005, METHOD_IN_DIRECT,  FILE_ANY_ACCESS)
#define IOCTL_UART_READ              CTL_CODE(FILE_DEVICE_UARTCTRL, 0x006, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_UART_GET_STATUS        CTL_CODE(FILE_DEVICE_UARTCTRL, 0x007, METHOD_BUFFERED,   FILE_ANY_ACCESS)
#define IOCTL_UART_SET_FLOW_CONTROL  CTL_CODE(FILE_DEVICE_UARTCTRL, 0x008, METHOD_BUFFERED,   FILE_ANY_ACCESS)

//
// UART configuration structure
//
typedef struct _UARTCTRL_CONFIG {
    ULONG BaudRate;    // Baud rate (e.g., 9600, 115200)
    UCHAR Parity;      // 0=None, 1=Odd, 2=Even
    UCHAR StopBits;    // 1 or 2
    UCHAR DataBits;    // typically 7 or 8
    UCHAR Reserved;    // reserved for alignment/padding
} UARTCTRL_CONFIG, *PUARTCTRL_CONFIG;

//
// UART status structure
//
typedef struct _UARTCTRL_STATUS {
    ULONG RxBytesAvailable;  // bytes currently in RX buffer
    ULONG TxBytesFree;       // free space in TX buffer
    ULONG Errors;            // bitfield: overrun, parity, framing errors
    ULONG Flags;             // modem signals: CTS/DSR/DCD/RI if exposed
} UARTCTRL_STATUS, *PUARTCTRL_STATUS;
