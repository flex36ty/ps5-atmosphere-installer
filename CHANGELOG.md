# Changelog

## Unreleased

### Library appearance and metadata

- Added frosted game cards with a more opaque tinted base, soft gradients, and crisp posters; the existing layout and scrolling are preserved.
- Made the selected card more distinct with a stronger highlight and a thicker, continuous rounded selector. The selector follows the selection smoothly and uses a slightly bluer version of the Library accent with a soft glow.
- Added soft, translucent frosted metadata tags with faint color hints to native library cards and the copy popup, preserving the existing layout and navigation.
- Added short content-ID territory labels: US, EUR, JPN, ASIA, and KOR. Missing, malformed, unsupported, or title-mismatched metadata is shown as Unknown; region is not guessed from the title ID or language.
- Bumped the metadata cache version. Rescan the library once to populate regions in existing image entries.
- Backported remains an indicator of detected backport folders. No marker does not prove that a game is unmodified.

### Transfer diagnostics

- Added experimental bounded read-ahead for desktop testing. It is disabled in the native app after a reported console throughput regression; the native app uses the previous synchronous copy path. Resume comparisons and final verification remain enabled.
- Added per-file remote-read, consumer-wait, write, hash, and verification timings in the saved transfer record, plus a best-effort size-capped `transfer-profile.log`. No console speed improvement is claimed yet.

### Validation and availability

- Region parsing and bounded read-ahead safety tests passed, along with FTP and SMB integration checks.
- Native UI rendering, build, signing, and module integrity checks passed. The latest rounded-selector UI was uploaded to the test PS5 and verified by readback SHA-256; on-console confirmation of this final visual adjustment is pending.
- These changes are in source and the test-console build. The existing v2026.10.07.1 release ZIP does not include them.

## v2026.10.07.1 — 2026-10-07

### Internal-storage permission repair

- When an internal copy fails to open `/data/homebrew` or `/data/.atmosphere-smb-staging` with a permission error, Atmosphere now starts a bundled permission helper and retries after confirmation.
- Added `atmosphere_permissions.elf`, launched through etaHEN's local ELF loader on port 9021. Atmosphere does not need to close for this repair.
- The helper grants read/write/traverse permissions (`0777`) only to the selected, allowlisted directory. It does not recursively modify game files, repair custom destinations, or change USB permissions.
- Added symlink rejection, same-device checks, request identifiers, an expiry window, and a bounded confirmation wait.
- Added specific errors for a missing helper, an unavailable ELF loader, a failed repair, or missing confirmation.

### Updating

Close Atmosphere before replacing its backend. Install the matching `atmosphere_backend.prx` in `sce_module/` and `atmosphere_permissions.elf` beside `eboot.bin`. Preserve `atmosphere-state`.

The existing `v2026.10.07` release ZIP predates this change and does not include the permission helper. The updated install ZIP is attached to the v2026.10.07.1 prerelease. Build and regression checks passed; console validation of automatic repair is still pending.

### Validation

- Permission safety tests passed: allowed paths, rejected symlinks and unapproved paths, repeated repair, and preservation of files inside the repaired directory.
- FTP integration tests passed: metadata, folder discovery, pause/resume, SHA-256 verification, and destination contents.
- Native backend and helper builds passed; backend imports resolved and the signed module passed its integrity check.

## v2026.10.07 — 2026-10-07

Initial published source and folder-install release.

- Native controller interface with SMB/FTP server management, game discovery, metadata and cached covers.
- Internal/USB copying, USB-root placement, progress and speed display, pause/resume, and verification.
- Installed-title detection, confirmed deletion through ShadowMountPlus, and rescan requests.
- Atmosphere branding, installation documentation, and a 4K library screenshot.
- Complete `PPSA99005` install ZIP, without saved settings or credentials.

The tested console setup uses firmware 12.70 with etaHEN/kstuff and ShadowMountPlus; other firmware versions remain unverified.
