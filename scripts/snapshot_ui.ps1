<#
    Launches the Standalone build, screenshots its window, optionally clicks a
    point inside the editor (to open the settings panel), screenshots again and
    closes the app.

    Usage:
        pwsh -File scripts/snapshot_ui.ps1
        pwsh -File scripts/snapshot_ui.ps1 -Out build/ui -NoClick
#>
param(
    [string] $Exe = "",
    [string] $Out = "build/ui",
    [int] $ClickX = 132,
    [int] $ClickY = 107,
    [switch] $NoClick
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
if (-not $Exe) {
    $Exe = Join-Path $root "build\DeepSeekMidiBridgeFx_artefacts\Release\Standalone\DeepSeek MIDI Bridge.exe"
}

if (-not (Test-Path -LiteralPath $Exe)) { throw "standalone build not found: $Exe" }

Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class DmbWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
    public static void Click(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(120);
        mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(60);
        mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
    }
}
"@

$proc = Start-Process -FilePath $Exe -PassThru
$proc.WaitForInputIdle(8000) | Out-Null
Start-Sleep -Seconds 3
$proc.Refresh()

$handle = $proc.MainWindowHandle
if ($handle -eq [IntPtr]::Zero) {
    $proc | Stop-Process -Force
    throw "standalone window did not appear"
}

[DmbWin]::SetForegroundWindow($handle) | Out-Null
Start-Sleep -Milliseconds 600

$rect = New-Object DmbWin+RECT
[DmbWin]::GetWindowRect($handle, [ref] $rect) | Out-Null
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
Write-Host "window: $w x $h at ($($rect.Left),$($rect.Top))"

function Save-Shot ([string] $path) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()

    # PrintWindow works even when another window covers the plugin
    $ok = [DmbWin]::PrintWindow($handle, $hdc, 2)

    $gfx.ReleaseHdc($hdc)

    if (-not $ok) {
        Write-Host "PrintWindow failed, falling back to a screen grab"
        $gfx.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
    }

    $full = Join-Path $root $path
    $dir = Split-Path -Parent $full
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
    $gfx.Dispose(); $bmp.Dispose()
    Write-Host "wrote $full"
}

Save-Shot "$Out-main.png"

if (-not $NoClick) {
    # click a point given in window-relative coordinates (default: the "⚙ 设置" button)
    [DmbWin]::Click(($rect.Left + $ClickX), ($rect.Top + $ClickY))
    Start-Sleep -Milliseconds 900
    Save-Shot "$Out-settings.png"
}

Start-Sleep -Milliseconds 300
if (-not $proc.HasExited) { $proc | Stop-Process -Force }
Write-Host "standalone closed"
