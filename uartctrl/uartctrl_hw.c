/* -----------------------------------------------------------------------
   uartctrl_hw.c – hardware access helpers for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl_ext.h"
#include "uartctrl_hw.h"

/* Configure baud rate: divisor = inputClock / (16 * baud) */
VOID
UartSetBaud(PUARTCTRL_DEVEXT ext, ULONG inputClockHz, ULONG baud)
{
    USHORT divisor = (USHORT)(inputClockHz / (16 * baud));
    UCHAR lcr = UartRead8(ext, UART_LCR);

    /* Enable divisor latch access */
    UartWrite8(ext, UART_LCR, lcr | LCR_DLAB);
    UartWrite8(ext, UART_DLL, (UCHAR)(divisor & 0xFF));
    UartWrite8(ext, UART_DLM, (UCHAR)((divisor >> 8) & 0xFF));

    /* Restore LCR */
    UartWrite8(ext, UART_LCR, lcr & ~LCR_DLAB);
}

/* Configure line control: data bits, stop bits, parity */
VOID
UartSetLineControl(PUARTCTRL_DEVEXT ext, UCHAR dataBits, UCHAR stopBits, UCHAR parity)
{
    UCHAR lcr = 0;

    switch (dataBits) {
    case 5: lcr |= LCR_WLS_5; break;
    case 6: lcr |= LCR_WLS_6; break;
    case 7: lcr |= LCR_WLS_7; break;
    default: lcr |= LCR_WLS_8; break;
    }

    if (stopBits == 2) lcr |= LCR_STB;
    if (parity) {
        lcr |= LCR_PEN;
        if (parity == 2) lcr |= LCR_EPS; /* even parity */
    }

    UartWrite8(ext, UART_LCR, lcr);
}

/* Enable FIFO and set RX trigger level */
VOID
UartEnableFifo(PUARTCTRL_DEVEXT ext, UCHAR trigger)
{
    UartWrite8(ext, UART_FCR, FCR_FIFO_EN | FCR_RXRST | FCR_TXRST | trigger);
}

/* Enable interrupts with mask (IER bits) */
VOID
UartEnableInterrupts(PUARTCTRL_DEVEXT ext, UCHAR mask)
{
    UartWrite8(ext, UART_IER, mask);
}

/* Disable all interrupts */
VOID
UartDisableInterrupts(PUARTCTRL_DEVEXT ext)
{
    UartWrite8(ext, UART_IER, 0);
}

/* Set modem control (RTS/CTS, OUT2 for IRQ enable) */
VOID
UartSetModemControl(PUARTCTRL_DEVEXT ext, UCHAR mcr)
{
    UartWrite8(ext, UART_MCR, mcr);
}

/* Read line status register */
UCHAR
UartReadLineStatus(PUARTCTRL_DEVEXT ext)
{
    return UartRead8(ext, UART_LSR);
}

/* Read received byte */
UCHAR
UartReadByte(PUARTCTRL_DEVEXT ext)
{
    return UartRead8(ext, UART_RBR);
}

/* Write transmit byte */
VOID
UartWriteByte(PUARTCTRL_DEVEXT ext, UCHAR value)
{
    UartWrite8(ext, UART_THR, value);
}
