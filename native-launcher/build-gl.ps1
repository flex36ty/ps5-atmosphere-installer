$ErrorActionPreference='Stop'
$base=(Resolve-Path "$PSScriptRoot/../..").Path
$dotnet="$base/packaging-tools/dotnet-win/dotnet.exe"
$tool="$base/packaging-tools/SharpProspero/tools/SharpProspero.Bindings.Generator/bin/Debug/net10.0/sharpprospero-bindgen.dll"
$obj="$PSScriptRoot/obj/gl"
function Invoke-Tool { & $dotnet $tool @args; if($LASTEXITCODE){throw "OpenGL link failed: $args"} }
$link=@('link','--obj',"$obj/gl-native.o",'--kind','prx','--publish-name','atmosphere_gl','--export-library','atmosphere_gl','--module-start','atmosphere_gl_start','--out',"$obj/atmosphere_gl.prx")
foreach($lib in (Get-Content "$obj/libraries.json" | ConvertFrom-Json)) {
 Invoke-Tool stub --lib $lib --module-version 1 --names "$obj/$lib.txt" --out "$obj/$lib.a"
 $link+=@('--stub',"$obj/$lib.a")
}
Invoke-Tool @link
Invoke-Tool self --sign --in "$obj/atmosphere_gl.prx" --out "$obj/atmosphere_gl.prx"
Invoke-Tool self --inspect --file "$obj/atmosphere_gl.prx"
