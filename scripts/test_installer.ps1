<#
    End-to-end check of the installer / uninstaller.

    It

      * installs the packed setup silently into a throw-away folder,
      * verifies every file, the manifest and the "Apps & features" registry entry,
      * plants foreign files (another plugin's folder, a stray file inside our own
        bundle) and checks the uninstaller leaves them alone,
      * uninstalls silently and verifies the result.

    Usage:
        pwsh -File scripts/test_installer.ps1
        pwsh -File scripts/test_installer.ps1 -Setup dist\DeepSeekMIDIBridge-Setup-0.3.0.exe
#>
param(
    [string] $Setup = "",
    [switch] $KeepTemp
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build"

if (-not $Setup) {
    $candidate = Get-ChildItem (Join-Path $root "dist") -Filter "*-Setup-*.exe" -ErrorAction SilentlyContinue |
                 Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $candidate) { throw "no installer in dist\ - run scripts/build_installer.ps1 first" }
    $Setup = $candidate.FullName
}

$checks = 0
$failures = 0

function check ([bool] $ok, [string] $what) {
    $script:checks++
    if ($ok) { Write-Host "    ok   $what" }
    else { $script:failures++; Write-Host "    FAIL $what" -ForegroundColor Red }
}

$sandbox = Join-Path $buildDir "installer_test"
if (Test-Path -LiteralPath $sandbox) { Remove-Item -LiteralPath $sandbox -Recurse -Force }

$vst3Root = Join-Path $sandbox "VST3"
$appDir = Join-Path $sandbox "Program"
$bundle = Join-Path $vst3Root "DeepSeek MIDI Bridge.vst3"
$pluginDll = Join-Path $bundle "Contents\x86_64-win\DeepSeek MIDI Bridge.vst3"
$moduleInfo = Join-Path $bundle "Contents\Resources\moduleinfo.json"
$uninstaller = Join-Path $appDir "$([char]0x5378)$([char]0x8F7D) DeepSeek MIDI Bridge.exe"
$manifest = Join-Path $appDir "install-manifest.txt"
$regKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\DeepSeekMidiBridge"
$infoKey = "HKCU:\Software\DeepSeekMidiBridge"

New-Item -ItemType Directory -Force -Path $vst3Root, $appDir | Out-Null

# a foreign plugin that must never be touched
$foreignBundle = Join-Path $vst3Root "SomeOtherPlugin.vst3"
New-Item -ItemType Directory -Force -Path (Join-Path $foreignBundle "Contents\x86_64-win") | Out-Null
Set-Content -LiteralPath (Join-Path $foreignBundle "Contents\x86_64-win\other.dll") -Value "not ours"

Write-Host "== installer: $Setup" -ForegroundColor Cyan
Write-Host "== sandbox:   $sandbox" -ForegroundColor Cyan

# ---------------------------------------------------------------------------
Write-Host "`n== install (silent)" -ForegroundColor Cyan
$p = Start-Process -FilePath $Setup -PassThru -Wait -ArgumentList @(
        "--silent", "--dir", "`"$vst3Root`"", "--app-dir", "`"$appDir`"")
check ($p.ExitCode -eq 0) "installer exits 0 (got $($p.ExitCode))"

check (Test-Path -LiteralPath $pluginDll) "plugin binary installed"
check (Test-Path -LiteralPath $moduleInfo) "moduleinfo.json installed"
check (Test-Path -LiteralPath $manifest) "install manifest written"
check (Test-Path -LiteralPath $uninstaller) "uninstaller installed next to the app folder"
check (Test-Path $regKey) "uninstall entry registered (Apps & features)"
check ((Get-ItemProperty -Path $regKey -ErrorAction SilentlyContinue).DisplayName -like "*DeepSeek*") "uninstall entry has a display name"
check ((Get-ItemProperty -Path $infoKey -ErrorAction SilentlyContinue).Vst3Dir -eq $vst3Root) "registry remembers the plugin folder"

$manifestText = Get-Content -LiteralPath $manifest -Raw
check ($manifestText -match "DeepSeek MIDI Bridge\.vst3") "manifest lists the plugin file"
$listed = ([regex]::Matches($manifestText, "(?m)^file=")).Count
check ($listed -ge 4) "manifest lists all written files ($listed)"

# plant a file *inside* our own bundle: the uninstaller must keep it and the folder
$stray = Join-Path $bundle "user-note.txt"
Set-Content -LiteralPath $stray -Value "keep me"

# ---------------------------------------------------------------------------
Write-Host "`n== uninstall (silent, keep user data)" -ForegroundColor Cyan
$p = Start-Process -FilePath $uninstaller -PassThru -Wait -ArgumentList @(
        "--uninstall", "--silent", "--keep-user-data")
check ($p.ExitCode -eq 0) "uninstaller exits 0 (got $($p.ExitCode))"

# the uninstaller deletes itself from a copy in %TEMP% - give it a moment
for ($i = 0; $i -lt 40 -and (Test-Path -LiteralPath $uninstaller); $i++) { Start-Sleep -Milliseconds 250 }

check (-not (Test-Path -LiteralPath $pluginDll)) "plugin binary removed"
check (-not (Test-Path -LiteralPath $moduleInfo)) "moduleinfo.json removed"
check (-not (Test-Path -LiteralPath $uninstaller)) "uninstaller removed itself"
check (-not (Test-Path -LiteralPath $manifest)) "manifest removed"

check (Test-Path -LiteralPath $stray) "a foreign file inside our bundle was NOT deleted"
check (Test-Path -LiteralPath $bundle) "the bundle folder was kept (it still holds that file)"
check (Test-Path -LiteralPath (Join-Path $foreignBundle "Contents\x86_64-win\other.dll")) "another plugin was NOT touched"

# the other plugin's content is intact - and only our files were considered
$leftOver = @(Get-ChildItem -LiteralPath $bundle -Recurse -File | ForEach-Object {
    $_.FullName.Substring($bundle.Length + 1) })
check ($leftOver.Count -eq 1 -and $leftOver[0] -eq "user-note.txt") `
      "only the foreign file is left inside our old bundle (found: $($leftOver -join ', '))"

if (Test-Path $regKey) { check $false "uninstall entry removed from the registry" }
else { check $true "uninstall entry removed from the registry" }

# ---------------------------------------------------------------------------
Write-Host "`n== optional MIDI-only flavour (other hosts' MIDI effect slot)" -ForegroundColor Cyan

$midiBundle = Join-Path $vst3Root "DeepSeek MIDI Bridge (MIDI).vst3"
$midiDll = Join-Path $midiBundle "Contents\x86_64-win\DeepSeek MIDI Bridge (MIDI).vst3"

check (-not (Test-Path -LiteralPath $midiBundle)) "default install does not add the (MIDI) flavour"

$p = Start-Process -FilePath $Setup -PassThru -Wait -ArgumentList @(
        "--silent", "--with-midi", "--dir", "`"$vst3Root`"", "--app-dir", "`"$appDir`"")
check ($p.ExitCode -eq 0) "--with-midi install exits 0 (got $($p.ExitCode))"
check (Test-Path -LiteralPath $midiDll) "the (MIDI) flavour is installed when asked for"
check ((Get-Content -LiteralPath $manifest -Raw) -match "\(MIDI\)\.vst3") "the manifest knows about it (so it is removed again)"

$uninstaller2 = Join-Path $appDir "$([char]0x5378)$([char]0x8F7D) DeepSeek MIDI Bridge.exe"
$p = Start-Process -FilePath $uninstaller2 -PassThru -Wait -ArgumentList @(
        "--uninstall", "--silent", "--keep-user-data")
check ($p.ExitCode -eq 0) "uninstall of the (MIDI) flavour exits 0"
for ($i = 0; $i -lt 40 -and (Test-Path -LiteralPath $uninstaller2); $i++) { Start-Sleep -Milliseconds 250 }
check (-not (Test-Path -LiteralPath $midiBundle)) "the (MIDI) flavour is removed again"

# ---------------------------------------------------------------------------
Write-Host "`n== installer without payload / unknown mode" -ForegroundColor Cyan
$plainExe = Join-Path $buildDir "DmbInstaller_artefacts\Release\DeepSeekMidiBridgeSetup.exe"
if (Test-Path -LiteralPath $plainExe) {
    $p = Start-Process -FilePath $plainExe -PassThru -Wait -ArgumentList @(
            "--silent", "--dir", "`"$vst3Root`"", "--app-dir", "`"$appDir`"")
    check ($p.ExitCode -ne 0) "an unpacked installer refuses to install (exit $($p.ExitCode))"
} else {
    Write-Host "    skip (no unpacked setup exe)"
}

# ---------------------------------------------------------------------------
if ((Test-Path -LiteralPath $sandbox) -and -not $KeepTemp) { Remove-Item -LiteralPath $sandbox -Recurse -Force }

Write-Host ""
Write-Host "$($checks - $failures) / $checks checks passed" -ForegroundColor $(if ($failures -eq 0) { "Green" } else { "Red" })

exit $(if ($failures -eq 0) { 0 } else { 1 })
