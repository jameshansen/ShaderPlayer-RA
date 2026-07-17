# Round-trips the things we persist to %ProgramData%: close the app gracefully, relaunch with no
# arguments, and check the playlist and shader came back. Exit 0 = everything restored.
param([string]$Config = "Release")

$ErrorActionPreference = 'Stop'
$root    = Split-Path $PSScriptRoot -Parent
$exe     = Join-Path $root "build\$Config\ShaderPlayer.exe"
$shaders = ($root -replace '\\','/') + '/extern/slang-shaders'
$log     = Join-Path (Split-Path $exe -Parent) 'shaderplayer.log'
$dataDir = Join-Path $env:ProgramData 'ShaderPlayer-RA'
$failed  = 0

function Check($name, $cond) {
    if ($cond) { Write-Host "PASS  $name" } else { Write-Host "FAIL  $name"; $script:failed++ }
}

# Real files, not av:// URLs: mpv rejects "unsafe" protocols inside playlist files, so a
# restore test built on av:// would fail for reasons a real user never hits.
# Raw YUV4MPEG2 needs no encoder - libmpv's own libavformat demuxes it.
function New-TestClip($path, $frames = 90, $w = 320, $h = 240) {
    $ySize = $w * $h
    $cSize = ($w / 2) * ($h / 2)
    $y = New-Object byte[] $ySize
    for ($i = 0; $i -lt $ySize; $i++) { $y[$i] = [byte](($i * 7) % 256) }  # visible gradient
    $u = New-Object byte[] $cSize
    $v = New-Object byte[] $cSize
    for ($i = 0; $i -lt $cSize; $i++) { $u[$i] = 90; $v[$i] = 200 }

    $fs = [System.IO.File]::Create($path)
    $bw = New-Object System.IO.BinaryWriter($fs)
    $bw.Write([Text.Encoding]::ASCII.GetBytes("YUV4MPEG2 W$w H$h F30:1 Ip A1:1 C420`n"))
    for ($f = 0; $f -lt $frames; $f++) {
        $bw.Write([Text.Encoding]::ASCII.GetBytes("FRAME`n"))
        $bw.Write($y); $bw.Write($u); $bw.Write($v)
    }
    $bw.Close(); $fs.Close()
}

Get-Process ShaderPlayer -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item $dataDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $dataDir -Force | Out-Null
@('[shaders]', "lastPreset=$shaders/crt/crt-royale.slangp") |
    Set-Content -Path (Join-Path $dataDir 'settings.ini') -Encoding UTF8

# 1. Launch with two entries, then close via WM_CLOSE so closeEvent actually runs.
$clipDir = Join-Path $env:TEMP 'shaderplayer-test'
New-Item -ItemType Directory -Path $clipDir -Force | Out-Null
$clipA = Join-Path $clipDir 'clip_a.y4m'
$clipB = Join-Path $clipDir 'clip_b.y4m'
if (-not (Test-Path $clipA)) { New-TestClip $clipA }
if (-not (Test-Path $clipB)) { New-TestClip $clipB }

$media = @("`"$clipA`"", "`"$clipB`"")
$p = Start-Process -FilePath $exe -ArgumentList $media -PassThru
Start-Sleep -Seconds 12
$null = $p.CloseMainWindow()
if (-not $p.WaitForExit(20000)) { $p.Kill(); Write-Host 'FAIL  app did not close cleanly'; exit 1 }

$m3u = Join-Path $dataDir 'playlist.m3u8'
Check 'playlist.m3u8 written on close' (Test-Path $m3u)
$entries = @(Get-Content $m3u -ErrorAction SilentlyContinue | Where-Object { $_ -and $_ -notmatch '^#' })
Check 'playlist has 2 entries' ($entries.Count -eq 2)

# 2. Relaunch with no arguments; the playlist and shader should come back on their own.
$p2 = Start-Process -FilePath $exe -ArgumentList @('--selftest','8') -PassThru
$p2.WaitForExit(60000) | Out-Null
Check 'restored run rendered an image' ($p2.ExitCode -eq 0)

$lines = @(Get-Content $log -ErrorAction SilentlyContinue | Select-String -Pattern 'SELFTEST')
foreach ($l in $lines) { Write-Host "      $($l.Line.Trim())" }
Check 'playlist restored (2 entries)' ($lines -match 'playlist: 2 entries')
Check 'shader restored (crt-royale)' ($lines -match 'preset:.*crt-royale')

Write-Host ''
if ($failed) { Write-Host "$failed check(s) failed"; exit 1 }
Write-Host 'persistence round-trips'
exit 0
