# One-shot hook test cycle for the injection path: stop everything, optionally rebuild, launch the
# game stock, launch ShaderPlayer, inject, screenshot, dump logs.
#   .\cycle.ps1            # run with the current build
#   .\cycle.ps1 -Rebuild   # rebuild the hook DLLs and ShaderPlayer first
#
# Nothing is copied into the game folder any more. The game launches completely stock and the hook
# DLL arrives at runtime, so a failed cycle leaves nothing behind to clean up.
param([switch]$Rebuild)
$ErrorActionPreference = "Stop"
$root    = Resolve-Path "$PSScriptRoot\.."
$app     = Resolve-Path "$root\.."
$game    = "C:\Program Files (x86)\Steam\steamapps\common\Sonic Mania"
$hooklog = "$env:TEMP\sp_hook.log"
$preset  = "$app\extern\slang-shaders\crt\crt-royale.slangp"

# 1. stop game + ShaderPlayer. Close the game GRACEFULLY: force-killing RSDKv5 repeatedly corrupts
# its state and it then quits during startup. Force only as a last resort.
$g = Get-Process SonicMania -ErrorAction SilentlyContinue
if ($g) {
    $g.CloseMainWindow() | Out-Null
    for ($i = 0; $i -lt 40; $i++) { if ($g.HasExited) { break }; Start-Sleep -Milliseconds 250 }
    if (-not $g.HasExited) { $g | Stop-Process -Force }
}
Get-Process ShaderPlayer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 3   # Steam lock settle

# 2. the old proxy would still load if it were left behind, and would fight the injected DLL
if (Test-Path "$game\d3d9.dll") { Remove-Item "$game\d3d9.dll" -Force }
if (Test-Path "$game\.shaderplayer_hook") { Remove-Item "$game\.shaderplayer_hook" -Force }

# 3. optional rebuild
if ($Rebuild) { & "$root\inject\build.ps1"; & "$app\windows-build-binaries.ps1" | Out-Null }

Remove-Item $hooklog -ErrorAction SilentlyContinue

# 4. point ShaderPlayer at a known preset and source resolution, so the cycle is reproducible
# without anyone clicking through the UI first.
$ini = "$env:ProgramData\ShaderPlayer-RA\settings.ini"
if (Test-Path $ini) {
    $text = Get-Content $ini -Raw
    $text = $text -replace '(?m)^lastPreset=.*$', ("lastPreset=" + ($preset -replace '\\', '/'))
    $text = $text -replace '(?m)^sourceOverride=.*$', 'sourceOverride=@Size(424 240)'
    Set-Content $ini $text -Encoding utf8
} else {
    Write-Host "no settings.ini yet; ShaderPlayer will start with whatever its defaults are"
}

# 5. launch the game through Steam, stock
Start-Process "steam://rungameid/584400"
for ($i = 0; $i -lt 40; $i++) { if (Get-Process SonicMania -ErrorAction SilentlyContinue) { break }; Start-Sleep -Milliseconds 500 }
$sm = Get-Process SonicMania -ErrorAction SilentlyContinue
if (-not $sm) { Write-Host "SonicMania never appeared"; return }
Start-Sleep -Seconds 8   # let it reach gameplay

# 6. launch ShaderPlayer (creates the control block, starts the hook worker thread)
Start-Process -FilePath "$app\build\Release\ShaderPlayer.exe"
Start-Sleep -Seconds 4

# 7. inject. Re-query the pid: Sonic Mania can re-exec during startup, so the process seen at step 5
# is not always the one that ends up rendering. Sonic Mania is 32-bit, so this goes through the
# 32-bit helper, which also switches shading on for that pid.
$sm = Get-Process SonicMania -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $sm) { Write-Host "SonicMania exited before injection"; return }
Write-Host "injecting into pid $($sm.Id)"
& "$root\inject\bin\sphelper32.exe" $sm.Id "$root\inject\bin\sphook32.dll"
Write-Host "sphelper32 exit code: $LASTEXITCODE"

# 8. self-report
Start-Sleep -Seconds 6
& "$PSScriptRoot\shot.ps1" -Out "$PSScriptRoot\shot.png"
Write-Host "==== hook log ===="
Get-Content $hooklog -ErrorAction SilentlyContinue
