# Trigger Steam's "verify integrity of game files" for Sonic Mania (appid 584400). Steam runs the
# verification and re-downloads any corrupted/missing files automatically. Requires Steam running.
# We can't see Steam's progress dialog, so wait a generous window for a small game to finish.
$g = Get-Process SonicMania -ErrorAction SilentlyContinue
if ($g) { $g.CloseMainWindow() | Out-Null; Start-Sleep -Seconds 2; if (-not $g.HasExited) { $g | Stop-Process -Force } }
Start-Process "steam://validate/584400"
Write-Host "triggered steam://validate/584400; waiting 90s for verification to finish..."
Start-Sleep -Seconds 90
Write-Host "done waiting. SonicMania running now? $([bool](Get-Process SonicMania -ErrorAction SilentlyContinue))"
