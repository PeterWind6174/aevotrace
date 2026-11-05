$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found" }
$expected = "4f6929f7c71c82169295b6675517a839ae9533a85941e2632115bf0ef47328dc"

function Get-Sha256Hex([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = $null
    try {
        $stream = [System.IO.File]::OpenRead((Resolve-Path $Path))
        $bytes = $sha.ComputeHash($stream)
        return ([System.BitConverter]::ToString($bytes) -replace '-', '').ToLowerInvariant()
    }
    finally {
        if ($null -ne $stream) { $stream.Dispose() }
        $sha.Dispose()
    }
}

& cmd.exe /d /c "call `"$vcvars`" >nul && cl /nologo /c /O2 /GS- /Brepro /W4 /Fo:aev_callback_fixture.rebuilt.obj aev_callback_fixture.c"
if ($LASTEXITCODE -ne 0) { throw "fixture compile failed: $LASTEXITCODE" }

$committed = Get-Sha256Hex "aev_callback_fixture.obj"
$rebuilt = Get-Sha256Hex "aev_callback_fixture.rebuilt.obj"
Write-Host ("FIXTURE_SHA256 " + $committed)
Write-Host ("REBUILT_OBJECT_SHA256 " + $rebuilt)
if ($committed -ne $expected) {
    throw "committed fixture hash mismatch: got=$committed expected=$expected"
}

# Whole-object hashes include path/debug/checksum metadata, so compare the
# actual .aevtext instruction stream. The Windows reference links the
# committed object, not the freshly built one.
& cmd.exe /d /c "call `"$vcvars`" >nul && dumpbin /nologo /disasm aev_callback_fixture.obj > committed.disasm.txt && dumpbin /nologo /disasm aev_callback_fixture.rebuilt.obj > rebuilt.disasm.txt"
if ($LASTEXITCODE -ne 0) { throw "dumpbin /disasm failed: $LASTEXITCODE" }
$committedInsn = Get-Content "committed.disasm.txt" | Where-Object { $_ -match '^\s+[0-9A-Fa-f]+:' } | ForEach-Object { $_.Trim() }
$rebuiltInsn = Get-Content "rebuilt.disasm.txt" | Where-Object { $_ -match '^\s+[0-9A-Fa-f]+:' } | ForEach-Object { $_.Trim() }
$diff = Compare-Object $committedInsn $rebuiltInsn
if ($diff) {
    $diff | Format-Table | Out-String | Write-Host
    throw "rebuilt .aevtext machine code differs from committed object"
}
Write-Host ("AEVTEXT_INSNS " + $committedInsn.Count + " MATCH")

& cmd.exe /d /c "call `"$vcvars`" >nul && cl /nologo /O2 /GS- /Brepro /W4 aev_callback_ref.c aev_callback_fixture.obj /link /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:aev_callback_ref.exe"
if ($LASTEXITCODE -ne 0) { throw "driver compile/link failed: $LASTEXITCODE" }
& .\aev_callback_ref.exe
if ($LASTEXITCODE -ne 0) { throw "reference run failed: $LASTEXITCODE" }
& cmd.exe /d /c "call `"$vcvars`" >nul && dumpbin /nologo /disasm aev_callback_fixture.obj > fixture_msvc.disasm.txt && dumpbin /nologo /relocations aev_callback_fixture.obj > fixture_msvc.relocs.txt"
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed: $LASTEXITCODE" }
Write-Host "BUILD_RUN_OK"
exit 0
