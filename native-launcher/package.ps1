param(
    [string]$Dotnet = "$PSScriptRoot/../../packaging-tools/dotnet-win/dotnet.exe",
    [string]$SdkRoot = "$PSScriptRoot/../../packaging-tools/SharpProspero"
)
$ErrorActionPreference = 'Stop'
$Dotnet = (Resolve-Path $Dotnet).Path
$SdkRoot = (Resolve-Path $SdkRoot).Path
$tool = Join-Path $SdkRoot 'tools/SharpProspero.Bindings.Generator/bin/Debug/net10.0/sharpprospero-bindgen.dll'
$pack = Join-Path $SdkRoot 'tools/SharpProspero.Packager/bin/Release/net10.0/sharpprospero-pack.dll'
$object = Join-Path $PSScriptRoot 'obj/Release/net10.0/linux-x64/native/Atmosphere.o'
$runtime = Join-Path $PSScriptRoot 'obj/runtime-support'
foreach ($required in @($tool, $pack, $object, "$PSScriptRoot/sce_module/libc.prx")) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing build input: $required" }
}
if (!(Test-Path "$runtime/atmosphere-streams.o")) {
    throw 'Missing standard stream initialization object. Run compile-linux.sh first.'
}
foreach ($inputFile in @('Program.cs', 'Atmosphere.csproj', '../build/atmosphere_core.elf')) {
    if ((Get-Item $object).LastWriteTimeUtc -lt (Get-Item "$PSScriptRoot/$inputFile").LastWriteTimeUtc) {
        throw "$inputFile is newer than the launcher object. Run compile-linux.sh first."
    }
}
if ((Get-Item "$runtime/atmosphere-compat.o").LastWriteTimeUtc -lt (Get-Item "$PSScriptRoot/runtime-compat.c").LastWriteTimeUtc) {
    throw 'Runtime compatibility code is newer than its object. Run compile-linux.sh first.'
}
$output = Join-Path $PSScriptRoot "../build/pkg-$(Get-Date -Format yyyyMMdd-HHmmss)"
$module = Join-Path $output 'module'
New-Item -ItemType Directory -Force $module | Out-Null
function Invoke-Bindgen {
    & $Dotnet $tool @args
    if ($LASTEXITCODE -ne 0) { throw "Native tool failed: $args" }
}
$link = @('link', '--self-contained', '--obj', $object)
Invoke-Bindgen prx --module "$PSScriptRoot/sce_module/libc.prx" --names "$PSScriptRoot/libc-extra.txt" --strict --out "$PSScriptRoot/obj/libc-extra-bindings.cs"
Invoke-Bindgen stub --module "$PSScriptRoot/sce_module/libc.prx" --names "$PSScriptRoot/libc-extra.txt" --out "$PSScriptRoot/obj/libc-extra.a"
$link += @('--stub', "$PSScriptRoot/obj/libc-extra.a")
foreach ($file in Get-ChildItem $runtime -Filter *.o) { $link += @('--obj', $file.FullName) }
foreach ($file in Get-ChildItem $runtime -Filter *.a) { $link += @('--lib', $file.FullName) }
$link += @('--out', "$module/eboot.bin", '--entry', '_start', '--kind', 'eboot')
Invoke-Bindgen @link
Copy-Item -Recurse "$PSScriptRoot/sce_sys", "$PSScriptRoot/sce_module" $module
Invoke-Bindgen sceassets --folder "$module/sce_sys"
Invoke-Bindgen modules --module "$module/eboot.bin" --folder "$module/sce_module"
Invoke-Bindgen param --folder $module --apply
& $Dotnet $tool sysver --folder $module --policy match --apply
if ($LASTEXITCODE -notin @(0, 4)) { throw 'Firmware requirement validation failed.' }
Invoke-Bindgen self --sign --in "$module/eboot.bin" --out "$module/eboot.bin"
Invoke-Bindgen self --sign --in "$module/sce_module/libc.prx" --out "$module/sce_module/libc.prx"
& $Dotnet $pack --in $module --out $output
if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
$packages = @(Get-ChildItem $output -Filter *.pkg)
if ($packages.Count -ne 1) { throw 'Expected exactly one package.' }
$final = Join-Path $PSScriptRoot '../build/Atmosphere.pkg'
Copy-Item $packages[0].FullName $final -Force
$manifest = [ordered]@{
    package = (Get-FileHash $final -Algorithm SHA256).Hash
    backend = (Get-FileHash "$PSScriptRoot/../build/atmosphere_core.elf" -Algorithm SHA256).Hash
    libc = (Get-FileHash "$PSScriptRoot/sce_module/libc.prx" -Algorithm SHA256).Hash
    toolchainRevision = '5dfdb4b0b166e83dc9ae275001afb695f12c7174'
    packageLibraryRevision = '748eabf1b7d17819528cabf367d8e27109d8fce3'
    packageLibraryPatch = 'BuildU2c: handle the trailing sentinel group at exact multiples of eight blocks'
    consoleTested = $false
}
$manifest | ConvertTo-Json | Set-Content "$final.manifest.json" -Encoding utf8
Write-Output "Built $final (console testing still required)."
