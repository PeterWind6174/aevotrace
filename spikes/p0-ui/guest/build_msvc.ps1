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

# Do not use Get-FileHash: under cmd.exe /c the inherited PSModulePath can
# point at PowerShell 7 modules, so Windows PowerShell 5.1 cannot auto-load
# Microsoft.PowerShell.Utility. The .NET API below needs no module loading.
function Get-Sha256Hex([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead($Path)
        $bytes = $sha.ComputeHash($stream)
        return ([System.BitConverter]::ToString($bytes) -replace '-', '').ToLowerInvariant()
    }
    finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}

# /Brepro makes the PE output independent of wall-clock timestamps and
# link-time environment noise; rebuilds of the same sources must match.
& cmd.exe /d /c "call `"$vcvars`" >nul && cl /nologo /LD /O2 /GS- /utf-8 /W4 /DUNICODE /D_UNICODE /DP0_GUI_EXPORTS p0_gui_test.c /link /Brepro /NODEFAULTLIB kernel32.lib user32.lib /ENTRY:DllMain /DEF:p0_gui_test.def /MACHINE:X64 /OUT:p0_gui_test.dll"
if ($LASTEXITCODE -ne 0) { throw "DLL compile failed: $LASTEXITCODE" }

& cmd.exe /d /c "call `"$vcvars`" >nul && cl /nologo /O2 /GS- /utf-8 /W4 /DUNICODE /D_UNICODE p0_gui_driver.c /link /Brepro /SUBSYSTEM:CONSOLE user32.lib /MACHINE:X64 /OUT:p0_gui_driver.exe"
if ($LASTEXITCODE -ne 0) { throw "driver compile failed: $LASTEXITCODE" }

& cmd.exe /d /c "call `"$vcvars`" >nul && dumpbin /headers p0_gui_test.dll > dumpbin_headers.txt && dumpbin /exports p0_gui_test.dll > dumpbin_exports.txt && dumpbin /imports p0_gui_test.dll > dumpbin_imports.txt"
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed: $LASTEXITCODE" }

Write-Host ("SHA256 p0_gui_test.dll  " + (Get-Sha256Hex "p0_gui_test.dll"))
Write-Host ("SHA256 p0_gui_driver.exe " + (Get-Sha256Hex "p0_gui_driver.exe"))
Write-Host ("SIZE p0_gui_test.dll  " + ([System.IO.FileInfo]::new("p0_gui_test.dll").Length))
Write-Host ("SIZE p0_gui_driver.exe " + ([System.IO.FileInfo]::new("p0_gui_driver.exe").Length))
Write-Host "BUILD OK"
exit 0
