# Changelog

## v2026.10.09 — 2026-10-09

- Move the native app's destination folder choice from Server settings to the copy popup. Square cycles through ShadowMountPlus scan folders on the selected storage device and shows the full path.
- Read custom `scanpath` entries from `/data/shadowmount/config.ini` on startup and scans; use ShadowMountPlus defaults when none are available. Internal storage root and ShadowMount-managed mounts are not copy targets.
- Remember the last accepted copy folder per drive. Preserve saved custom server folders, legacy API requests and paused jobs; reject new destination choices outside the listed folders.
- Destination parsing and native UI tests passed, including saved drive preferences and source-specific legacy folders. Rendered popup and settings screens were checked. Both signed modules were uploaded to the test PS5 and verified by SHA-256 readback; on-console selection/copy confirmation is pending.

## v2026.10.08.1 — 2026-10-08

- Added WebDAV HTTP/HTTPS sources, Basic authentication, bounded XML directory parsing, metadata and cover reads, recursive folder copying, and verified byte-range transfers through the existing copy/resume workflow.
- Added WebDAV protocol choices to native and browser server settings. HTTPS retains certificate verification; redirects and invalid range responses are rejected.
- Scan all configured active servers once when the native app starts, retaining cached games while refreshing and waiting for an idle backend.
- Removed the unused pairing-code startup notification and log from the native app. Browser/API pairing remains protected.
- Corrected the password keyboard to use Basic Latin with password masking. Keyboard errors remain visible instead of being overwritten by status updates.
- WebDAV local integration and malformed-listing/range tests passed; authenticated HEAD and beginning/middle/end reads succeeded on the test Apache server. PS5 end-to-end WebDAV validation is pending.
- Native builds, signing, and integrity checks passed. The WebDAV modules and subsequent password-keyboard fix were uploaded to the test PS5 and verified by SHA-256 readback; console confirmation of password entry is pending.
- Includes the complete matching folder build and the Yxml MIT license. Close Atmosphere before updating and preserve `atmosphere-state`. Released as a release; no broader firmware compatibility claim is made.

## v2026.10.08 — 2026-10-08

### Servers and unified library

- Keep multiple SMB and FTP servers active together and browse their games in one library. Refresh scans all active servers sequentially; transfers use the selected game's source.
- Added a full-width `Server:` tag to each card and server details in the copy popup.
- Added L2 server duplication with confirmation. The copy receives a new ID, starts inactive, and copies connection settings and login preferences without duplicating the game cache.
- Fixed Triangle Edit opening the last active server instead of the highlighted entry. Saves now target the explicit server ID, including inactive entries, without changing another server's settings.
- Retained recursive game-folder discovery/copying alongside exFAT and FFPFSC images.

### Cards and navigation

- Selected cards ease forward with a subtle enlargement, lift, and deeper shadow; the rounded selector follows the card.
- Overflowing text keeps its font size, scrolls left, pauses at the end for two seconds, resets instantly, and rests for two seconds before repeating. Library card text scrolls when highlighted.
- Moved the format badge onto the poster's top-left corner with a solid background. Moved size up and gave server information its own bottom row.
- Cards show `Installed` without the device name; the opened game popup retains the installed location.
- Added `Not Backported` when no backport-folder marker is detected. This label is a detection result, not proof that the game is unmodified or compatible with a particular firmware.

### Updating and validation

- Install the complete matching folder build: the scrolling-text graphics interface requires the updated `eboot.bin` together with `atmosphere_ui.prx` and `atmosphere_backend.prx`. Do not mix these modules with an older executable.
- Close Atmosphere before updating and preserve `atmosphere-state`. The ZIP includes runtime files and helpers, without saved settings or credentials.
- SMB/FTP integration checks, targeted second-server editing tests, native UI preview checks, and module signing/integrity checks passed. The final backend and UI were uploaded to the test PS5 and verified by SHA-256 readback; user confirmation of the final editing fix is pending.
- Released as a release for the existing firmware 12.70 test setup; broader firmware compatibility remains unverified.

## v2026.10.07.2 — 2026-10-07

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
- Included in the v2026.10.07.2 install ZIP. Close Atmosphere before updating, preserve `atmosphere-state`, and rescan once to populate region metadata. The previous v2026.10.07.1 ZIP predates these changes.

## v2026.10.07.1 — 2026-10-07

### Internal-storage permission repair

- When an internal copy fails to open `/data/homebrew` or `/data/.atmosphere-smb-staging` with a permission error, Atmosphere now starts a bundled permission helper and retries after confirmation.
- Added `atmosphere_permissions.elf`, launched through etaHEN's local ELF loader on port 9021. Atmosphere does not need to close for this repair.
- The helper grants read/write/traverse permissions (`0777`) only to the selected, allowlisted directory. It does not recursively modify game files, repair custom destinations, or change USB permissions.
- Added symlink rejection, same-device checks, request identifiers, an expiry window, and a bounded confirmation wait.
- Added specific errors for a missing helper, an unavailable ELF loader, a failed repair, or missing confirmation.

### Updating

Close Atmosphere before replacing its backend. Install the matching `atmosphere_backend.prx` in `sce_module/` and `atmosphere_permissions.elf` beside `eboot.bin`. Preserve `atmosphere-state`.

The existing `v2026.10.07` release ZIP predates this change and does not include the permission helper. The updated install ZIP is attached to the v2026.10.07.1 release. Build and regression checks passed; console validation of automatic repair is still pending.

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
