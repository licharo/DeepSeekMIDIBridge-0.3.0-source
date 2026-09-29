<#
    Builds the two things that go to GitHub:

        dist\DeepSeekMIDIBridge-<version>-source.zip   the complete source tree
                                                       (vendored JUCE included, no build output)
        dist\DeepSeekMIDIBridge-Setup-<version>.exe    the installer (release asset)

    The zip is what you can unpack and push, or upload through the GitHub web UI.
    Run scripts/build_installer.ps1 first if the installer is missing.

    Usage:
        pwsh -File scripts/package_for_github.ps1
#>
param(
    [string] $Version = "",
    [switch] $SkipInstaller
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$distDir = Join-Path $root "dist"

if (-not $Version) {
    $cmakeLists = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
    if ($cmakeLists -notmatch 'project\s*\(\s*DeepSeekMidiBridge\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        throw "cannot read the project version from CMakeLists.txt"
    }
    $Version = $Matches[1]
}

if (-not (Test-Path $distDir)) { New-Item -ItemType Directory -Force -Path $distDir | Out-Null }

# ---------------------------------------------------------------------------
# the installer (release asset)
# ---------------------------------------------------------------------------
$setupExe = Join-Path $distDir "DeepSeekMIDIBridge-Setup-$Version.exe"

if (-not $SkipInstaller -and -not (Test-Path -LiteralPath $setupExe)) {
    Write-Host "==> installer missing, building it" -ForegroundColor Yellow
    & (Join-Path $PSScriptRoot "build_installer.ps1")
    if ($LASTEXITCODE -ne 0) { throw "installer build failed" }
}

# ---------------------------------------------------------------------------
# the source zip
# ---------------------------------------------------------------------------
$staging = Join-Path ([System.IO.Path]::GetTempPath()) "dmb_github_$Version"
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
New-Item -ItemType Directory -Force -Path $staging | Out-Null

Write-Host "==> staging source tree" -ForegroundColor Cyan

$include = @(
    "CMakeLists.txt", "README.md", "CHANGELOG.md", "LICENSE", ".gitignore", ".gitattributes",
    "source", "tools", "scripts", "installer", "docs", "third_party"
)

$excludePatterns = @(
    '\\build\\', '\\dist\\', '\\\.vs\\', '\\\.git\\', '\\out\\',
    '\.log$', '\.tmp$', '\.zip$', '\.exe$', '\.obj$', '\.pdb$', '\.ilk$', '\.exp$', '\.lib$'
)

foreach ($item in $include) {
    $path = Join-Path $root $item
    if (-not (Test-Path -LiteralPath $path)) { Write-Warning "missing: $item"; continue }

    if (Test-Path -LiteralPath $path -PathType Leaf) {
        Copy-Item -LiteralPath $path -Destination (Join-Path $staging $item) -Force
        continue
    }

    Get-ChildItem -LiteralPath $path -Recurse -File -Force | ForEach-Object {
        $relative = $_.FullName.Substring($root.Length + 1)
        foreach ($pattern in $excludePatterns) {
            if ($relative -match $pattern) { return }
        }

        $target = Join-Path $staging $relative
        $targetDir = Split-Path -Parent $target
        if (-not (Test-Path -LiteralPath $targetDir)) { New-Item -ItemType Directory -Force -Path $targetDir | Out-Null }
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
    }
}

$zip = Join-Path $distDir "DeepSeekMIDBIBridge-$Version-source.zip".Replace("DeepSeekMIDBIBridge", "DeepSeekMIDIBridge")
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }

# Build the archive entry by entry: the ZIP spec requires forward slashes, and
# ZipFile::CreateFromDirectory writes Windows separators on this framework.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$stagingFull = (Resolve-Path -LiteralPath $staging).Path
$stream = [System.IO.File]::Create($zip)

try {
    $archive = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create)

    try {
        Get-ChildItem -LiteralPath $staging -Recurse -File -Force | ForEach-Object {
            $relative = $_.FullName.Substring($stagingFull.Length + 1).Replace('\', '/')
            $entry = $archive.CreateEntry($relative, [System.IO.Compression.CompressionLevel]::Optimal)
            $entryStream = $entry.Open()
            $input = [System.IO.File]::OpenRead($_.FullName)

            try { $input.CopyTo($entryStream) }
            finally { $input.Dispose(); $entryStream.Dispose() }
        }
    }
    finally { $archive.Dispose() }
}
finally { $stream.Dispose() }

$stagedFiles = (Get-ChildItem -LiteralPath $staging -Recurse -File | Measure-Object).Count
$stagedMb = [math]::Round(((Get-ChildItem -LiteralPath $staging -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)

Remove-Item -LiteralPath $staging -Recurse -Force

Write-Host ""
Write-Host "==> release files ready" -ForegroundColor Green
Write-Host ("    {0}  ({1} MB zip, {2} files, {3} MB unpacked)" -f $zip, [math]::Round((Get-Item $zip).Length / 1MB, 2), $stagedFiles, $stagedMb)
if (Test-Path -LiteralPath $setupExe) {
    Write-Host ("    {0}  ({1} MB)" -f $setupExe, [math]::Round((Get-Item $setupExe).Length / 1MB, 2))
}
Write-Host ""
Write-Host "Next: unpack the zip, 'git init', commit, push - or upload the tree through the GitHub web UI."
Write-Host "Attach the .exe to a release tagged v$Version."
