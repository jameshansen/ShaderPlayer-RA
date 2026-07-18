# Screenshot the Sonic Mania window to a PNG so the shaded result can be inspected without asking.
# GDI CopyFromScreen captures the composited window, which includes our write-back. Caveat: true
# exclusive-fullscreen D3D9 is invisible to the compositor and captures black; use the worker BMP
# dump for that case.
param([string]$Out = "$PSScriptRoot\shot.png", [string]$Process = "SonicMania")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
$p = Get-Process $Process -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Host "$Process not running"; exit 1 }
$h = $p.MainWindowHandle
if ($h -eq 0) { Write-Host "no main window handle"; exit 1 }
[Win]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 200
$r = New-Object Win+RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $ht = $r.B - $r.T
if ($w -le 0 -or $ht -le 0) { Write-Host "bad window rect"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $Out ($w x $ht)"
