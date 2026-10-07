#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sdk=/home/flex360/atmosphere-toolchain/ps5-payload-sdk
hb="$sdk/target/user/homebrew"
"$sdk/bin/prospero-clang" -O2 -Wno-unreachable-code-generic-assoc -I.deps/cjson -I"$hb/include" native-launcher/delete-helper.c .deps/cjson/cJSON.c -L"$hb/lib" -lcurl -lssl -lcrypto -lz -o native-launcher/obj/atmosphere_delete.elf
"$sdk/bin/prospero-clang" -O2 -Wno-unreachable-code-generic-assoc -I.deps/cjson native-launcher/permission-helper.c .deps/cjson/cJSON.c -o native-launcher/obj/atmosphere_permissions.elf
