<#
    Copies the built .vst3 bundles into the standard VST3 plug-in folders so that
    Ableton Live (and any other VST3 host) can find them.

    Usage:
        pwsh -File scripts/install_vst3.ps1            # install everything that was built
        pwsh -File scripts/install_vst3.ps1 -Uninstall # remove them again
#>
param(
    [switch] $Uninstall
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build"

$perUser = Join-Path $env:LOCALAPPDATA "Programs\Common\VST3"
$systemWide = Join-Path $env:ProgramFiles "Common Files\VST3"
$systemWideX86 = Join-Path ${env:ProgramFiles(x86)} "Common Files\VST3"

$bundles = Get-ChildItem -Path $buildDir -Recurse -Filter "*.vst3" -ErrorAction SilentlyContinue |
           Where-Object { $_.PSIsContainer -or $_.Extension -eq ".vst3" } |
           Select-Object -Unique

if (-not $bundles -or $bundles.Count -eq 0) {
    throw "no .vst3 bundles found in $buildDir - run scripts/build.ps1 first"
}

Write-Host "found bundles:" -ForegroundColor Cyan
$bundles | ForEach-Object { Write-Host "  $($_.FullName)" }

function Test-Writable ([string] $path) {
    try {
        if (-not (Test-Path $path)) { New-Item -ItemType Directory -Force -Path $path | Out-Null }
        $probe = Join-Path $path ".dmb_write_test"
        Set-Content -Path $probe -Value "x" -ErrorAction Stop
        Remove-Item $probe -Force
        return $true
    } catch {
        return $false
    }
}

$targets = @()

if (Test-Writable $perUser) {
    $targets += $perUser
    Write-Host "per-user VST3 folder is writable: $perUser" -ForegroundColor Green
} else {
    Write-Warning "cannot write to $perUser"
}

if (Test-Writable $systemWide) {
    $targets += $systemWide
    Write-Host "system VST3 folder is writable: $systemWide" -ForegroundColor Green
} else {
    Write-Warning "cannot write to $systemWide (start PowerShell as administrator to install there)"
}

if ($targets.Count -eq 0) {
    throw "no writable VST3 folder found"
}

foreach ($target in $targets) {
    foreach ($bundle in $bundles) {
        $destination = Join-Path $target $bundle.Name

        if ($Uninstall) {
            if (Test-Path $destination) {
                Remove-Item $destination -Recurse -Force
                Write-Host "removed  $destination" -ForegroundColor Yellow
            }
            continue
        }

        if (Test-Path $destination) { Remove-Item $destination -Recurse -Force }
        Copy-Item $bundle.FullName $destination -Recurse -Force
        Write-Host "installed $destination" -ForegroundColor Green
    }
}

if (-not $Uninstall) {
    # Ableton Live refuses to load the pure-MIDI build ("no valid audio input bus"),
    # so make sure it never ends up in a scanned folder next to the Fx build.
    foreach ($target in $targets) {
        $stale = Join-Path $target "DeepSeek MIDI Bridge (MIDI).vst3"

        if (Test-Path -LiteralPath $stale) {
            Remove-Item -LiteralPath $stale -Recurse -Force
            Write-Host "removed stale MIDI-only bundle: $stale" -ForegroundColor Yellow
        }
    }

    Write-Host ""
    Write-Host "Next steps in Ableton Live:" -ForegroundColor Cyan
    Write-Host "  1. Live > Settings > Plug-Ins > activate 'Use VST3 Plug-In System Folders' (and 'Use VST3 Plug-In Custom Folder' if you used one)"
    Write-Host "  2. press 'Rescan' (or hold Alt while clicking it for a full rescan)"
    Write-Host "  3. the plugin appears in the browser under Plug-Ins > VST3 > DeepSeek MIDI Bridge"
}
