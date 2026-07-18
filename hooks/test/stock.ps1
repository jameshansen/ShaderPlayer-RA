# Bisect: does Sonic Mania launch to a stable running state with our proxy REMOVED? Isolates
# "our proxy broke it" from "Steam/game state is bad". Moves d3d9.dll aside, launches, polls for a
# process that survives ~8s, then restores the proxy.
$game = "C:\Program Files (x86)\Steam\steamapps\common\Sonic Mania"
Get-Process SonicMania -ErrorAction SilentlyContinue | Stop-Process -Force
for ($i = 0; $i -lt 40; $i++) { if (-not (Get-Process SonicMania -ErrorAction SilentlyContinue)) { break }; Start-Sleep -Milliseconds 250 }
Start-Sleep -Seconds 3

$moved = $false
if (Test-Path "$game\d3d9.dll") { Move-Item "$game\d3d9.dll" "$game\d3d9.dll.off" -Force; $moved = $true }
Write-Host "proxy removed: $moved. Launching stock game..."

Start-Process "steam://rungameid/584400"
$seen = $false; $firstSeen = 0; $stable = $false
for ($i = 0; $i -lt 50; $i++) {
    Start-Sleep -Seconds 1
    $p = Get-Process SonicMania -ErrorAction SilentlyContinue
    if ($p) { if (-not $seen) { $seen = $true; $firstSeen = $i }; if (($i - $firstSeen) -ge 8) { $stable = $true; break } }
    elseif ($seen) { break }   # appeared then vanished = same early-exit as with our proxy
}
$now = [bool](Get-Process SonicMania -ErrorAction SilentlyContinue)
Write-Host ("STOCK RESULT: appeared={0} survived8s={1} runningNow={2}" -f $seen, $stable, $now)

# restore the proxy (game loaded the real system d3d9, so our file is not locked)
if ($moved -and (Test-Path "$game\d3d9.dll.off")) {
    if (Test-Path "$game\d3d9.dll") { Remove-Item "$game\d3d9.dll" -Force }
    Move-Item "$game\d3d9.dll.off" "$game\d3d9.dll" -Force
    Write-Host "proxy restored"
}