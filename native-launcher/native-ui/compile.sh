#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
export DOTNET_ROOT=/home/flex360/atmosphere-toolchain/dotnet
export PATH="$DOTNET_ROOT:/usr/lib/llvm-19/bin:$PATH"
export NUGET_PACKAGES=/home/flex360/atmosphere-toolchain/nuget
export DOTNET_CLI_TELEMETRY_OPTOUT=1
dotnet restore Atmosphere.UI.csproj -r linux-x64 --source ../../../packaging-tools/nuget-feed -p:NuGetAudit=false
object=obj/Release/net10.0/linux-x64/native/Atmosphere.UI.o
if [[ -f $object ]]; then mv "$object" "$object.previous"; fi
dotnet publish Atmosphere.UI.csproj -c Release -r linux-x64 --no-restore || true
test -s "$object"
mkdir -p obj/runtime
clang-19 -O2 -fPIC -c module.c -o obj/module.o
python3 - <<'PY'
import json,pathlib,os,shutil
a=json.loads(pathlib.Path('obj/project.assets.json').read_text())
n='Microsoft.NETCore.App.Runtime.NativeAOT.linux-x64'
d=a['project']['frameworks']['net10.0']['downloadDependencies']
v=next(x['version'].strip('[]').split(',')[0].strip() for x in d if x['name']==n)
p=pathlib.Path(os.environ['NUGET_PACKAGES'])/n.lower()/v/'runtimes/linux-x64/native'
shutil.copy2(p/'libbootstrapperdll.o','obj/runtime/libbootstrapperdll.o')
print('Native UI library object and library bootstrapper ready')
PY
llvm-objcopy-19 --redefine-sym _ZL17InitializeRuntimev=atmosphere_runtime_initialize obj/runtime/libbootstrapperdll.o
llvm-objcopy-19 --globalize-symbol atmosphere_runtime_initialize obj/runtime/libbootstrapperdll.o
