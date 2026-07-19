# Renders a synthetic clip through a matrix of shader presets and source resolutions,
# reading the pipeline's own pixel readback rather than a screenshot.
# Exit code 0 = every case produced an image.
param([string]$Config = "Release", [double]$Seconds = 8)

$ErrorActionPreference = 'Stop'
$root    = Split-Path $PSScriptRoot -Parent
$exe     = Join-Path $root "build\$Config\ShaderPlayer.exe"
$shaders = Join-Path $root 'extern\slang-shaders'
$log     = Join-Path (Split-Path $exe -Parent) 'shaderplayer.log'
if (-not (Test-Path $exe)) { throw "not built: $exe" }

# testsrc2 comes from libmpv's own libavfilter, so no sample video is needed.
$video = 'av://lavfi:testsrc2=size=320x240:rate=60'

$cases = @(
    @{ Name = 'passthrough / native'; Preset = '';                        Lines = '' }
    @{ Name = 'bilinear / native';    Preset = 'bilinear.slangp';         Lines = '' }
    @{ Name = 'crt-royale / native';  Preset = 'crt\crt-royale.slangp';   Lines = '' }
    @{ Name = 'crt-geom / native';    Preset = 'crt\crt-geom.slangp';     Lines = '' }
    @{ Name = 'crt-royale / 224p';    Preset = 'crt\crt-royale.slangp';   Lines = '224' }
    @{ Name = 'crt-royale / 240p';    Preset = 'crt\crt-royale.slangp';   Lines = '240' }
    @{ Name = 'crt-royale / 480p';    Preset = 'crt\crt-royale.slangp';   Lines = '480' }
    @{ Name = 'bilinear / 480p';      Preset = 'bilinear.slangp';         Lines = '480' }
    # No file at all: the chain still has to run and shade the no-signal screen, so this case
    # fails if "always running" ever regresses to "only while mpv is producing frames".
    @{ Name = 'no signal / royale';   Preset = 'crt\crt-royale.slangp';   Lines = '';    NoVideo = $true }
)

$dataDir = Join-Path $env:ProgramData 'ShaderPlayer-RA'
$ini     = Join-Path $dataDir 'settings.ini'
$failed  = 0

foreach ($c in $cases) {
    Get-Process ShaderPlayer -ErrorAction SilentlyContinue | Stop-Process -Force
    Remove-Item $dataDir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path $dataDir -Force | Out-Null

    # Seed settings.ini the way QSettings would have written it. Forward slashes keep the
    # INI writer from escaping the path.
    $lines = @('[shaders]')
    if ($c.Preset) { $lines += 'lastPreset=' + ((Join-Path $shaders $c.Preset) -replace '\\','/') }
    if ($c.Lines)  { $lines += 'sourceLines=' + $c.Lines }
    Set-Content -Path $ini -Value $lines -Encoding UTF8

    $argv = @('--selftest', "$Seconds")
    if (-not $c.NoVideo) { $argv = @("`"$video`"") + $argv }
    $p = Start-Process -FilePath $exe -ArgumentList $argv -PassThru
    $p.WaitForExit(60000) | Out-Null
    if (-not $p.HasExited) { $p.Kill(); Write-Host "TIMEOUT  $($c.Name)"; $failed++; continue }

    $lines = @(Get-Content $log -ErrorAction SilentlyContinue | Select-String -Pattern 'SELFTEST')
    $verdict = if ($p.ExitCode -eq 0) { 'PASS' } else { 'FAIL' }
    if ($p.ExitCode -ne 0) { $failed++ }

    Write-Host ("{0,-24} {1}" -f $c.Name, $verdict)
    foreach ($l in $lines) { Write-Host "    $($l.Line.Trim())" }
}

Write-Host ''
if ($failed) { Write-Host "$failed case(s) failed"; exit 1 }
Write-Host 'all cases rendered'
exit 0
