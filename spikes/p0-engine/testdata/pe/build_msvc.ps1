param(
    [string]$OutDir = "C:\Users\PeterWind\AevotraceP0\P0-PE-DLL-01"
)
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }
$envLines = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path ("env:" + $matches[1]) -Value $matches[2]
    }
}

$src = $PSScriptRoot
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item "$src\*.c" $OutDir -Force
Copy-Item "$src\*.def" $OutDir -Force
Set-Location $OutDir
Remove-Item build.log -ErrorAction SilentlyContinue
function Run-Native([string]$command, [string]$what) {
    cmd.exe /c "$command >> build.log 2>&1"
    if ($LASTEXITCODE -ne 0) { throw "$what failed: $LASTEXITCODE" }
}
function Run-To([string]$command, [string]$file, [string]$what) {
    cmd.exe /c "$command > $file 2>&1"
    if ($LASTEXITCODE -ne 0) { throw "$what failed: $LASTEXITCODE" }
}

"== tools ==" | Out-File -Encoding ascii build.log
Run-Native "cl 2>&1 | findstr /C:`"Version`"" "cl version"
Run-Native "link 2>&1 | findstr /C:`"Linker`"" "link version"

"== helper ==" | Out-File -Encoding ascii -Append build.log
Run-Native "cl /nologo /c /Od /GS- /Zl /W4 /Brepro aevhost.c" "cl aevhost"
Run-Native "link /nologo /DLL /NOENTRY /MACHINE:X64 /NODEFAULTLIB /DYNAMICBASE /FIXED:NO /Brepro /OUT:aevhost.dll /IMPLIB:aevhost.lib /DEF:aevhost.def aevhost.obj" "link aevhost"

"== guest ==" | Out-File -Encoding ascii -Append build.log
Run-Native "cl /nologo /c /Od /GS- /Zl /W4 /Brepro aevguest.c" "cl aevguest"
Run-Native "link /nologo /DLL /NOENTRY /MACHINE:X64 /NODEFAULTLIB /DYNAMICBASE /FIXED:NO /Brepro /OUT:aevguest.dll /DEF:aevguest.def aevguest.obj aevhost.lib" "link aevguest"

"== reference ==" | Out-File -Encoding ascii -Append build.log
Run-Native "cl /nologo /Od /W4 /Brepro aevguest_ref.c /link /OUT:aevguest_ref.exe" "cl reference"

"== inspection ==" | Out-File -Encoding ascii -Append build.log
Run-To "dumpbin /headers aevguest.dll" "aevguest.headers.txt" "dumpbin headers"
Run-To "dumpbin /sections aevguest.dll" "aevguest.sections.txt" "dumpbin sections"
Run-To "dumpbin /imports aevguest.dll" "aevguest.imports.txt" "dumpbin imports"
Run-To "dumpbin /exports aevguest.dll" "aevguest.exports.txt" "dumpbin exports"
Run-To "dumpbin /relocations aevguest.dll" "aevguest.relocations.txt" "dumpbin relocations"
Run-To "dumpbin /headers aevhost.dll" "aevhost.headers.txt" "dumpbin helper headers"
Run-To "dumpbin /exports aevhost.dll" "aevhost.exports.txt" "dumpbin helper exports"

"== run reference ==" | Out-File -Encoding ascii -Append build.log
$env:PATH = "$OutDir;$env:PATH"
Run-To ".\aevguest_ref.exe" "reference_stdout.txt" "reference run"
"reference exit=0" | Out-File -Encoding ascii -Append build.log

"== hashes ==" | Out-File -Encoding ascii -Append build.log
$files = @("aevguest.dll", "aevhost.dll", "aevguest_ref.exe", "aevguest.obj", "aevhost.obj")
foreach ($f in $files) {
    $h = (Get-FileHash -Algorithm SHA256 $f).Hash.ToLower()
    "$h  $f" | Out-File -Encoding ascii -Append build.log
}
"BUILD_OK" | Out-File -Encoding ascii -Append build.log
Write-Output "BUILD_OK"
