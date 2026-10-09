#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sdk=/home/flex360/atmosphere-toolchain/ps5-payload-sdk
hb="$sdk/target/user/homebrew"
out=native-launcher/obj/backend
mkdir -p "$out"
objects=()
cp "$sdk/target/lib/libc.a" "$out/libc-native.a"
llvm-ar-19 d "$out/libc-native.a" dlfcn.o mman.o syscalls.o
for source in backend/*.c .deps/cjson/cJSON.c native-launcher/backend-platform.c native-launcher/native-files.c native-launcher/native-directories.c native-launcher/native-sockets.c native-launcher/native-vfs.c native-launcher/backend-globals.c native-launcher/native-bridge.c; do
  if [ "$source" = "backend/api.c" ]; then continue; fi
  "$sdk/bin/prospero-clang" -Wno-unreachable-code-generic-assoc -std=c11 -O2 -fPIC -DATMOSPHERE_NATIVE_APP -D_FILE_OFFSET_BITS=64 -Ibackend -Ilauncher -I.deps/cjson -Ibuild/generated -I"$hb/include" -c "$source" -o "$out/$(basename "$source" .c).o"
  objects+=("$out/$(basename "$source" .c).o")
done
ld.lld-19 -r -d -o "$out/backend.o" "${objects[@]}" --start-group "$hb/lib/libsmb2.a" "$hb/lib/libcurl.a" "$hb/lib/libssl.a" "$hb/lib/libcrypto.a" "$hb/lib/libz.a" "$out/libc-native.a" --end-group --wrap=open --wrap=close --wrap=dup --wrap=access --wrap=fcntl --wrap=connect --wrap=lstat --wrap=openat --wrap=mkdirat --wrap=fstatat --wrap=linkat --wrap=renameat --wrap=unlinkat --wrap=statvfs --wrap=fstatvfs --wrap=opendir --wrap=readdir --wrap=closedir
llvm-nm-19 -u "$out/backend.o" > "$out/imports.txt"
llvm-nm-19 "$out/backend.o" > "$out/symbols.txt"
if ! grep -q ' Curl_handler_ftp$' "$out/symbols.txt"; then
  echo 'FTP is disabled in SDK libcurl; rebuild dependencies with --enable-ftp.' >&2
  exit 1
fi

llvm-objcopy-19 --rename-section .ctors=.init_array --rename-section .dtors=.fini_array "$out/backend.o" "$out/backend-native.o"

# Native titles may only enter the kernel through the system libraries.
llvm-objdump-19 -d "$out/backend-native.o" | awk '/[[:space:]]syscall([[:space:]]|$)/ {print; bad=1} END {exit bad}' > "$out/syscall-audit.txt"
bash native-launcher/build-delete-helper.sh
