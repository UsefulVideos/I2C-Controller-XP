#include "i2cctrl_log.h"

/* -----------------------------------------------------------------------
 * kernel logger with printf-style formatting + timestamp prefix
 * ----------------------------------------------------------------------- */
VOID
I2cCtrl_Log(
    PCSTR Format,
    ...
    )
{
    CHAR  buffer[512];
    CHAR  final[600];
    va_list args;
    NTSTATUS status;

    UNICODE_STRING      path;
    OBJECT_ATTRIBUTES   oa;
    IO_STATUS_BLOCK     iosb;
    HANDLE              hFile;

    LARGE_INTEGER       sysTime, localTime;
    TIME_FIELDS         tf;

    PAGED_CODE();

    //
    // Hard safety guards: prevent use-after-free crashes
    //
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    if (Format == NULL) {
        return;
    }

    //
    // SAFE pointer formatting:
    // Convert all %p to 0x%I64X BEFORE calling VPrintf.
    // This prevents the CRT from dereferencing freed pointers.
    //
    {
        CHAR safeFmt[256];
        SIZE_T i = 0, j = 0;

        while (Format[i] != '\0' && j < sizeof(safeFmt) - 1) {
            if (Format[i] == '%' && Format[i+1] == 'p') {
                safeFmt[j++] = '0';
                safeFmt[j++] = 'x';
                safeFmt[j++] = '%';
                safeFmt[j++] = 'I';
                safeFmt[j++] = '6';
                safeFmt[j++] = '4';
                safeFmt[j++] = 'X';
                i += 2;
                continue;
            }
            safeFmt[j++] = Format[i++];
        }
        safeFmt[j] = '\0';

        va_start(args, Format);
        status = RtlStringCbVPrintfA(buffer, sizeof(buffer), safeFmt, args);
        va_end(args);

        if (!NT_SUCCESS(status)) {
            return;
        }
    }

    /* Get local time */
    KeQuerySystemTime(&sysTime);
    ExSystemTimeToLocalTime(&sysTime, &localTime);
    RtlTimeToTimeFields(&localTime, &tf);

    /* Format timestamp prefix: [DD/MM/YYYY, HH:MM AM/PM] */
    {
        CHAR ts[64];
        ULONG hour = tf.Hour;
        BOOLEAN pm = FALSE;

        if (hour == 0) {
            hour = 12;
        } else if (hour == 12) {
            pm = TRUE;
        } else if (hour > 12) {
            hour -= 12;
            pm = TRUE;
        }

        RtlStringCbPrintfA(
            ts,
            sizeof(ts),
            "[%02u/%02u/%04u, %02u:%02u %s] ",
            tf.Day,
            tf.Month,
            tf.Year,
            hour,
            tf.Minute,
            pm ? "PM" : "AM"
        );

        RtlStringCbPrintfA(
            final,
            sizeof(final),
            "%s%s",
            ts,
            buffer
        );
    }

    /* Open log file */
    RtlInitUnicodeString(&path, L"\\SystemRoot\\System32\\i2cctrl.log");

    InitializeObjectAttributes(
        &oa,
        &path,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL
    );

    status = ZwCreateFile(
                 &hFile,
                 FILE_APPEND_DATA | SYNCHRONIZE,
                 &oa,
                 &iosb,
                 NULL,
                 FILE_ATTRIBUTE_NORMAL,
                 0,
                 FILE_OPEN_IF,
                 FILE_SYNCHRONOUS_IO_NONALERT,
                 NULL,
                 0
             );

    if (!NT_SUCCESS(status)) {
        return;
    }

    /* Write timestamped line */
    ZwWriteFile(
        hFile,
        NULL,
        NULL,
        NULL,
        &iosb,
        final,
        (ULONG)strlen(final),
        NULL,
        NULL
    );

    ZwClose(hFile);

    //
    // Mirror to ETW/WPP without the timestamp prefix.
    //
    TraceEvents(
        TRACE_LEVEL_INFORMATION,
        TRACE_FLAG_BUS,
        "%s",
        buffer
    );
}
