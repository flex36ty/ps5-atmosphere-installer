$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../../..").Path
$launcher=(Resolve-Path "$PSScriptRoot/..").Path
$dotnet="$root/packaging-tools/dotnet-win/dotnet.exe"
$tool="$root/packaging-tools/SharpProspero/tools/SharpProspero.Bindings.Generator/bin/Debug/net10.0/sharpprospero-bindgen.dll"
$output="$PSScriptRoot/obj/atmosphere_ui.prx"
Set-Content "$PSScriptRoot/obj/kernel-extra.txt" @('getegid','seteuid','tcgetattr','tcsetattr')
& $dotnet $tool stub --lib libkernel --module-version 1 --names "$PSScriptRoot/obj/kernel-extra.txt" --out "$PSScriptRoot/obj/kernel-extra.a"
if($LASTEXITCODE){throw 'UI kernel stub generation failed'}
$link=@('link','--self-contained','--system-libc','--kind','prx','--module-start','atmosphere_ui_start','--publish-name','atmosphere_ui','--export-library','atmosphere_ui','--out',$output,
 '--obj',"$launcher/obj/runtime-streams.o",'--obj',"$PSScriptRoot/obj/module.o",'--obj',"$PSScriptRoot/obj/Release/net10.0/linux-x64/native/Atmosphere.UI.o",'--obj',"$PSScriptRoot/obj/runtime/libbootstrapperdll.o",'--stub',"$launcher/obj/runtime-test.a",'--stub',"$PSScriptRoot/obj/kernel-extra.a")
foreach($file in Get-ChildItem "$launcher/obj/runtime-support" -Filter *.o) {
 if($file.Name -notin @('libbootstrapper.o','atmosphere-streams.o')){$link+=@('--obj',$file.FullName)}
}
foreach($file in Get-ChildItem "$launcher/obj/runtime-support" -Filter *.a){$link+=@('--lib',$file.FullName)}
& $dotnet $tool @link
if($LASTEXITCODE){throw 'UI module link failed'}
Copy-Item -LiteralPath $output -Destination "$PSScriptRoot/obj/atmosphere_ui.elf" -Force
& $dotnet $tool self --sign --in $output --out $output
if($LASTEXITCODE){throw 'UI signing failed'}
& $dotnet $tool self --inspect --file $output
if($LASTEXITCODE){throw 'UI module validation failed'}
