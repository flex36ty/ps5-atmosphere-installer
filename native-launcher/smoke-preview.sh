#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
clang-19 -shared -fPIC -O2 -DATMOSPHERE_GL_HOST -DATMOSPHERE_GL_SMOKE -I../../packaging-tools/ps5-opengl/ps5-opengl-sdk-1.0.1/sdk/include gl-renderer.c -Wl,-l:libEGL.so.1 -Wl,-l:libGL.so.1 -lm -o obj/gl/host-smoke.so
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 python3 smoke-preview.py
