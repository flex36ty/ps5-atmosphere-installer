#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
clang-19 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I.deps/cjson tests/copy_destinations.c .deps/cjson/cJSON.c -o build/copy-destinations-test
build/copy-destinations-test
bash native-launcher/compile-backend.sh > build/destinations-backend.log 2>&1
bash native-launcher/native-ui/compile.sh > build/destinations-ui.log 2>&1
