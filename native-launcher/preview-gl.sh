#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
clang-19 -shared -fPIC -O2 -DATMOSPHERE_GL_HOST -I../../packaging-tools/ps5-opengl/ps5-opengl-sdk-1.0.1/sdk/include gl-renderer.c -Wl,-l:libEGL.so.1 -Wl,-l:libGL.so.1 -lm -o obj/gl/host-renderer.so
export DOTNET_ROOT=/home/flex360/atmosphere-toolchain/dotnet
export PATH="$DOTNET_ROOT:$PATH"
export NUGET_PACKAGES=/home/flex360/atmosphere-toolchain/nuget
export EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1
export ATMOSPHERE_GL_TEST="$PWD/obj/gl/host-renderer.so"
dotnet run --project preview/Preview.csproj -p:RestoreSources=/mnt/c/projects/atmosphereps5/packaging-tools/nuget-feed -p:NuGetAudit=false
