#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
export DOTNET_ROOT="${ATMOSPHERE_DOTNET_ROOT:-/home/flex360/atmosphere-toolchain/dotnet}"
export PATH="$DOTNET_ROOT:$PATH"
export NUGET_PACKAGES="${ATMOSPHERE_NUGET_CACHE:-/home/flex360/atmosphere-toolchain/nuget}"
export DOTNET_CLI_TELEMETRY_OPTOUT=1
mkdir -p obj/runtime-support
clang-19 -O2 -fPIC -fno-stack-protector -c runtime-entropy.c -o obj/runtime-support/atmosphere-entropy.o
clang-19 -O1 -g -fsanitize=address,undefined runtime-entropy.c runtime-entropy-test.c -o obj/runtime-entropy-test
obj/runtime-entropy-test
clang-19 -O2 -fPIC -fno-stack-protector -c runtime-compat.c -o obj/runtime-support/atmosphere-compat.o
clang-19 -O2 -fPIC -fno-stack-protector -c runtime-streams.c -o obj/runtime-support/atmosphere-streams.o
clang-19 -O1 -g -fsanitize=address,undefined runtime-streams-test.c -o obj/runtime-streams-test
ASAN_OPTIONS=detect_leaks=0 obj/runtime-streams-test
clang-19 -O1 -g -fsanitize=address,undefined -DATMOSPHERE_COMPAT_TEST runtime-compat.c runtime-compat-test.c -o obj/runtime-compat-test
ASAN_OPTIONS=detect_leaks=0 obj/runtime-compat-test
dotnet restore Atmosphere.csproj -r linux-x64 --source ../../packaging-tools/nuget-feed -p:NuGetAudit=false
# The SDK replaces the final Linux link with the PS5 linker. AOT must first emit a fresh object.
object=obj/Release/net10.0/linux-x64/native/Atmosphere.o
if [ -f "$object" ]; then mv "$object" "$object.previous"; fi
dotnet publish Atmosphere.csproj -c Release -r linux-x64 --no-restore || true
test -s "$object"
pack="$NUGET_PACKAGES/microsoft.netcore.app.runtime.nativeaot.linux-x64"
python3 - <<'PY'
import json, pathlib, shutil, os
assets = json.loads(pathlib.Path('obj/project.assets.json').read_text())
name = 'Microsoft.NETCore.App.Runtime.NativeAOT.linux-x64'
deps = assets['project']['frameworks']['net10.0']['downloadDependencies']
version = next(d['version'].strip('[]').split(',')[0].strip() for d in deps if d['name'] == name)
source = pathlib.Path(os.environ['NUGET_PACKAGES']) / name.lower() / version / 'runtimes/linux-x64/native'
dest = pathlib.Path('obj/runtime-support')
dest.mkdir(exist_ok=True)
names = ['libbootstrapper.o', 'libRuntime.WorkstationGC.a', 'libRuntime.VxsortDisabled.a',
         'libeventpipe-disabled.a', 'libstandalonegc-disabled.a', 'libaotminipal.a',
         'libstdc++compat.a', 'libSystem.Native.a', 'libz.a', 'libbrotlienc.a',
         'libbrotlidec.a', 'libbrotlicommon.a', 'libSystem.IO.Compression.Native.a']
for name in names:
    if (source / name).exists(): shutil.copy2(source / name, dest / name)
print('NativeAOT object and runtime archives ready.')
PY
