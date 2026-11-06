$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$ExpectedFixtureSha = "145800f4a4729c201aab2c94a74937d349e01f6ec7871658b6becd3773bd1205"
$CommittedObject = Join-Path $PSScriptRoot "aev_trace_fixture.obj"
if (-not (Test-Path $CommittedObject)) {
    throw "committed fixture object missing: $CommittedObject"
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found" }

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

# Rebuild for a diagnostic comparison only.  The driver and disassembly below
# always use the committed object, so the behavior reference is bound to the
# fixed input bytes rather than to the current compiler's object layout.
& cmd.exe /d /c "call `"$vcvars`" >nul && cl /nologo /c /O2 /GS- /Brepro /W4 /Fo:aev_trace_fixture.rebuilt.obj aev_trace_fixture.c"
if ($LASTEXITCODE -ne 0) { throw "fixture diagnostic compile failed: $LASTEXITCODE" }
& cmd.exe /d /c "call `"$vcvars`" >nul && cl /nologo /O2 /GS- /Brepro /W4 aev_trace_ref.c aev_trace_fixture.obj /link /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:aev_trace_ref.exe"
if ($LASTEXITCODE -ne 0) { throw "driver compile/link failed: $LASTEXITCODE" }
& .\aev_trace_ref.exe
if ($LASTEXITCODE -ne 0) { throw "reference run failed: $LASTEXITCODE" }
& cmd.exe /d /c "call `"$vcvars`" >nul && dumpbin /nologo /disasm aev_trace_fixture.obj > aev_trace_fixture.disasm.txt && dumpbin /nologo /relocations aev_trace_fixture.obj > aev_trace_fixture.relocs.txt"
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed: $LASTEXITCODE" }

$committedSha = Get-Sha256Hex "aev_trace_fixture.obj"
$rebuiltSha = Get-Sha256Hex "aev_trace_fixture.rebuilt.obj"
if ($committedSha -ne $ExpectedFixtureSha) {
    throw "committed object SHA mismatch: $committedSha != $ExpectedFixtureSha"
}
Write-Host ("FIXTURE_SHA256 " + $committedSha)
Write-Host ("REBUILT_SHA256 " + $rebuiltSha)
Write-Host ("REBUILT_MATCH " + ($rebuiltSha -eq $committedSha).ToString().ToLowerInvariant())
Write-Host "COMMITTED_OBJECT_BUILD_RUN_OK"
exit 0
