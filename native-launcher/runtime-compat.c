/* Additional Linux NativeAOT ABI names; the PS5 libc supplies vfscanf and fgetc.
 * Keep these definitions local to this application, not in the vendor toolchain.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

extern void *calloc(size_t count, size_t size);
extern void free(void *pointer);
extern int vfscanf(void *stream, const char *format, va_list args);

#ifndef ATMOSPHERE_COMPAT_TEST
__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;
#endif

void *__sched_cpualloc(size_t count)
{
    const size_t bits = 8 * sizeof(unsigned long);
    if (count > SIZE_MAX - (bits - 1)) return NULL;
    return calloc((count + bits - 1) / bits, sizeof(unsigned long));
}

void __sched_cpufree(void *set) { free(set); }

int __isoc99_fscanf(void *stream, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = vfscanf(stream, format, args);
    va_end(args);
    return result;
}
