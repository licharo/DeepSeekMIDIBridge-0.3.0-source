<#
    Launches an executable, waits for its window and saves a screenshot of it
    (PrintWindow, so it also works when the window is behind another one).

    Usage:
        pwsh -File scripts/snapshot_app.ps1 -Exe dist\x.exe -Out build\shot.png
        pwsh -File scripts/snapshot_app.ps1 -Exe dist\x.exe -Arguments "--silent" -WaitSeconds 20
#>
param(
    [Parameter(Mandatory = $true)][string] $Exe,
    [Parameter(Mandatory = $true)][string] $Out,
    [string[]] $Arguments = @(),
    [int] $WaitSeconds = 6,
    [int] $ClickX = -1,
    [int] $ClickY = -1,
    [switch] $KeepOpen
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class DmbShot {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hWnd, int attr, out RECT r, int size);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
    public static void Click(int x, int y) {
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(150);
        mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(60);
        mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
    }
}
"@

$proc = if ($Arguments.Count -gt 0) {
    Start-Process -FilePath $Exe -ArgumentList $Arguments -PassThru
} else {
    Start-Process -FilePath $Exe -PassThru
}
Start-Sleep -Seconds $WaitSeconds
$proc.Refresh()

$handle = $proc.MainWindowHandle
if ($handle -eq [IntPtr]::Zero) {
    if (-not $proc.HasExited) { $proc | Stop-Process -Force }
    throw "no window appeared for $Exe"
}

[DmbShot]::SetForegroundWindow($handle) | Out-Null
Start-Sleep -Milliseconds 400

$rect = New-Object DmbShot+RECT
[DmbShot]::GetWindowRect($handle, [ref] $rect) | Out-Null

# This PowerShell is not DPI aware, so GetWindowRect reports scaled ("virtual")
# coordinates. The DWM frame bounds are the real pixels the window occupies, which
# is what PrintWindow needs to render the whole window.
$physical = New-Object DmbShot+RECT
$dwmOk = [DmbShot]::DwmGetWindowAttribute($handle, 9, [ref] $physical, 16) -eq 0

$w = if ($dwmOk) { $physical.Right - $physical.Left } else { $rect.Right - $rect.Left }
$h = if ($dwmOk) { $physical.Bottom - $physical.Top } else { $rect.Bottom - $rect.Top }

Write-Host "window: $w x $h $(if ($dwmOk) { '(physical)' } else { '(logical)' })"

if ($ClickX -ge 0 -and $ClickY -ge 0) {
    [DmbShot]::Click(($rect.Left + $ClickX), ($rect.Top + $ClickY))
    Start-Sleep -Milliseconds 900
}

$bmp = New-Object System.Drawing.Bitmap $w, $h
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $gfx.GetHdc()
$ok = [DmbShot]::PrintWindow($handle, $hdc, 2)
$gfx.ReleaseHdc($hdc)

if (-not $ok) {
    Write-Host "PrintWindow failed - falling back to the screen"
    $gfx.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
}
$full = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Out))
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $full) | Out-Null
$bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
$gfx.Dispose(); $bmp.Dispose()

Write-Host "wrote $full"

if (-not $KeepOpen -and -not $proc.HasExited) { $proc | Stop-Process -Force }
