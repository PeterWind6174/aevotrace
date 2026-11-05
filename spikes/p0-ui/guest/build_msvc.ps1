param(
    [string]$OutDir = "."
)
$ErrorActionPreference = "Stop"

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found"
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "Visual Studio C++ x64 tools not found"
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found: $vcvars"
}
Write-Host "VS=$vsPath"
Write-Host "VCVARS=$vcvars"
& cmd.exe /d /c "call `"$vcvars`" >nul && cl /nologo /LD /O2 /GS- /utf-8 /W4 /DUNICODE /D_UNICODE /DP0_GUI_EXPORTS p0_gui_test.c /link /NODEFAULTLIB kernel32.lib user32.lib /ENTRY:DllMain /DEF:p0_gui_test.def /MACHINE:X64 /OUT:p0_gui_test.dll"
if ($LASTEXITCODE -ne 0) { throw "DLL compile failed: $LASTEXITCODE" }

& cmd.exe /d /c "call `"$vcvars`" >nul && cl /nologo /O2 /GS- /utf-8 /W4 /DUNICODE /D_UNICODE p0_gui_driver.c /link /SUBSYSTEM:CONSOLE user32.lib /MACHINE:X64 /OUT:p0_gui_driver.exe"
if ($LASTEXITCODE -ne 0) { throw "driver compile failed: $LASTEXITCODE" }

& cmd.exe /d /c "call `"$vcvars`" >nul && dumpbin /headers p0_gui_test.dll > dumpbin_headers.txt && dumpbin /exports p0_gui_test.dll > dumpbin_exports.txt && dumpbin /imports p0_gui_test.dll > dumpbin_imports.txt"
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed: $LASTEXITCODE" }

Get-FileHash p0_gui_test.dll -Algorithm SHA256 | Format-List
Get-FileHash p0_gui_driver.exe -Algorithm SHA256 | Format-List
Get-Item p0_gui_test.dll, p0_gui_driver.exe | Select-Object Name,Length
Write-Host "BUILD OK"
