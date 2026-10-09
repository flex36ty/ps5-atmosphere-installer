$ErrorActionPreference = 'Stop'
$base = (Resolve-Path "$PSScriptRoot/../..").Path
$dotnet = "$base/packaging-tools/dotnet-win/dotnet.exe"
$tool = "$base/packaging-tools/SharpProspero/tools/SharpProspero.Bindings.Generator/bin/Debug/net10.0/sharpprospero-bindgen.dll"
$obj = "$PSScriptRoot/obj/backend"
function Invoke-Tool {
    & $dotnet $tool @args
    if ($LASTEXITCODE) { throw "Native backend tool failed: $args" }
}
$link = @('link','--obj',"$obj/backend-native.o",'--kind','prx','--publish-name','atmosphere_backend','--export-library','atmosphere_backend','--export','atmosphere_backend_run','--export','atmosphere_request_stop','--export','atmosphere_backend_stage','--export','atmosphere_native_request','--export','atmosphere_native_free','--out',"$obj/atmosphere_backend.prx")
foreach ($lib in @('libSceLibcInternal','libkernel','libSceNet')) {
    $names = "$obj/$lib.txt"
    if ($lib -eq 'libkernel') {
        $names = "$obj/kernel-directory-imports.txt"
        @((Get-Content "$obj/libkernel.txt"); 'sceKernelGetdents'; 'sceKernelGetdirentries') | Sort-Object -Unique | Set-Content $names
    }
    Invoke-Tool stub --lib $lib --module-version 1 --names $names --out "$obj/$lib.a"
    $link += @('--stub',"$obj/$lib.a")
}
$link += @('--module-start','atmosphere_module_start')
Invoke-Tool @link
Invoke-Tool self --sign --in "$obj/atmosphere_backend.prx" --out "$obj/atmosphere_backend.prx"
Invoke-Tool self --inspect --file "$obj/atmosphere_backend.prx"

