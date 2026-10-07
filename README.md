# Atmosphere

See [CHANGELOG.md](CHANGELOG.md) for changes and update requirements.

Atmosphere is a controller-driven PS5 homebrew application for browsing games on your own SMB and FTP servers and copying them to internal storage or an attached USB drive. It is derived from [Orbit Store](https://github.com/saawant12/orbit-store-ps5).

The current application uses a native OpenGL interface and an in-process C backend. Normal browsing and copying do not require a separately launched service ELF.

[Watch the Atmosphere demo (WebM)](https://github.com/flex36ty/ps5-atmosphere-installer/releases/download/v2026.10.07.2/Atmosphere_20261008000625.webm)

![Atmosphere library with frosted game cards, metadata tags, and a rounded selection outline](docs/images/atmosphere-library.jpg)

![Atmosphere copy dialog with game details and USB and internal storage destinations](docs/images/atmosphere-copy-dialog.jpg)

## Features

- Multiple SMB and FTP servers, including anonymous/guest connections; activate, deactivate, edit, and remove saved servers.
- Discovery of game folders, exFAT images, and FFPFSC images, with metadata and cached cover art where supported.
- Smooth scrolling, controller focus animations, square cover cards, and a frosted background.
- Sorting by title or date added.
- Copy to internal storage or USB, including a USB-root destination option.
- Automatic repair of permission-denied errors on the default internal destination and staging folders, using a bundled etaHEN helper.
- Transfer progress, speed, pause/resume, and SHA-256 verification.
- Installed-game detection by metadata title ID, with installed-source deletion through ShadowMountPlus.
- Minimum firmware display when present in metadata. The Backported label indicates detected `fakelib`/`fakelib2` content; it does not guarantee compatibility.
- Subdued metadata tags and short region labels derived from a valid game content ID. Region is a content territory, not a guarantee of language support or region locking. Rescan after updating to populate the new field; unavailable regions show Unknown.
- ShadowMountPlus rescan requests after transfers. Detection may complete after Atmosphere closes.

## Requirements and status

The current deployment is a folder application named **Atmosphere**, title ID **PPSA99005**, used with an existing etaHEN/kstuff and ShadowMountPlus setup. Development and console testing have used firmware **12.70**; compatibility with every firmware through 13.60 has not been verified.

The working folder deployment should not be confused with the older experimental PKG build. A generally installable PKG is not currently validated.

The delete workflow uses a bundled helper launched through etaHEN's local ELF loader on port 9021. Atmosphere closes before that helper deletes the confirmed installed source. Leave the app closed while deletion finishes.

Internal permission repair also requires etaHEN's ELF loader on port 9021. If `/data/homebrew` or `/data/.atmosphere-smb-staging` rejects access, the app launches `atmosphere_permissions.elf`, waits for confirmation, and retries. The helper grants read/write/traverse permissions on that directory only; it does not recursively change game files or repair custom destinations. Copy the helper alongside `eboot.bin` when updating to a build with this feature.

## Installation

These instructions describe the tested **USB folder installation with ShadowMountPlus**. Download `Atmosphere-PPSA99005-2026.10.07.2.zip` from the [release page](https://github.com/flex36ty/ps5-atmosphere-installer/releases/tag/v2026.10.07.2). This attachment contains the complete install folder, including runtime modules. The automatically generated source ZIP is not a ready-to-run application.

The v2026.10.07.2 prerelease adds frosted cards, a continuous rounded selector, region tags, and transfer diagnostics, and retains the permission helper. Close Atmosphere before updating, preserve `atmosphere-state`, and rescan once for region metadata. Build and regression tests passed; automatic permission repair and the final selector adjustment still await console confirmation. See the changelog for details.

1. Start your working etaHEN/kstuff session and ensure ShadowMountPlus is available.
2. Extract the complete Atmosphere folder build on your computer. Keep its supplied directory structure and runtime files intact.
3. Copy the entire `PPSA99005` folder into the USB drive's `homebrew` directory or /data/homebrew directory. The tested console path is `/mnt/usb0/homebrew/PPSA99005`. You can copy it with the drive connected to your computer or use the PS5's FTP server. The USB mount number may differ on your console.
4. Check that `eboot.bin` is directly inside `PPSA99005`, rather than accidentally nested inside another `PPSA99005` directory. The layout should include:

   ```text
   homebrew/
     PPSA99005/
       eboot.bin
       atmosphere_delete.elf
       atmosphere_permissions.elf
       sce_sys/
         param.json
         icon0.png
       sce_module/
         atmosphere_backend.prx
         atmosphere_ui.prx
         ...other supplied runtime modules
       ui/
         ...supplied UI assets
   ```

5. Let ShadowMountPlus scan/register the folder using your existing setup. Close any running application if detection is pending.
6. Launch **Atmosphere** from the PS5 home screen, then configure a server as described below.

### First server setup

Open **Servers** and press **Square** to add a server. Choose SMB or FTP and enter the server IP address or hostname.

- **SMB:** enter the share name separately from the folder. For example, `\\SERVER\games\ps5` uses share `games` and folder `ps5`. The default port is `445`. Guest shares can use blank credentials if the server permits guest access.
- **FTP:** enter the folder relative to the FTP server's root, without a leading slash. Leave Share and Domain blank. The default port is `21`; blank username/password uses anonymous FTP if the server permits it. This is plain FTP, not SFTP.

Save the server, highlight it, and press **Cross** so its status reads **ACTIVE**. Go to **Library** and press **Square** to scan. If the list stays empty, check the server path, permissions, and that the PS5 can reach the server on your network.

## Usage

1. Launch Atmosphere from the registered folder application.
2. Open **Servers**, add an SMB or FTP server, and activate it. Use your server address, share/path, and credentials as appropriate.
3. Return to **Library** and press Square to scan.
4. Select a game, choose a destination, and start copying.
5. Close Atmosphere with Options when finished so ShadowMountPlus can finish detecting copied games.

Game files and configured server credentials are not included in this repository.

## Controller controls

| Control | Action |
| --- | --- |
| D-pad / left stick | Move selection |
| L1 / R1 | Switch tabs |
| Cross | Select / confirm; activate or deactivate a highlighted server |
| Circle | Back / dismiss |
| Square in Library | Refresh / scan |
| Triangle in Library | Change sort |
| R2 on an installed game | Open delete confirmation |
| Square in Servers | Add server |
| Triangle in Servers | Edit active server |
| R2 in Servers | Remove selected server |
| Options | Close Atmosphere |

Follow the on-screen hints for transfer and destination controls.

## License and credits

GPL-3.0-or-later. See [LICENSE](LICENSE), [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), and [native OpenGL notices](native-launcher/OPENGL-NOTICES.txt).

Original upstream: Orbit Store by saawant12. Original attribution and upstream URLs are retained. Game names and artwork belong to their respective owners and are not covered by the application's source license.
