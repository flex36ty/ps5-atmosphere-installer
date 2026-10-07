# Third-party notices

Atmosphere is derived from Orbit Store by saawant12. Original upstream source: https://github.com/saawant12/orbit-store-ps5. The original licensing and third-party notices remain applicable.

Atmosphere is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. See [LICENSE](LICENSE). It is distributed without any warranty.

Each release attaches the source archive of the exact commit its `atmosphere.elf` was built from, together with the upstream sources of the copyleft components below.

## Components in atmosphere.elf

| Component | Version | Licence | Licence text | Source |
|---|---|---|---|---|
| zlib | 1.3.1 | zlib | [licenses/zlib.txt](licenses/zlib.txt) | [zlib source](https://zlib.net/fossils/zlib-1.3.1.tar.gz) |
| libsmb2 client library (DCE/RPC disabled) | commit `51c5910da240a3b60bc4ad1b472185c2815628d4` | LGPL-2.1-or-later | [licenses/LGPL-2.1.txt](licenses/LGPL-2.1.txt) and upstream COPYING | [pinned source archive](https://codeload.github.com/sahlberg/libsmb2/tar.gz/51c5910da240a3b60bc4ad1b472185c2815628d4) |
| PS5 payload SDK: startup code and libraries linked by its toolchain | v0.43, commit `d9c9519116944a7f1c22d262012c53b80b3520b7` | GPL-3.0-or-later (files in `include/freebsd` are BSD) | [licenses/GPL-3.0.txt](licenses/GPL-3.0.txt) | `ps5-payload-sdk-d9c9519.tar.gz` in each release; [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) |
| GNU libmicrohttpd | 1.0.1 | LGPL-2.1-or-later | [licenses/LGPL-2.1.txt](licenses/LGPL-2.1.txt) | `libmicrohttpd-1.0.1.tar.gz` in each release; [gnu.org](https://www.gnu.org/software/libmicrohttpd/) |
| curl (libcurl) | 8.18.0 | curl licence | [licenses/curl.txt](licenses/curl.txt) | [curl.se](https://curl.se/download/curl-8.18.0.tar.xz) |
| OpenSSL | 3.5.2 | Apache-2.0 | [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt) | [openssl.org](https://github.com/openssl/openssl/releases/tag/openssl-3.5.2) |
| cJSON | 1.7.19 | MIT | [licenses/cJSON-MIT.txt](licenses/cJSON-MIT.txt) | [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON/tree/v1.7.19) |
| React and React DOM | 19.3.0 | MIT | [licenses/React-MIT.txt](licenses/React-MIT.txt) | [facebook/react](https://github.com/facebook/react) |
| scheduler | 0.28.0 | MIT | [licenses/scheduler-MIT.txt](licenses/scheduler-MIT.txt) | [facebook/react](https://github.com/facebook/react) |
| Vite runtime helper (module preload) | 6.4.3 | MIT | [licenses/Vite-MIT.txt](licenses/Vite-MIT.txt) | [vitejs/vite](https://github.com/vitejs/vite) |
| Mozilla CA certificate bundle, via curl's CA Extract | bundle fetched by `tools/bootstrap.py` | MPL-2.0 | [licenses/MPL-2.0.txt](licenses/MPL-2.0.txt) | Embedded unmodified; [curl.se/docs/caextract.html](https://curl.se/docs/caextract.html) |

libmicrohttpd is linked statically. Its complete source and Atmosphere's complete source are both provided, so you can rebuild `atmosphere.elf` with a modified libmicrohttpd using the steps in [BUILDING.md](BUILDING.md).

## Not covered by these licences

Game names, cover art and screenshots shown by Atmosphere belong to their respective owners. Artwork is loaded from providers or the user's SMB files at runtime; it is not part of Atmosphere's licensed source and is not licensed under the GPL.
