#include <assert.h>
#include <setjmp.h>
#include <string.h>
#define ATMOSPHERE_STREAM_TEST
#define stdin test_stdin
#define stdout test_stdout
#define stderr test_stderr
#define fdopen test_fdopen
#define fopen test_fopen
#define abort test_abort
#include "runtime-streams.c"
void *test_stdin, *test_stdout, *test_stderr;
static int tokens[3], fail_descriptor=-1, fail_fallback, calls;
static jmp_buf failure;
void *test_fdopen(int fd, const char *mode) {
    assert(fd >= 0 && fd < 3);
    assert(strcmp(mode, fd == 0 ? "r" : "w") == 0);
    calls++;
    return fd == fail_descriptor ? 0 : &tokens[fd];
}
void *test_fopen(const char *path, const char *mode) {
    assert(strcmp(path,"/dev/null")==0);
    assert(strcmp(mode,fail_descriptor==0?"r":"w")==0);
    return fail_fallback ? 0 : &tokens[fail_descriptor];
}
void test_abort(void) { longjmp(failure,1); }
int main(void) {
    for (fail_descriptor=-1; fail_descriptor<3; fail_descriptor++) {
        calls=0; atmosphere_init_streams();
        assert(calls==3 && test_stdin==&tokens[0] && test_stdout==&tokens[1] && test_stderr==&tokens[2]);
    }
    fail_descriptor=1; fail_fallback=1;
    if (!setjmp(failure)) { atmosphere_init_streams(); assert(0); }
    return 0;
}
