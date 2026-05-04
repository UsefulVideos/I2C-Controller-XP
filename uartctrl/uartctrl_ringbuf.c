/* -----------------------------------------------------------------------
   uartctrl_ringbuf.c – ring buffer helpers for UART controller
   ----------------------------------------------------------------------- */

#include <ntddk.h>
#include "uartctrl_ext.h"

/* -----------------------------------------------------------------------
   Put a byte into the ring buffer
   Returns TRUE if successful, FALSE if buffer full
   ----------------------------------------------------------------------- */
BOOLEAN
RingPut(PUCHAR buf, ULONG size,
        volatile ULONG* head, volatile ULONG* tail,
        UCHAR value)
{
    ULONG next = (*head + 1) % size;
    if (next == *tail) {
        // buffer full
        return FALSE;
    }
    buf[*head] = value;
    *head = next;
    return TRUE;
}

/* -----------------------------------------------------------------------
   Get a byte from the ring buffer
   Returns TRUE if successful, FALSE if buffer empty
   ----------------------------------------------------------------------- */
BOOLEAN
RingGet(PUCHAR buf, ULONG size,
        volatile ULONG* head, volatile ULONG* tail,
        UCHAR* value)
{
    if (*tail == *head) {
        // buffer empty
        return FALSE;
    }
    *value = buf[*tail];
    *tail = (*tail + 1) % size;
    return TRUE;
}

/* -----------------------------------------------------------------------
   Count available bytes in the ring buffer
   ----------------------------------------------------------------------- */
ULONG
RingAvail(PUCHAR buf, ULONG size,
          volatile ULONG head, volatile ULONG tail)
{
    UNREFERENCED_PARAMETER(buf);
    if (head >= tail) {
        return head - tail;
    } else {
        return size - (tail - head);
    }
}

/* -----------------------------------------------------------------------
   Count free space in the ring buffer
   ----------------------------------------------------------------------- */
ULONG
RingFree(PUCHAR buf, ULONG size,
         volatile ULONG head, volatile ULONG tail)
{
    UNREFERENCED_PARAMETER(buf);
    return size - 1 - RingAvail(buf, size, head, tail);
}

/* -----------------------------------------------------------------------
   Reset ring buffer indices
   ----------------------------------------------------------------------- */
VOID
RingReset(volatile ULONG* head, volatile ULONG* tail)
{
    *head = 0;
    *tail = 0;
}
