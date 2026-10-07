"""Build a separate server-only ELF with pre-main tracing; never upload or run it.

Run inside atmosphere-build:0.1 with python3. SDK v0.43 CRT sources must be at
.deps/sdk-trace (https://github.com/ps5-payload-dev/sdk/tree/v0.43/crt).
The receiver IPv4 address is embedded only in the ignored diagnostic artifact.
"""
import argparse
import ipaddress
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("receiver", type=ipaddress.IPv4Address)
parser.add_argument("--port", type=int, default=34178)
parser.add_argument("--launcher-imports-only", action="store_true",
                    help="Trace loading the icon API library, then exit without Atmosphere state or icon writes")
args = parser.parse_args()
if not 1024 <= args.port <= 65535 or not args.receiver.is_private:
    parser.error("Use a private LAN receiver and an unprivileged port")

root = Path.cwd()
out = root / ("build/launcher-import-trace" if args.launcher_imports_only else "build/startup-trace")
out.mkdir(parents=True, exist_ok=True)
for source in (root / ".deps/sdk-trace").glob("*"):
    if source.suffix in (".c", ".h", ".S"):
        shutil.copy2(source, out / source.name)

declarations = """
void atmosphere_trace(const char *);
void atmosphere_trace_number(const char *, long);
void atmosphere_trace_init(void);
void atmosphere_trace_close(void);
"""


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise SystemExit(f"Unexpected source layout: {old!r}")
    return text.replace(old, new, 1)


crt = (out / "crt.c").read_text()
crt = declarations + crt
crt = replace_once(crt, "  if((error=__kernel_init(args))) {",
                   '  atmosphere_trace_init();\n  atmosphere_trace("CRT: kernel init");\n'
                   "  if((error=__kernel_init(args))) {")
for function in ("__klog_init()", "__patch_init()", "__rtld_init()"):
    old = f"  if((error={function})) {{"
    crt = replace_once(crt, old, f'  atmosphere_trace("CRT: {function}");\n' + old)
for old, label in (
    ("  if(!(lib=__rtld_payload_new(__progname))) {", "payload object"),
    ("  if((err=__rtld_lib_open(lib))) {", "resolve imports"),
    ("  if((err=__rtld_lib_init(lib, argc, argv, environ))) {", "constructors"),
    ("  err = main(argc, argv, environ);", "enter main"),
):
    crt = replace_once(crt, old, f'  atmosphere_trace("CRT: {label}");\n' + old)
crt = replace_once(crt, "  err = main(argc, argv, environ);",
                   '  err = main(argc, argv, environ);\n  atmosphere_trace_number("main returned", err);')
crt = replace_once(crt, "  if((err=payload_init(args))) {",
                   '  if((err=payload_init(args))) {\n  atmosphere_trace_number("payload_init failed", err);')
crt = replace_once(crt, "    if((err=payload_run())) {",
                   '    if((err=payload_run())) {\n    atmosphere_trace_number("payload_run failed", err);')
crt = replace_once(crt, "  // terminate payload\n  return payload_terminate();",
                   '  atmosphere_trace("CRT: terminating");\n  atmosphere_trace_close();\n'
                   "  return payload_terminate();")
(out / "crt.c").write_text(crt)

klog = declarations + (out / "klog.c").read_text()
klog = klog.replace("  return (int)__crt_syscall(0x259, 7, buf, 0);",
                    "  atmosphere_trace(buf);\n  return (int)__crt_syscall(0x259, 7, buf, 0);")
(out / "klog.c").write_text(klog)

rtld = declarations + (out / "rtld.c").read_text()
rtld = replace_once(rtld, "    return ctx->open(ctx);",
                    '    atmosphere_trace("CRT: opening module");\n'
                    '    atmosphere_trace(ctx->soname);\n    return ctx->open(ctx);')
rtld = replace_once(rtld, "  return ctx->init(ctx, argc, argv, envp);",
                    '  atmosphere_trace("CRT: initializing module");\n'
                    '  atmosphere_trace(ctx->soname);\n  return ctx->init(ctx, argc, argv, envp);')
(out / "rtld.c").write_text(rtld)

main = declarations + 'int atmosphere_fs_probe(int);\n' + (root / "backend/main.c").read_text()
for old, label in (
    ("    setvbuf(stdout, NULL, _IOLBF, 0);", "stdio"),
    ("    if (atmosphere.port < 1024 || atmosphere.port > 65535 || load_catalog()) {", "catalogue"),
    ("    if (mkdir(atmosphere.state_dir, 0700) != 0 && access(atmosphere.state_dir, F_OK) != 0) {", "state directory"),
    ('    atmosphere.state_fd = open(atmosphere.state_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);', "open state directory"),
    ('    int lock = openat(atmosphere.state_fd, "instance.lock", O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);', "open instance lock"),
    ("    if (flock(lock, LOCK_EX | LOCK_NB)) {", "lock instance"),
    ("    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0 || state_load()) {", "HTTPS and saved state"),
    ("    random_hex(random, 4);", "secure randomness"),
    ("    if (api_start()) {", "HTTP server"),
    ("    pthread_attr_t worker_attr;", "worker thread"),
    ("    while (!stopping)", "ready"),
):
    main = replace_once(main, old, f'    atmosphere_trace("main: {label}");\n' + old)
main = main.replace("        return 1;", '        atmosphere_trace_number("startup errno", errno);\n        return 1;')
main = replace_once(main, '    atmosphere_trace("main: open instance lock");',
                    '    if (atmosphere_fs_probe(atmosphere.state_fd)) return 1;\n'
                    '    atmosphere_trace("main: open instance lock");')
main = '#include <openssl/err.h>\n' + main
main = replace_once(main, "    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0 || state_load()) {", """
    int tls_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    atmosphere_trace_number("curl_global_init", tls_result);
    if (tls_result) {
        unsigned long ssl_error;
        char ssl_text[256];
        while ((ssl_error = ERR_get_error()) != 0) {
            ERR_error_string_n(ssl_error, ssl_text, sizeof ssl_text);
            atmosphere_trace(ssl_text);
        }
    }
    int saved_result = tls_result ? 0 : state_load();
    atmosphere_trace_number("state_load (only attempted when curl succeeds)", saved_result);
    if (tls_result || saved_result) {""")
# This is a short diagnostic, not a second persistent service.
main = replace_once(main, "    while (!stopping)\n        sleep(1);",
                    "    for (int seconds = 0; !stopping && seconds < 20; seconds++)\n        sleep(1);")
if args.launcher_imports_only:
    main = declarations + """
extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilTerminate(void);
static int (*volatile imported_initialize)(void) __attribute__((used)) = sceAppInstUtilInitialize;
static int (*volatile imported_terminate)(void) __attribute__((used)) = sceAppInstUtilTerminate;
int main(void) {
    atmosphere_trace("Launcher dependency probe reached main; no installer called");
    return 0;
}
"""
(out / "atmosphere-main.c").write_text(main)
state = declarations + (root / "backend/state.c").read_text()
state = replace_once(state, '    int fd = openat(atmosphere.state_fd, "state.json", O_RDONLY | O_NOFOLLOW);',
    '    int fd = openat(atmosphere.state_fd, "state.json", O_RDONLY | O_NOFOLLOW);\n'
    '    atmosphere_trace_number("state file fd", fd);\n'
    '    atmosphere_trace_number("state open errno", errno);')
state = '#include <errno.h>\n' + state
state = replace_once(state, "    if (fstat(fd, &st) || st.st_size < 1 || st.st_size > 1024 * 1024) {", """
    int stat_result = fstat(fd, &st);
    atmosphere_trace_number("state fstat", stat_result);
    atmosphere_trace_number("state fstat errno", errno);
    if (!stat_result) {
        atmosphere_trace_number("state size", st.st_size);
        atmosphere_trace_number("state mode", st.st_mode);
    }
    if (stat_result || st.st_size < 1 || st.st_size > 1024 * 1024) {""")
state = replace_once(state, "    cJSON *o = p == n ? cJSON_Parse(s) : NULL;",
    '    atmosphere_trace_number("state bytes read", p);\n'
    '    cJSON *o = p == n ? cJSON_Parse(s) : NULL;\n'
    '    atmosphere_trace_number("state parsed", o != NULL);\n'
    '    if (o) atmosphere_trace_number("state schema", json_int(o, "schema"));')
(out / "atmosphere-state.c").write_text(state)
(out / "boottrace-config.h").write_text(
    f"#define ATMOSPHERE_TRACE_PORT {args.port}\n#define ATMOSPHERE_TRACE_ADDRESS "
    + ", ".join(str(part) for part in args.receiver.packed) + "\n")
shutil.copy2(root / "diagnostics/boottrace.c", out / "boottrace.c")

sdk = Path("/opt/ps5-payload-sdk")
flags = ["-target", "x86_64-sie-ps5", "-ffreestanding", "-fno-builtin", "-nostdlib",
         "-fPIC", "-fno-plt", "-fno-stack-protector", "-fvisibility-nodllstorageclass=default",
         "-Wall", "-Werror", "-fms-extensions", "-Wno-microsoft-anon-tag", "-O1"]
objects = []
for source in sorted(out.glob("*.c")) + sorted(out.glob("*.S")):
    if source.name in ("atmosphere-main.c", "atmosphere-state.c"):
        continue
    obj = source.with_suffix(".o")
    subprocess.run(["clang-19", *flags, "-c", str(source), "-o", str(obj)], check=True)
    objects.append(str(obj))
crt_object = out / "trace-crt1.o"
subprocess.run(["ld.lld-19", "-m", "elf_x86_64", "-r", "-o", str(crt_object), *objects], check=True)
sources = [str(p) for p in (root / "backend").glob("*.c") if p.name not in ("main.c", "state.c")]
artifact = "build/atmosphere_launcher_import_trace.elf" if args.launcher_imports_only else "build/atmosphere_startup_trace.elf"
subprocess.run([str(sdk / "bin/prospero-clang"), "-nostartfiles", "-std=c11", "-O2", "-g",
                "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                "-Wno-unreachable-code-generic-assoc", "-Wno-unused-command-line-argument",
                "-D_FILE_OFFSET_BITS=64",
                "-Ibackend", "-Ilauncher", "-I.deps/cjson", "-Ibuild/generated",
                f"-I{sdk}/target/user/homebrew/include", *sources, ".deps/cjson/cJSON.c",
                str(out / "atmosphere-main.c"), str(out / "atmosphere-state.c"), "diagnostics/fs_probe.c", str(crt_object),
                "-o", artifact,
                "-lcurl", "-lmicrohttpd", "-lssl", "-lcrypto",
                *(["-Wl,--push-state,--no-as-needed", "-lSceIpmi", "-Wl,--pop-state",
                   "-lSceAppInstUtil"] if args.launcher_imports_only else []),
                *[f"-Wl,--wrap={name}" for name in
                  ("openat", "mkdirat", "fstatat", "linkat", "renameat", "unlinkat")]], check=True)
print(f"Built {artifact}; no upload or console execution performed.")
