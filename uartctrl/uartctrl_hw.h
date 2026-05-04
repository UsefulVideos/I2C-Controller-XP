/* -----------------------------------------------------------------------
   uartctrl_hw.h – UART hardware register definitions and prototypes
   ----------------------------------------------------------------------- */

#pragma once

#include <ntddk.h>
#include "uartctrl_ext.h"

// 16550-like register offsets (byte addressing)
#define UART_RBR   0x00  // Receiver Buffer (read)
#define UART_THR   0x00  // Transmit Holding (write)
#define UART_IER   0x01  // Interrupt Enable
#define UART_IIR   0x02  // Interrupt Identification (read)
#define UART_FCR   0x02  // FIFO Control (write)
#define UART_LCR   0x03  // Line Control
#define UART_MCR   0x04  // Modem Control
#define UART_LSR   0x05  // Line Status
#define UART_MSR   0x06  // Modem Status
#define UART_SCR   0x07  // Scratch
#define UART_DLL   0x00  // Divisor Latch Low (when DLAB=1)
#define UART_DLM   0x01  // Divisor Latch High (when DLAB=1)

// IER bits
#define IER_RDA    0x01  // Enable Received Data Available interrupt
#define IER_THRE   0x02  // Enable Transmitter Holding Register Empty interrupt
#define IER_RLS    0x04  // Enable Receiver Line Status interrupt
#define IER_MS     0x08  // Enable Modem Status interrupt

// LCR bits
#define LCR_WLS_5  0x00
#define LCR_WLS_6  0x01
#define LCR_WLS_7  0x02
#define LCR_WLS_8  0x03
#define LCR_STB    0x04
#define LCR_PEN    0x08
#define LCR_EPS    0x10
#define LCR_SPS    0x20
#define LCR_DLAB   0x80

// FCR bits
#define FCR_FIFO_EN  0x01
#define FCR_RXRST    0x02
#define FCR_TXRST    0x04
#define FCR_DMA_SEL  0x08
#define FCR_TRIG_1   0x00
#define FCR_TRIG_4   0x40
#define FCR_TRIG_8   0x80
#define FCR_TRIG_14  0xC0

// LSR bits
#define LSR_DR     0x01
#define LSR_OE     0x02
#define LSR_PE     0x04
#define LSR_FE     0x08
#define LSR_BI     0x10
#define LSR_THRE   0x20
#define LSR_TEMT   0x40
#define LSR_ERR    0x80

// MCR bits
#define MCR_DTR    0x01
#define MCR_RTS    0x02
#define MCR_OUT1   0x04
#define MCR_OUT2   0x08
#define MCR_LOOP   0x10

// MSR bits (modem status)
#define MSR_DCTS   0x01
#define MSR_DDSR   0x02
#define MSR_TERI   0x04
#define MSR_DDCD   0x08
#define MSR_CTS    0x10
#define MSR_DSR    0x20
#define MSR_RI     0x40
#define MSR_DCD    0x80

// IIR masks and IDs
#define IIR_INT_PENDING  0x01    // Bit0: 1 = no interrupt pending
#define IIR_ID_MASK      0x0E    // Bits1-3: interrupt ID
#define IIR_ID_RLS       0x06    // Receiver Line Status
#define IIR_ID_RDA       0x04    // Received Data Available
#define IIR_ID_CTI       0x0C    // Character Timeout Indication
#define IIR_ID_THRE      0x02    // Transmitter Holding Register Empty
#define IIR_FIFO_MASK    0xC0    // Bits6-7: FIFO enabled/trigger level

// Aliases for ISR switch-case readability
#define IIR_RLS          IIR_ID_RLS
#define IIR_RDA          IIR_ID_RDA
#define IIR_CTI          IIR_ID_CTI
#define IIR_THRE         IIR_ID_THRE

#ifndef NonPagedPoolNx
#define NonPagedPoolNx NonPagedPool
#endif

// Basic register access helpers (inline)
__forceinline UCHAR UartRead8(PUARTCTRL_DEVEXT ext, ULONG off)
{
    return READ_REGISTER_UCHAR((PUCHAR)ext->MmioBase + off);
}

__forceinline VOID UartWrite8(PUARTCTRL_DEVEXT ext, ULONG off, UCHAR v)
{
    WRITE_REGISTER_UCHAR((PUCHAR)ext->MmioBase + off, v);
}

// Convenience: line status and data
__forceinline UCHAR UartReadLSR(PUARTCTRL_DEVEXT ext)
{
    return UartRead8(ext, UART_LSR);
}

__forceinline UCHAR UartReadRBR(PUARTCTRL_DEVEXT ext)
{
    return UartRead8(ext, UART_RBR);
}

__forceinline VOID UartWriteTHR(PUARTCTRL_DEVEXT ext, UCHAR v)
{
    UartWrite8(ext, UART_THR, v);
}

// Prototypes for higher-level helpers (implemented in uartctrl_hw.c)
VOID UartSetBaud(PUARTCTRL_DEVEXT ext, ULONG inputClockHz, ULONG baud);
VOID UartSetLineControl(PUARTCTRL_DEVEXT ext, UCHAR dataBits, UCHAR stopBits, UCHAR parity);
VOID UartEnableFifo(PUARTCTRL_DEVEXT ext, UCHAR trigger);
VOID UartEnableInterrupts(PUARTCTRL_DEVEXT ext, UCHAR mask);
VOID UartDisableInterrupts(PUARTCTRL_DEVEXT ext);
VOID UartSetModemControl(PUARTCTRL_DEVEXT ext, UCHAR mcr);
UCHAR UartReadLineStatus(PUARTCTRL_DEVEXT ext);
UCHAR UartReadByte(PUARTCTRL_DEVEXT ext);
VOID  UartWriteByte(PUARTCTRL_DEVEXT ext, UCHAR value);
