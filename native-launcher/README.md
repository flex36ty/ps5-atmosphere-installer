# Atmosphere native app

> Historical development notes: the framebuffer build, FAKE34177 title, controls, and PKG instructions below are superseded. See the [current project README](../README.md) for the OpenGL PPSA99005 application and current module build sequence.

Experimental PS5 folder application, title FAKE34177. The UI renders directly to
 the framebuffer and uses the controller and native text-input dialog. It loads
sce_module/atmosphere_backend.prx in its own process and calls its SMB engine
through an in-process JSON bridge. Module startup fills a versioned 48-byte function table; the launcher validates it before starting the worker. It does not use sceKernelDlsym for backend entry points. There is no HTTP listener, browser dialog,
service ELF upload, or port 9021 dependency. Closing the app ends its transfers.
SMB still connects to the configured file server over the network.

Controls: L1/R1 switches Library, SMB Sources and Transfers. D-pad navigates,
X selects, Circle goes back and Options exits. Library Square scans, Triangle
sorts by title/date added, and L2/R2 changes source. The destination dialog's
Square button toggles USB-root placement. Transfers show MB/s, with X for
pause/resume and Square for cancel. SMB Sources supports up to eight sources;
Square adds, X activates and Triangle edits the active source.

Cached PNG/JPEG data covers are decoded locally. Remote cover URLs currently
show a placeholder. The SMB discovery and copy engine is shared with the prior
web build; external-provider browsing is not exposed in the native UI.

Build backend with compile-backend.sh, backend-imports.py, build-backend.ps1.
Build launcher with compile-linux.sh, then build-runtime-test.ps1 -SystemLibc
-Runtime C:/projects/atmosphereps5/PPSA99177/sce_module/libc.prx (adjust the path).
The original loader shim is bundled unchanged; C imports use libSceLibcInternal.
The entropy adapter implements only NativeAOT's random-byte dependency using
sceRandomGetRandomNumber, including chunk limits and failure propagation.

Current artifact: ../build/Atmosphere-native.zip. This is a folder build for the
existing ShadowMountPlus setup, not a validated installable PKG. The old
package.ps1 flow is superseded and must not be used for this native build.

Host validation: native screen preview rendered, bridge revision/action tests
and entropy boundary/failure tests pass with sanitizers, final link has zero
unresolved symbols, and both signed containers pass their integrity checks.
Console validation of this native UI build remains pending. No all-firmware
compatibility claim is made; the test console is on 12.70.

## Historical diagnostics (superseded builds)
**Current console result:** the first PKG installed through etaHEN but was rejected
at launch on the user's 12.70 console with "the data is corrupted" and no numeric
error code. The cause is not yet identified. Host-side signing and extraction checks
passed, but do not establish console acceptance. Kernel-log capture identified package-image validation failure 0x80f00800. Further
diagnostics use `capture-klog.ps1` on port 3232.

The package requires etaHEN/kstuff with working native application support and a
local ELF loader. It does not jailbreak the console. No console launch or firmware
matrix has been validated. The user-supplied `libc.prx` reports firmware 12.00, which
is the minimum retained by this build. 12.70 is the intended first test; 13.60 is
unverified. Supporting versions below 12.00 needs a compatible older runtime and
testing, not just a lower metadata version.

## Build inputs

- SharpProspero commit `5dfdb4b0b166e83dc9ae275001afb695f12c7174` under
  `../../packaging-tools/SharpProspero` (GPL-3.0-or-later).
- .NET SDK 10.0.401 / NativeAOT 10.0.12 on Linux, .NET 10 on Windows for packaging.
- LibProsperoPKG commit `748eabf1b7d17819528cabf367d8e27109d8fce3` under
  `../../packaging-tools/LibProsperoPKG`. The packager project references this source
  instead of its bundled DLL. A local `BuildU2c` bounds fix handles the format's
  trailing sentinel group when the block count is an exact multiple of eight.
  Eleven boundary and explicit-count serialization regression cases live in
  `../../packaging-tools/package-checks`. The packager also prints full exception
  details when assembly fails.
  The verification reader has two additional fixes: distinguish zero alignment
  padding from extra file-offset entries, and consume the single zero-fill marker
  spanning the gap before metadata. The regression cases also exercise automatic
  padded-layout parsing. These reader fixes do not change the generated package.
- The existing ps5-payload-sdk toolchain for `make payload-core`.
- The user's PS5 runtime at `sce_module/libc.prx` (excluded from source control).
- `sce_sys/icon0.png` rendered from Atmosphere's `public/atmosphere.svg`.

## Build sequence

1. Build `../build/atmosphere_core.elf` using `make payload-core`. This omits the legacy
   deeplink registration so the package does not create a second home-screen icon.
2. Restore the launcher's Linux NativeAOT dependencies. This workspace keeps a local
   feed in `../../packaging-tools/nuget-feed` because WSL networking is unavailable.
3. Run `bash compile-linux.sh` in WSL. Override `ATMOSPHERE_DOTNET_ROOT` and
   `ATMOSPHERE_NUGET_CACHE` if needed. NativeAOT emits the object before the expected
   Linux link failure on Sony symbols; the script requires a freshly generated object.
   It also compiles and sanitizer-tests the additional CPU-set allocation and
   scanning compatibility functions in `runtime-compat.c`. The extra libc imports
   (`fgetc`, `vfscanf`) are checked against the supplied module's actual exports.
4. Build SharpProspero.Bindings.Generator (Debug) and SharpProspero.Packager (Release).
5. Run `./package.ps1` from PowerShell. It links, checks bundled modules, prepares
   icon textures, validates metadata, matches the firmware requirement to the actual
   modules, wraps the executable, and packages the result. It does not install it.

Output: `../build/Atmosphere.pkg`, plus a SHA-256 manifest. Timestamped package
staging directories remain under `../build` for inspection.

The completed package passed the structural acceptance checks and metadata-signature
validation. Its `eboot.bin` and `libc.prx` were decrypted/extracted and SHA-256 compared
to the build inputs; the icon PNG, DDS, and param.json CNT entries also match byte
for byte. This verifies packaging integrity, not console execution.

## Console validation still required

Install with etaHEN active and port 9021 enabled. Test a cold backend start, an
already-running backend, browser close/reopen, startup retry with the loader
disabled, and an SMB transfer to each destination. Confirm closing and reopening
the launcher preserves the active transfer. The host build checks cannot validate
PS5 browser dialog behavior or firmware-specific service access.

## Runtime folder test (2026-10-05)

ShadowMountPlus 1.7 rejects ATMO-prefixed IDs; FAKE34177 registered successfully.
The folder launched but failed with PRX_SCE_MODULE_LOAD_ERROR. Both supplied libc
versions lack the three FreeBSD __std*p exports the linker originally imported.
The local CompatEmitter now creates empty standard stream slots; runtime-streams.c
initializes them through fdopen (with /dev/null fallback) in a native constructor.
The constructor runs after _init_env and before main. Its normal, fallback and
failure paths passed host ASan/UBSan tests. The relinked application has 52 libc
imports, all present in the PS5VR runtime; this does not prove console execution.

The separate build/Atmosphere-runtime-test.zip uses PS5VR's supplied libc.prx.
Original files remain in the earlier builds and console-before-runtime-test-eboot.bin.
Rebuild with build-runtime-test.ps1 after compiling runtime-streams.c to
obj/runtime-streams.o and rebuilding the patched SharpProspero binding generator.

The first runtime test loaded libc but trapped at image base 0x400000.
DynamicWriter emitted zero-initialized lazy GOT slots and incomplete PLT stubs.
LazyBinding.patch adds PLT0, per-slot index/branch tails and initial slot addresses.
check-lazy-binding.py verifies all 179 stubs and also passes on PS5VR's reference.
The diagnostic entry (build-runtime-test.ps1 -TraceStartup) is currently deployed.
Its console result remains pending; the module-load fix alone did not yield a working app.


The PS5VR libc lists exports but indexes its SysV hash table by short numeric names;
0/51 required exports were reachable using canonical names. The original user-supplied
runtime resolves 51/51 using the same hash algorithm. The test build now uses that
original runtime plus the standard-stream and lazy-binding fixes. Merely listing
exports was insufficient validation; audit-symbol-hashes.py checks actual lookup.
The restored runtime requires 12.00; its console retest is pending.


The original runtime was still rejected during module loading. The next test uses
a COPY of PS5VR libc whose hash buckets/chains were recomputed with canonical
names via audit-symbol-hashes.py --repair-copy. The repair asserts that no ELF bytes
outside the hash table changed, and verifies all 2668 entries and 51 required
exports before re-signing. Both uploaded files were read back and hash-verified.
Rebuild with build-runtime-test.ps1 -TraceStartup -Runtime <rehashed.prx>.
The supplied PPSA99177 files are unchanged. Console retest pending.


### System C runtime diagnostic build (2026-10-05)
The rehashed PS5VR runtime test still crashed. Its required functions (including
fdopen, fopen, malloc and abort) share a return-zero placeholder. Symbol hash
reachability was insufficient to establish a functional runtime.

The next build uses `build-runtime-test.ps1 -SystemLibc -Runtime ../../PPSA99177/sce_module/libc.prx`
(use an absolute runtime path when invoking from a different directory).
The new optional `--system-libc` linker flag routes the C catalog to
libSceLibcInternal, module version 1/library version 1, matching the working
PS5VR executable. Additional stdio imports use the same library. The diagnostic
entry supplies app-local Need_sceLibc and catchReturnFromMain hooks. The original
PS5VR loader shim is packaged unchanged; Atmosphere imports no functions from it.

Validation: tool build passed; link has zero unresolved symbols; all 178 lazy
binding slots pass structural checks; SELF digest check passes; extracted ELF
has no libc imports. Uploaded eboot.bin and libc.prx were downloaded again and
matched SHA-256. Hardware launch is pending, so this is not a verified fix.
Klog capture: build/system-libc-klog.txt.


### Native module load diagnostic
The first in-process backend build returned sceKernelLoadStartModule error
0x80020016 (EINVAL) on FW12.70. The cause has not yet been confirmed.
The next build sets the generated PRX initialization/finalization return value
explicitly to zero and supplies a module-result output pointer. Its screen shows
both loader and initializer status; 0x80000000 means the output was not changed.
Both files were uploaded and verified by SHA-256 readback. Console retest pending.
Log: build/module-start-klog.txt.



### Native interface handoff (2026-10-05)
Console startup reached module load but dlsym returned 0x80020003 for atmosphere_backend_run. All five exports passed host hash-table checks, so the exact runtime lookup rejection remains undetermined. The next build uses an explicit --module-start callback (DT_INIT) to return the interface through the loader's argument buffer. Host tests cover wrong size/version, null arguments, and all five returned functions. Console retest pending.


Options now requests SMB shutdown, waits up to 15 seconds for the worker, verifies FAKE34177 via the running app title, and calls the PS5 four-argument sceSystemServiceKillApp API with (-1,0,0). It reports failures in the native UI. The prior ProcessExit sentinel path is no longer used by the Options action. Console exit validation pending.


### Backend startup diagnostics
The console now reaches backend execution but returns status 1. The diagnostic build publishes a startup error through the native bridge before accessing SMB snapshots, including directory/lock failures, TLS initialization, saved state, and worker creation. Host sanitizer tests verify error delivery before SMB initialization. The underlying console failure remains pending the next launch report.


### etaHEN storage access
Native startup uses /app0/atmosphere-state for settings, cached metadata and transfer state. It does not request etaHEN IPC or require port 9028. Native payload auto-start integration is disabled. Destination enumeration still requires writable internal/USB storage; the copy worker checks the selected filesystem and reports access errors separately. Existing /data/atmosphere settings are not automatically migrated. This requires a writable folder installation; console verification is pending.

Native filesystem compatibility (`native-files.c`) replaces payload-only direct syscalls with exported filesystem operations. It tracks directory paths and checks descriptor and path device/inode identity before resolving relative operations. Host sanitizer tests cover creation, rename publication, directory duplication, symlink refusal and stale descriptors. Path resolution cannot offer kernel-atomic protection against concurrent external directory renames; do not modify destination directories externally during a transfer. This follows the native-title compatibility approach documented by [PS5_PayloadSDK](https://github.com/mihawk-99/PS5_PayloadSDK/blob/main/platform/src/directory.c), using a separate implementation. Legacy hard-link publication and generic payload syscall helpers return unsupported errors. The native build excludes SDK `syscalls.o` and rejects direct syscall instructions in the resulting backend. No fabricated storage capacity values are supplied.


The console rejected lstat on /app0/atmosphere-state with EPERM. Native directory identity checks now use open(O_DIRECTORY | O_NOFOLLOW) plus fstat; other no-follow metadata checks use open(O_NONBLOCK | O_NOFOLLOW) plus fstat. Symlinks are refused with ELOOP rather than returning link metadata. Internal and USB access outside the app mount remains dependent on the console's sandbox configuration.
