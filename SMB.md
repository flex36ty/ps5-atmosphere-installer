# SMB library fork

This fork of Atmosphere 0.4.1 adds an **SMB** tab (the initial view), using
libsmb2 for SMB2/3. The original Atmosphere interface, controller navigation, local
pairing and storage discovery are retained. SMB operations have their own
single-copy worker; HTTP downloads and ShadowMount storage operations cannot
write concurrently with an SMB copy.

## Use

Use **SMB source** to switch between up to eight saved shares and **Add source**
to configure another. Each source retains its name, connection settings,
password preference and cached games. Session-only passwords survive switching
but are not saved to disk. Existing single-source settings migrate automatically.
The library shows the selected source; scanning and copying operate on that
source. Switching is disabled during active operations. Copy recovery remains
one global job; return to its original share before resuming.

1. Pair the browser with Atmosphere, then open **SMB → Connection settings**.
2. Enter the server IP/hostname, share name, optional folder, and credentials.
   A custom port can be written as `server:port`. Leave username/password empty
   for a server configured to allow guest access.
   Guest mode negotiates SMB2.1; authenticated connections negotiate SMB2/3.
3. Set **Destination folder on selected drive** to a relative folder scanned
   by ShadowMountPlus. The default is `homebrew`: `/data/homebrew` on internal
   storage, or `/mnt/usbN/homebrew` on USB. Nested paths are supported; absolute
   paths, parent traversal and symlinked destination components are rejected.
4. Choose **Save and scan**. Select internal/external storage, then **Copy game**.
5. Use **Pause**, **Resume**, or **Cancel (keep partial)** in the SMB progress panel.

When selecting an external drive in a game's details, **USB location** offers
the configured game folder or **USB root**. Root placement preserves the game's
filename/folder directly under `/mnt/usbN/`. The choice is saved in the copy job
for resume; staging, verification and overwrite protection apply to both options.

The server/share is read-only from Atmosphere's perspective. Atmosphere does not mount,
install, decrypt or convert games. ShadowMountPlus handles detection after
publication into the chosen destination.

## Discovery and artwork

- Game folders are identified by `sce_sys/param.json` or `eboot.bin`.
- PS5 titles/title IDs come from `sce_sys/param.json`; PNG art comes from
  `sce_sys/icon0.png`. Unidentified titles use the folder name and a placeholder.
- `.ffpfsc`, `.ffpfs`, `.exfat` and `.img` filenames are recognized. Extension
  detection does not validate the internal image format.
- Images are transferred byte-for-byte. The scanner reads embedded `param.json`
  and `icon0.png` from exFAT and unsigned contiguous PFS/PFSC images, including
  compressed blocks and nested wrappers. It fetches only metadata ranges over SMB.
  Signed/encrypted PFS and unsupported inner filesystems fall back to filenames.
  Optional sidecars are supported: `Game.exfat.json` with
  `{"title":"Game title","titleId":"PPSA12345"}` and `Game.exfat.png`.
- No online title/cover matching is implemented.
- Scan results are cached and restored after restart. **Scan share** refreshes
  them. Scan limits: 1,024 games, 10,000 visited folders and 12 nested levels.
  Image metadata is reused when path, size, timestamp and reader version match.
  PNGs are limited to 4 MiB each and a 64 MiB total artwork budget. Each image
  parser is bounded to 64 MiB of reads, 8,192 read calls and a 45-second budget
  checked between reads. Directory reads are limited to 4 MiB.

## Copy integrity and recovery

Transfers pipeline four reads of up to 1 MiB each, respecting the server's
maximum read size and filling short reads before writing. Storage identity and
free-space checks run once per second during writes, with write errors checked
on every write and destination identity checked again before publication.
Progress updates are throttled to 10 Hz; verification uses larger local reads.

A manifest freezes source file paths, sizes and timestamps before copying.
Game folders preserve their hierarchy, including empty directories. Copies
support files larger than 4 GiB using 64-bit offsets. Manifest limits are
100,000 entries and 32 nested directory levels.

Incomplete files live in `<drive>/.atmosphere-smb-staging/<copy-id>/content`, outside
the configured game destination. Do not configure ShadowMountPlus to scan that
staging directory. Completed data is renamed into the selected game folder only
after every file passes SHA-256 verification against bytes read from SMB.
Existing destination names are never intentionally overwritten.

Resume compares all existing partial bytes with the source, checks timestamps
and sizes, then writes the remainder. This favors correctness over instantaneous
resume: already-copied bytes must be read again. A disconnected destination,
changed source, mismatching partial file or full disk produces an error and
keeps partial data. Reconnect the original drive/share and resume. Interruptions
become paused after restart; session-only passwords must be reentered first.

Only one SMB copy and one resume entry are retained. Starting another copy
replaces that entry and leaves the prior partial files in staging; clean those
up manually if no longer needed. There is no multi-game SMB queue or automatic
retry yet. Failed scans retain the previous successful library.

Passwords stay in memory by default. **Remember password** opts into storage
in the console's `smb-state.json` (mode 0600, not encrypted). Passwords never
appear in API responses or source URLs. SMB endpoints require Atmosphere pairing.
Atmosphere's existing phone interface uses local HTTP; use it on a trusted LAN.

## Build and test

Follow `BUILDING.md` for the PS5 toolchain. `tools/build-deps.sh` now builds the
pinned libsmb2 commit `51c5910da240a3b60bc4ad1b472185c2815628d4` with Kerberos,
GSSAPI and DCE/RPC disabled. The payload links its static library. The curl SMB
backend remains disabled deliberately: all SMB traffic uses libsmb2.
Zlib 1.3.1 is also built and linked for PFSC decompression. The native metadata
reader uses the formats documented by PSBrew/MkPFS commit
`1d5df56ae29390900e888fa12e5f9856aa797adc` as its reference.

For Linux host testing, install libmicrohttpd/OpenSSL development libraries,
libcurl 7.85 or newer, and that same libsmb2 build. Use `CPPFLAGS` and `LDFLAGS`
to point `make host` at non-system headers/libraries; `LDLIBS` can supply static
curl's extra dependencies. The frontend builds with `npm ci`,
`npm run typecheck`, and `npx vite build`; run `python3 tools/embed.py` and
`python3 tools/embed-launcher.py` before `make host`.

`sudo python3 tests/smb_integration.py` runs against a temporary loopback-only
Samba share, using synthetic files and disposable storage. Samba must be
installed. It tests discovery, metadata, folder/image copying, overwrite
protection, pause/restart/resume, source changes, partial corruption and
credential handling. No PS5 or existing share is contacted by these tests.

`tests/test_image_metadata.py` exercises the native `tools/image-probe.c`
reader with synthetic fragmented/contiguous exFAT, compressed/uncompressed
PFS, nested images, legacy directory padding and malformed inputs. All 16
cases passed under AddressSanitizer and UndefinedBehaviorSanitizer. Read-only
testing on the configured real share extracted titles and icons from all 25
images (23 FFPFSC and two exFAT); no curated artwork was needed.

Validation completed in this workspace: TypeScript check, production Vite build,
Linux host compilation, browser inspection of the settings screen, and all
integration scenarios above, including authenticated SMB3, rejection of bad
passwords, custom nested destination paths and lightweight status polling.

PS5 cross-compilation and on-console SMB/USB testing are separate from host
validation. Do not treat the host executable as a PS5 payload. Atmosphere's original
updater targets upstream releases, which do not contain these fork changes.
