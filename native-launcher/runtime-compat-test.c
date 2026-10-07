#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern void *__sched_cpualloc(size_t);
extern void __sched_cpufree(void *);

int main(void)
{
    for (size_t n = 1; n <= 1025; ++n) {
        unsigned long *set = __sched_cpualloc(n);
        assert(set);
        size_t words = (n + 63) / 64;
        for (size_t i = 0; i < words; ++i) { assert(set[i] == 0); set[i] = ~0UL; }
        __sched_cpufree(set);
    }
    assert(__sched_cpualloc(SIZE_MAX) == NULL);
    __sched_cpufree(NULL);
    FILE *stream = tmpfile();
    assert(stream);
    fputs("1234 -27 native 0x1.8p+1", stream);
    rewind(stream);
    int a = 0, b = 0;
    char text[16];
    float c = 0;
    assert(fscanf(stream, "%d %d %15s %a", &a, &b, text, &c) == 4);
    assert(a == 1234 && b == -27 && c == 3.0f);
    assert(fscanf(stream, "%d", &a) == EOF);
    fclose(stream);
    puts("Native runtime compatibility tests passed.");
    return 0;
}
