# Restart Steam to clear a wedged "game is running" lock that makes Sonic Mania exit at startup.
# Exits Steam gracefully (force only as fallback), waits for it to close, relaunches, waits ready.
Get-Process SonicMania -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
Start-Sleep -Seconds 2
Get-Process SonicMania -ErrorAction SilentlyContinue | Stop-Process -Force

Start-Process "steam://exit"
Write-Host "asked Steam to exit; waiting up to 60s..."
for ($i = 0; $i -lt 60; $i++) { if (-not (Get-Process steam -ErrorAction SilentlyContinue)) { break }; Start-Sleep -Seconds 1 }
if (Get-Process steam -ErrorAction SilentlyContinue) {
    Write-Host "steam did not exit gracefully; force-closing"
    Get-Process steam -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 3
}
Write-Host "steam closed: $(-not [bool](Get-Process steam -ErrorAction SilentlyContinue))"

Start-Sleep -Seconds 3
Start-Process "steam://open/main"
Write-Host "relaunching Steam; waiting 25s to be ready..."
Start-Sleep -Seconds 25
Write-Host "steam running now: $([bool](Get-Process steam -ErrorAction SilentlyContinue))"
