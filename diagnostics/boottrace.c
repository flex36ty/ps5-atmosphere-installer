/* Diagnostic-only UDP trace. No libc, heap, files, or console configuration writes. */
#include "syscall.h"
#include "boottrace-config.h"

static long trace_fd = -1;
static const unsigned char destination[16] = {
    16, 2, ATMOSPHERE_TRACE_PORT >> 8, ATMOSPHERE_TRACE_PORT & 255,
    ATMOSPHERE_TRACE_ADDRESS, 0, 0, 0, 0, 0, 0, 0, 0
};

void atmosphere_trace(const char *message) {
    unsigned long length = 0;
    while (message[length] && length < 768)
        length++;
    if (trace_fd >= 0)
        __crt_syscall(133, trace_fd, message, length, 0, destination, sizeof destination);
}

void atmosphere_trace_number(const char *label, long value) {
    char text[192], digits[24];
    unsigned long used = 0, count = 0;
    while (*label && used < 140)
        text[used++] = *label++;
    text[used++] = '=';
    unsigned long magnitude = (unsigned long)value;
    if (value < 0) {
        text[used++] = '-';
        magnitude = 0UL - magnitude;
    }
    do {
        digits[count++] = '0' + magnitude % 10;
        magnitude /= 10;
    } while (magnitude);
    while (count)
        text[used++] = digits[--count];
    text[used] = 0;
    atmosphere_trace(text);
}

void atmosphere_trace_init(void) {
    trace_fd = __crt_syscall(97, 2, 2, 0);
    atmosphere_trace("CRT: syscall bridge ready");
    atmosphere_trace_number("pid", __crt_syscall(SYS_getpid));
}

void atmosphere_trace_close(void) {
    if (trace_fd >= 0)
        __crt_syscall(SYS_close, trace_fd);
    trace_fd = -1;
}
