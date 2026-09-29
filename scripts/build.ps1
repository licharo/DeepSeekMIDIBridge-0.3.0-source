<#
    Configure + build the DeepSeek MIDI Bridge plugins and the verification suite.

    Usage:
        pwsh -File scripts/build.ps1                # release build of everything
        pwsh -File scripts/build.ps1 -Target tests  # only the test executable
        pwsh -File scripts/build.ps1 -Clean
#>
param(
    [string] $Config = "Release",
    [ValidateSet("all", "tests", "cli", "fx", "midi", "installer")]
    [string] $Target = "all",
    [switch] $Clean
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build"

$vsRoot = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
$cmake = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = Join-Path $vsRoot "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }
if (-not (Test-Path $cmake))  { throw "cmake.exe not found at $cmake" }

# import the MSVC environment into this PowerShell process
Write-Host "==> importing MSVC environment" -ForegroundColor Cyan
$envLines = & cmd.exe /c "call `"$vcvars`" >nul 2>&1 && set"
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') {
        Set-Item -Path ("env:" + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
    }
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "==> removing $buildDir" -ForegroundColor Yellow
    Remove-Item $buildDir -Recurse -Force
}

$generator = @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=$Config")
if (Test-Path $ninja) { $generator += "-DCMAKE_MAKE_PROGRAM=$ninja" }

if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
    Write-Host "==> configuring" -ForegroundColor Cyan
    & $cmake -S $root -B $buildDir @generator
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
}

$targets = switch ($Target) {
    "all"       { @("DeepSeekMidiBridgeFx_VST3", "DeepSeekMidiBridgeMidi_VST3", "DeepSeekMidiBridgeFx_Standalone", "DeepSeekMidiBridgeTests", "DeepSeekMidiBridgeCli", "DmbInstaller") }
    "tests"     { @("DeepSeekMidiBridgeTests") }
    "cli"       { @("DeepSeekMidiBridgeCli") }
    "fx"        { @("DeepSeekMidiBridgeFx_VST3", "DeepSeekMidiBridgeFx_Standalone") }
    "midi"      { @("DeepSeekMidiBridgeMidi_VST3") }
    "installer" { @("DmbInstaller") }
}

foreach ($t in $targets) {
    Write-Host "==> building $t" -ForegroundColor Cyan
    & $cmake --build $buildDir --target $t
    if ($LASTEXITCODE -ne 0) { throw "build failed: $t" }
}

Write-Host "==> done" -ForegroundColor Green
Get-ChildItem -Path $buildDir -Recurse -Include *.vst3,*.exe -ErrorAction SilentlyContinue |
    Select-Object -Expand FullName