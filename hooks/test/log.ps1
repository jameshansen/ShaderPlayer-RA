# Dump the hook logs. The proxy log is the oracle: a "shared textures gen <n>" line for a game
# process means the worker handshake engaged and shading is running for it.
$root = Resolve-Path "$PSScriptRoot\.."
Write-Host "==== processes (sonic/steam/worker/rsdk) ===="
Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.ProcessName -match 'sonic|mania|rsdk|worker|steam' } |
    Select-Object Id, ProcessName, @{n='RunSec';e={[int]((Get-Date)-$_.StartTime).TotalSeconds}}, MainWindowTitle |
    Sort-Object ProcessName | Format-Table -AutoSize
Write-Host "==== proxy (sp_hook.log) ===="
Get-Content "C:\Users\Home\AppData\Local\Temp\sp_hook.log" -ErrorAction SilentlyContinue
Write-Host "`n==== worker.out ===="
Get-Content "$root\worker\worker.out" -ErrorAction SilentlyContinue
Write-Host "`n==== worker.err ===="
Get-Content "$root\worker\worker.err" -ErrorAction SilentlyContinue
