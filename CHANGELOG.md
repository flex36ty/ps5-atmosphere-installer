# Changelog

## Unreleased — 2026-10-07

### Internal-storage permission repair

- When an internal copy fails to open `/data/homebrew` or `/data/.atmosphere-smb-staging` with a permission error, Atmosphere now starts a bundled permission helper and retries after confirmation.
- Added `atmosphere_permissions.elf`, launched through etaHEN's local ELF loader on port 9021. Atmosphere does not need to close for this repair.
- The helper grants read/write/traverse permissions (`0777`) only to the selected, allowlisted directory. It does not recursively modify game files, repair custom destinations, or change USB permissions.
- Added symlink rejection, same-device checks, request identifiers, an expiry window, and a bounded confirmation wait.
- Added specific errors for a missing helper, an unavailable ELF loader, a failed repair, or missing confirmation.

### Updating

Close Atmosphere before replacing its backend. Install the matching `atmosphere_backend.prx` in `sce_module/` and `atmosphere_permissions.elf` beside `eboot.bin`. Preserve `atmosphere-state`.

The existing `v2026.10.07` release ZIP predates this change and does not include the permission helper. These changes are currently available in source; console validation and an updated binary release are pending.

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
