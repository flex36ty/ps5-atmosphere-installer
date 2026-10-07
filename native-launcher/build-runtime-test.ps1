param([string]$Runtime = "$PSScriptRoot/sce_module/libc.prx", [switch]$TraceStartup, [switch]$SystemLibc)
$ErrorActionPreference = 'Stop'
$base = (Resolve-Path "$PSScriptRoot/../..").Path
$dotnet = "$base/packaging-tools/dotnet-win/dotnet.exe"
$tool = "$base/packaging-tools/SharpProspero/tools/SharpProspero.Bindings.Generator/bin/Debug/net10.0/sharpprospero-bindgen.dll"
$out = "$PSScriptRoot/../build/Atmosphere-runtime-test/FAKE34177"
function Invoke-Tool {
    & $dotnet $tool @args
    if ($LASTEXITCODE) { throw "Tool failed: $args" }
}
$streams = "$PSScriptRoot/obj/runtime-streams.o"
if (!(Test-Path $streams)) { throw 'Compile runtime-streams.c to obj/runtime-streams.o first.' }
New-Item -ItemType Directory -Force "$out/sce_module" | Out-Null
Copy-Item "$PSScriptRoot/sce_sys" $out -Recurse -Force
Copy-Item "$PSScriptRoot/obj/backend/atmosphere_backend.prx" "$out/sce_module/atmosphere_backend.prx" -Force
Copy-Item "$PSScriptRoot/obj/gl/atmosphere_gl.prx" "$out/sce_module/atmosphere_gl.prx" -Force
New-Item -ItemType Directory -Force "$out/ui" | Out-Null
Copy-Item "$PSScriptRoot/gl-assets/Montserrat-Medium.ttf","$PSScriptRoot/gl-assets/OFL.txt" "$out/ui" -Force
$glLicense="$out/ui/licenses/ps5-opengl"
New-Item -ItemType Directory -Force $glLicense | Out-Null
$glSdk="$base/packaging-tools/ps5-opengl/ps5-opengl-sdk-1.0.1"
Copy-Item "$glSdk/LICENSE","$glSdk/THIRD_PARTY_NOTICES.md" $glLicense -Force
Copy-Item "$glSdk/LICENSES" $glLicense -Recurse -Force
Copy-Item "$PSScriptRoot/OPENGL-NOTICES.txt" "$out/ui" -Force
Copy-Item $Runtime "$out/sce_module/libc.prx" -Force
Set-Content "$PSScriptRoot/obj/runtime-test-imports.txt" @('fgetc','vfscanf','fdopen','fopen')
if (!$SystemLibc) { Invoke-Tool prx --module "$out/sce_module/libc.prx" --names "$PSScriptRoot/obj/runtime-test-imports.txt" --strict --out "$PSScriptRoot/obj/runtime-test-imports.cs"
Invoke-Tool stub --module "$out/sce_module/libc.prx" --names "$PSScriptRoot/obj/runtime-test-imports.txt" --out "$PSScriptRoot/obj/runtime-test.a" } else { Invoke-Tool stub --lib libSceLibcInternal --module-version 1 --names "$PSScriptRoot/obj/runtime-test-imports.txt" --out "$PSScriptRoot/obj/runtime-test.a" }
$link = @('link','--self-contained','--obj',"$PSScriptRoot/obj/Release/net10.0/linux-x64/native/Atmosphere.o",'--obj',$streams,'--stub',"$PSScriptRoot/obj/runtime-test.a")
foreach ($file in Get-ChildItem "$PSScriptRoot/obj/runtime-support" -Filter *.o) {
    if ($file.Name -ne 'atmosphere-streams.o') { $link += @('--obj',$file.FullName) }
}
foreach ($file in Get-ChildItem "$PSScriptRoot/obj/runtime-support" -Filter *.a) { $link += @('--lib',$file.FullName) }
if ($TraceStartup -or $SystemLibc) { $link += @('--obj',"$PSScriptRoot/obj/runtime-entry.o") }
$entry = if ($TraceStartup -or $SystemLibc) { 'atmosphere_start' } else { '_start' }
$link += @('--out',"$out/eboot.bin",'--entry',$entry,'--kind','eboot')
if ($SystemLibc) { $link += '--system-libc' }; Invoke-Tool @link
Invoke-Tool sceassets --folder "$out/sce_sys"
& $dotnet $tool sysver --folder $out --policy match --apply
if ($LASTEXITCODE -notin @(0,4)) { throw 'Firmware validation failed.' }
Invoke-Tool self --sign --in "$out/eboot.bin" --out "$out/eboot.bin"
Compress-Archive -Path $out -DestinationPath "$PSScriptRoot/../build/Atmosphere-runtime-test.zip" -Force


