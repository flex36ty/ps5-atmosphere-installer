/* Diagnostic entry: kernel writes work before libc/NativeAOT initialization. */
extern long write(int, const void *, unsigned long);
extern void _init_env(void *);
extern int atexit(void (*)(void));
extern void _init(void), _fini(void), exit(int);
extern int main(int, char **, char **);
/* App-local marker and return hook for the system C runtime. */
unsigned long Need_sceLibc;
void catchReturnFromMain(int status) { (void)status; }
#define TRACE(s) ((void)write(1, "[Atmosphere startup] " s "\n", sizeof("[Atmosphere startup] " s "\n")-1))
__attribute__((noreturn)) void atmosphere_start(void *parameters, void (*cleanup)(void))
{
    TRACE("entry");
    _init_env(parameters);
    TRACE("libc ready");
    if (cleanup) atexit(cleanup);
    atexit(_fini);
    TRACE("before constructors");
    _init();
    TRACE("before managed main");
    int result = main(*(int *)parameters, (char **)((char *)parameters+8), (char **)0);
    TRACE("managed main returned");
    exit(result);
    __builtin_unreachable();
}

