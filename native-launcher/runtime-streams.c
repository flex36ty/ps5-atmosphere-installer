/* Initialize the linker's standard stream slots before NativeAOT startup. */
extern void *stdin, *stdout, *stderr;
extern void *fdopen(int descriptor, const char *mode);
extern void *fopen(const char *path, const char *mode);
extern void abort(void);
static void *open_stream(int descriptor, const char *mode)
{
    void *stream = fdopen(descriptor, mode);
    if (!stream) stream = fopen("/dev/null", mode);
    if (!stream) abort();
    return stream;
}
#ifndef ATMOSPHERE_STREAM_TEST
__attribute__((constructor))
#endif
static void atmosphere_init_streams(void)
{
    stdin = open_stream(0, "r");
    stdout = open_stream(1, "w");
    stderr = open_stream(2, "w");
}
