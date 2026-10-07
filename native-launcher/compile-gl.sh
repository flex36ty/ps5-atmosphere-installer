#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
sdk=/home/flex360/atmosphere-toolchain/ps5-payload-sdk
gl=../../packaging-tools/ps5-opengl/ps5-opengl-sdk-1.0.1/sdk
out=obj/gl
mkdir -p "$out"
extra=()
if [[ ${ATMOSPHERE_GL_SMOKE:-0} == 1 ]]; then extra+=(-DATMOSPHERE_GL_SMOKE); fi
for source in gl-renderer.c gl-app-heap.c gl-runtime.c; do
 "$sdk/bin/prospero-clang" -std=c11 -O2 -fPIC "${extra[@]}" -I"$gl/include" -c "$source" -o "$out/${source%.c}.o"
done
cp "$sdk/target/lib/libc.a" "$out/libc-native.a"
llvm-ar-19 d "$out/libc-native.a" dlfcn.o mman.o syscalls.o
ld.lld-19 -r -d -o "$out/gl.o" "$out/gl-renderer.o" "$out/gl-app-heap.o" "$out/gl-runtime.o" -u ps5_agc_gate2_run \
 -L"$gl/lib" --start-group -lPS5OpenGL "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a" "$out/libc-native.a" --end-group \
 --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size
llvm-objcopy-19 --redefine-sym __real_malloc_usable_size=malloc_usable_size --rename-section .ctors=.init_array --rename-section .dtors=.fini_array "$out/gl.o" "$out/gl-native.o"
llvm-nm-19 -u "$out/gl-native.o" > "$out/imports.txt"
llvm-objdump-19 -d "$out/gl-native.o" | awk '/[[:space:]]syscall([[:space:]]|$)/ {print; bad=1} END {exit bad}' > "$out/syscall-audit.txt"
python3 gl-imports.py
