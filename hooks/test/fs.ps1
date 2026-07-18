# Toggle Sonic Mania fullscreen by sending Alt+Enter to its window, then wait, screenshot, and dump
# the proxy log. Used to test the fullscreen transition unattended. If the game ignores Alt+Enter,
# the shot/log will show no change and we know the toggle must be done another way.
$hooklog = "C:\Users\Home\AppData\Local\Temp\sp_hook.log"
Add-Type @"
using System; using System.Runtime.InteropServices;
public class K {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
$p = Get-Process SonicMania -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Host "SonicMania not running"; exit 1 }
[K]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 500
# Alt down, Enter down, Enter up, Alt up (keyup flag = 2)
[K]::keybd_event(0x12, 0, 0, [IntPtr]::Zero)
[K]::keybd_event(0x0D, 0, 0, [IntPtr]::Zero)
[K]::keybd_event(0x0D, 0, 2, [IntPtr]::Zero)
[K]::keybd_event(0x12, 0, 2, [IntPtr]::Zero)
Write-Host "sent Alt+Enter"
Start-Sleep -Seconds 6
& "$PSScriptRoot\shot.ps1" -Out "$PSScriptRoot\shot_fs.png"
Write-Host "==== proxy log (tail) ===="
Get-Content $hooklog -ErrorAction SilentlyContinue | Select-Object -Last 20
