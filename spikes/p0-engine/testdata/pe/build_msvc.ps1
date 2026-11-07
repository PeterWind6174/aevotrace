param(
    [string]$OutDir = "C:\Users\PeterWind\AevotraceP0\P0-PE-GUI-01"
)
$ErrorActionPreference = "Stop"

# Fixed input identity for P0-PE-GUI-01.  The freshly built DLLs below must
# byte-match these values (and the repository SHA256SUMS); otherwise the build
# fails instead of running a reference against a different binary.
$ExpectedDllSha = [ordered]@{
    "aevguest.dll" = "1f82b67978dc13e1e31c1262497a9fe186d39f637949b94408c646d4b11152b0"
    "aevhost.dll"  = "0eabd013127ef5cc4e73c149aa0988b3d88dbb009d491c0c895e7a0d479757d1"
}

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
    "CMD $command" | Out-File -Encoding ascii -Append build.log
    cmd.exe /c "$command >> build.log 2>&1"
    $code = $LASTEXITCODE
    "EXIT $code  $what" | Out-File -Encoding ascii -Append build.log
    if ($code -ne 0) { throw "$what failed: exit $code" }
}
function Run-To([string]$command, [string]$file, [string]$what) {
    "CMD $command > $file" | Out-File -Encoding ascii -Append build.log
    cmd.exe /c "$command > $file 2>&1"
    $code = $LASTEXITCODE
    "EXIT $code  $what" | Out-File -Encoding ascii -Append build.log
    if ($code -ne 0) { throw "$what failed: exit $code" }
}
# Windows PowerShell 5.1 started through a PowerShell 7 environment can inherit
# PS7 module paths and fail to autoload Microsoft.PowerShell.Utility cmdlets such
# as Get-FileHash.  Use the .NET API so SHA verification never depends on that.
function Get-Sha256Hex([string]$file) {
    $path = (Resolve-Path -LiteralPath $file).ProviderPath
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            $hash = $sha.ComputeHash($stream)
        } finally {
            $stream.Dispose()
        }
    } finally {
        $sha.Dispose()
    }
    return ([System.BitConverter]::ToString($hash) -replace '-', '').ToLowerInvariant()
}
function Assert-FixedDll([string]$file) {
    $actual = Get-Sha256Hex $file
    "$actual  $file" | Out-File -Encoding ascii -Append build.log
    $expected = $ExpectedDllSha[$file]
    if ($actual -ne $expected) {
        "SHA_MISMATCH file=$file expected=$expected actual=$actual" |
            Out-File -Encoding ascii -Append build.log
        throw "fixed input SHA mismatch: $file expected $expected actual $actual"
    }
}

try {
    "== build environment ==" | Out-File -Encoding ascii build.log
    "OutDir=$OutDir" | Out-File -Encoding ascii -Append build.log
    "Src=$src" | Out-File -Encoding ascii -Append build.log
    "PSVersion=$($PSVersionTable.PSVersion)" | Out-File -Encoding ascii -Append build.log
    "vcvars=$vcvars" | Out-File -Encoding ascii -Append build.log

    "== tools ==" | Out-File -Encoding ascii -Append build.log
    Run-Native "cl 2>&1 | findstr /C:`"Version`"" "cl version"
    Run-Native "link 2>&1 | findstr /C:`"Linker`"" "link version"

    "== fixed input identity (repository SHA256SUMS) ==" |
        Out-File -Encoding ascii -Append build.log
    $sumsPath = Join-Path $src "SHA256SUMS"
    if (-not (Test-Path $sumsPath)) { throw "SHA256SUMS not found: $sumsPath" }
    $sums = @{}
    foreach ($line in Get-Content $sumsPath) {
        if ($line -match '^([0-9a-fA-F]{64})\s{2}(.+)$') {
            $sums[$matches[2].Trim()] = $matches[1].ToLower()
        }
    }
    foreach ($name in $ExpectedDllSha.Keys) {
        if (-not $sums.ContainsKey($name)) { throw "SHA256SUMS missing $name" }
        if ($sums[$name] -ne $ExpectedDllSha[$name]) {
            throw "SHA256SUMS $name does not match frozen identity: $($sums[$name])"
        }
        "declared $($sums[$name])  $name" | Out-File -Encoding ascii -Append build.log
    }

    "== helper ==" | Out-File -Encoding ascii -Append build.log
    Run-Native "cl /nologo /c /Od /GS- /Zl /W4 /Brepro aevhost.c" "cl aevhost"
    Run-Native "link /nologo /DLL /NOENTRY /MACHINE:X64 /NODEFAULTLIB /DYNAMICBASE /FIXED:NO /Brepro /OUT:aevhost.dll /IMPLIB:aevhost.lib /DEF:aevhost.def aevhost.obj" "link aevhost"

    "== guest ==" | Out-File -Encoding ascii -Append build.log
    Run-Native "cl /nologo /c /Od /GS- /Zl /W4 /Brepro aevguest.c" "cl aevguest"
    Run-Native "link /nologo /DLL /NOENTRY /MACHINE:X64 /NODEFAULTLIB /DYNAMICBASE /FIXED:NO /Brepro /OUT:aevguest.dll /DEF:aevguest.def aevguest.obj aevhost.lib" "link aevguest"

    "== verify fixed input bytes ==" | Out-File -Encoding ascii -Append build.log
    Assert-FixedDll "aevguest.dll"
    Assert-FixedDll "aevhost.dll"
    "fixed DLL identity OK" | Out-File -Encoding ascii -Append build.log

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

    $env:PATH = "$OutDir;$env:PATH"
    "== run reference (legacy no-arg mode) ==" | Out-File -Encoding ascii -Append build.log
    Run-To ".\aevguest_ref.exe" "reference_stdout.txt" "reference run"
    "reference exit=0" | Out-File -Encoding ascii -Append build.log

    "== run reference (stateful mode) ==" | Out-File -Encoding ascii -Append build.log
    Run-To ".\aevguest_ref.exe stateful" "reference_stateful_stdout.txt" "stateful reference run"
    "stateful reference exit=0" | Out-File -Encoding ascii -Append build.log

    "== hashes ==" | Out-File -Encoding ascii -Append build.log
    $files = @("aevguest.dll", "aevhost.dll", "aevguest_ref.exe", "aevguest.obj",
               "aevhost.obj", "reference_stdout.txt", "reference_stateful_stdout.txt")
    foreach ($f in $files) {
        $h = Get-Sha256Hex $f
        "$h  $f" | Out-File -Encoding ascii -Append build.log
    }
    "BUILD_OK" | Out-File -Encoding ascii -Append build.log
    Write-Output "BUILD_OK"
}
catch {
    $message = "BUILD_FAILED: $($_.Exception.Message)"
    $message | Out-File -Encoding ascii -Append build.log
    [Console]::Error.WriteLine($message)
    exit 1
}
