# Unattended hook test across every backend.
#
#   .\hooktest.ps1                    # run with the current build
#   .\hooktest.ps1 -Rebuild           # rebuild the hook DLLs, the test game and ShaderPlayer first
#   .\hooktest.ps1 -Backends gl       # just one
#
# Each case launches the stand-in game (hooks/test/testgame.cpp) rendering through one graphics API,
# then launches ShaderPlayer with --hooktest, which presses play on that process and reports what
# actually arrived, using its own pixel readback. Exit code 0 means frames crossed the bridge,
# animated, and landed the right way up. Nothing here needs a real game, Steam, or a human looking
# at a screenshot.
param(
    [switch]$Rebuild,
    [string[]]$Backends = @('d3d9', 'dxgi', 'gl'),
    [string]$Config = 'Release',
    [string]$Preset = 'crt\crt-royale.slangp',
    [string]$Lines = '240'
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = Resolve-Path "$here\..\.."          # ShaderPlayer-RA
$exe  = Join-Path $root "build\$Config\ShaderPlayer.exe"
$hooklog = "$env:TEMP\sp_hook.log"

# D3D9 is tested 32-bit on purpose. 64-bit d3d9.dll does not use jmp thunks for its entry points, so
# the detour correctly refuses to patch them, and essentially no real D3D9 game is 64-bit anyway --
# the API predates the transition. Testing it 32-bit is testing the configuration that exists, and it
# exercises the sphelper32 injection detour into the bargain.
$bitness = @{ d3d9 = '32'; dxgi = '64'; gl = '64' }

if ($Rebuild) {
    & "$root\hooks\inject\build.ps1"
    & "$here\build.ps1"
    & "$root\windows-build-binaries.ps1" | Out-Null
}
if (-not (Test-Path $exe)) { throw "not built: $exe (run with -Rebuild)" }

# Seed the settings the run depends on, so the result does not hang on whatever was last clicked.
$dataDir = Join-Path $env:ProgramData 'ShaderPlayer-RA'
$ini     = Join-Path $dataDir 'settings.ini'
New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
$presetPath = (Join-Path $root "extern\slang-shaders\$Preset") -replace '\\', '/'
Set-Content $ini "[General]`nlastPreset=$presetPath`nsourceLines=$Lines`n" -Encoding utf8

$results = @()
foreach ($b in $Backends) {
    Write-Host "`n=== $b ===" -ForegroundColor Cyan
    $bits = $bitness[$b]
    $gameExe = "testgame$bits.exe"
    $game = Join-Path $here "bin\$gameExe"
    if (-not (Test-Path $game)) { throw "not built: $game (run with -Rebuild)" }

    Get-Process ShaderPlayer, testgame64, testgame32 -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Remove-Item $hooklog -ErrorAction SilentlyContinue

    $g = Start-Process $game -ArgumentList $b -PassThru
    Start-Sleep -Seconds 2
    if ($g.HasExited) {
        Write-Host "testgame exited immediately (exit $($g.ExitCode)); this machine cannot run $b" -ForegroundColor Yellow
        $results += [pscustomobject]@{ Backend = $b; Code = -1; Verdict = 'SKIP (game would not start)' }
        continue
    }

    # --hooktest presses play on the test game and exits with a code describing what arrived.
    $p = Start-Process $exe -ArgumentList '--hooktest', $gameExe -PassThru -Wait
    $code = $p.ExitCode
    $verdict = switch ($code) {
        0       { 'PASS' }
        5       { 'FAIL: no frames from the game' }
        6       { 'FAIL: frames frozen' }
        7       { 'FAIL: frame upside down' }
        8       { 'FAIL: test game not found' }
        default { "FAIL: exit $code" }
    }
    $colour = if ($code -eq 0) { 'Green' } else { 'Red' }
    Write-Host "$b -> $verdict" -ForegroundColor $colour
    $results += [pscustomobject]@{ Backend = $b; Code = $code; Verdict = $verdict }

    # Filter by the test game's pid rather than clearing the file. Every hooked process holds the
    # log open for its lifetime, so deleting it between runs silently fails and the leftovers read
    # as this run's numbers -- which is exactly how a stale measurement got believed once already.
    if (Test-Path $hooklog) {
        $mine = Get-Content $hooklog | Select-String "pid $($g.Id)\]"
        if ($code -ne 0) {
            Write-Host "---- hook log (pid $($g.Id)) ----" -ForegroundColor DarkGray
            $mine | Select-Object -Last 30
        }
        $mine | Select-String 'present/s' | Select-Object -Last 2 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
    }
    Stop-Process -Id $g.Id -Force -ErrorAction SilentlyContinue
}

Get-Process ShaderPlayer, testgame64, testgame32 -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host "`n==== summary ====" -ForegroundColor Cyan
$results | Format-Table -AutoSize
if ($results | Where-Object { $_.Code -gt 0 }) { exit 1 }
exit 0
