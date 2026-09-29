<#
    Builds the one-click installer:

        dist\DeepSeekMIDIBridge-Setup-<version>.exe

    It

      1. builds the plugin and the installer executable,
      2. collects the payload (the .vst3 bundle + the readme) in build\installer_payload,
      3. appends that payload to a copy of the installer executable.

    Usage:
        pwsh -File scripts/build_installer.ps1
        pwsh -File scripts/build_installer.ps1 -SkipBuild
#>
param(
    [string] $Config = "Release",
    [switch] $SkipBuild
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build"
$distDir = Join-Path $root "dist"

# version straight out of CMakeLists.txt so everything stays in sync
$cmakeLists = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
if ($cmakeLists -notmatch 'project\s*\(\s*DeepSeekMidiBridge\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "cannot read the project version from CMakeLists.txt"
}
$version = $Matches[1]

if (-not $SkipBuild) {
    Write-Host "==> building plugin + installer" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot "build.ps1") -Config $Config -Target fx
    if ($LASTEXITCODE -ne 0) { throw "plugin build failed" }

    & (Join-Path $PSScriptRoot "build.ps1") -Config $Config -Target installer
    if ($LASTEXITCODE -ne 0) { throw "installer build failed" }
}

$setupExe = Join-Path $buildDir "DmbInstaller_artefacts\$Config\DeepSeekMidiBridgeSetup.exe"
$bundle = Join-Path $buildDir "DeepSeekMidiBridgeFx_artefacts\$Config\VST3\DeepSeek MIDI Bridge.vst3"
$midiBundle = Join-Path $buildDir "DeepSeekMidiBridgeMidi_artefacts\$Config\VST3\DeepSeek MIDI Bridge (MIDI).vst3"

if (-not (Test-Path -LiteralPath $setupExe)) { throw "installer not built: $setupExe" }
if (-not (Test-Path -LiteralPath $bundle))   { throw "plugin bundle not built: $bundle" }

# ---------------------------------------------------------------------------
# collect the payload
# ---------------------------------------------------------------------------
$payloadDir = Join-Path $buildDir "installer_payload"

if (Test-Path -LiteralPath $payloadDir) { Remove-Item -LiteralPath $payloadDir -Recurse -Force }

$payloadVst3 = Join-Path $payloadDir "vst3"
$payloadMidi = Join-Path $payloadDir "vst3-midi"
$payloadApp = Join-Path $payloadDir "app"
New-Item -ItemType Directory -Force -Path $payloadVst3, $payloadApp | Out-Null

Write-Host "==> collecting payload" -ForegroundColor Cyan

Copy-Item -LiteralPath $bundle -Destination $payloadVst3 -Recurse -Force

# the MIDI-only flavour is optional: hosts with a MIDI effect slot (Cubase, Bitwig)
# can use it, Ableton Live refuses to load it - so it is only installed when the
# user ticks the box in the installer
if (Test-Path -LiteralPath $midiBundle) {
    New-Item -ItemType Directory -Force -Path $payloadMidi | Out-Null
    Copy-Item -LiteralPath $midiBundle -Destination $payloadMidi -Recurse -Force
} else {
    Write-Warning "MIDI-only build not found - the optional variant will be missing"
}

# the readme (localised file name, so it is looked up by extension)
$readme = Get-ChildItem -LiteralPath (Join-Path $root "installer") -Filter "*.txt" |
          Select-Object -First 1
if ($null -eq $readme) { throw "no readme found in installer folder" }
Copy-Item -LiteralPath $readme.FullName -Destination $payloadApp -Force

# the detailed manual, when it has been built
$manual = Join-Path $root "docs\使用手册.docx"
if (-not (Test-Path -LiteralPath $manual)) {
    $manual = (Get-ChildItem -LiteralPath (Join-Path $root "docs") -Filter "*.docx" -ErrorAction SilentlyContinue |
               Select-Object -First 1).FullName
}
if ($manual -and (Test-Path -LiteralPath $manual)) {
    Copy-Item -LiteralPath $manual -Destination $payloadApp -Force
}

# the (MIDI) flavour can never be loaded by Live, so it is never shipped
$stale = Join-Path $payloadVst3 "DeepSeek MIDI Bridge (MIDI).vst3"
if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Recurse -Force }

Get-ChildItem -LiteralPath $payloadDir -Recurse -File |
    ForEach-Object { "    " + $_.FullName.Substring($payloadDir.Length + 1) + "  (" + [math]::Round($_.Length / 1KB) + " KB)" }

# ---------------------------------------------------------------------------
# pack: setup exe + payload
# ---------------------------------------------------------------------------
if (-not (Test-Path $distDir)) { New-Item -ItemType Directory -Force -Path $distDir | Out-Null }

$outExe = Join-Path $distDir "DeepSeekMIDIBridge-Setup-$version.exe"
$logFile = Join-Path $buildDir "installer_pack.log"

Write-Host "==> packing $outExe" -ForegroundColor Cyan

$process = Start-Process -FilePath $setupExe `
                         -ArgumentList @("--pack", "`"$payloadDir`"", "`"$outExe`"", "--log", "`"$logFile`"") `
                         -PassThru -Wait

if (Test-Path $logFile) { Get-Content $logFile | ForEach-Object { "    $_" } }

if ($process.ExitCode -ne 0) { throw "packing failed (exit $($process.ExitCode))" }
if (-not (Test-Path -LiteralPath $outExe)) { throw "packing produced no file" }

$size = [math]::Round((Get-Item -LiteralPath $outExe).Length / 1MB, 2)

Write-Host ""
Write-Host "==> installer ready" -ForegroundColor Green
Write-Host "    $outExe  ($size MB, v$version)"
Write-Host ""
Write-Host "    silent install:   `"$outExe`" --silent --dir `"C:\Program Files\Common Files\VST3`""
Write-Host "    silent uninstall: run the installed uninstaller with --uninstall --silent"
