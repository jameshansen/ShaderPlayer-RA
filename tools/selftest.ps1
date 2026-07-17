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
    @{ Name = 'passthrough / native'; Preset = '';                        Size = '' }
    @{ Name = 'bilinear / native';    Preset = 'bilinear.slangp';         Size = '' }
    @{ Name = 'crt-royale / native';  Preset = 'crt\crt-royale.slangp';   Size = '' }
    @{ Name = 'crt-geom / native';    Preset = 'crt\crt-geom.slangp';     Size = '' }
    @{ Name = 'crt-royale / 256x224'; Preset = 'crt\crt-royale.slangp';   Size = '@Size(256 224)' }
    @{ Name = 'crt-royale / 320x240'; Preset = 'crt\crt-royale.slangp';   Size = '@Size(320 240)' }
    @{ Name = 'crt-royale / 640x480'; Preset = 'crt\crt-royale.slangp';   Size = '@Size(640 480)' }
    @{ Name = 'bilinear / 640x480';   Preset = 'bilinear.slangp';         Size = '@Size(640 480)' }
)

$reg = 'HKCU:\Software\ShaderPlayer\ShaderPlayer-RA\shaders'
$failed = 0

foreach ($c in $cases) {
    Get-Process ShaderPlayer -ErrorAction SilentlyContinue | Stop-Process -Force
    Remove-Item 'HKCU:\Software\ShaderPlayer' -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -Path $reg -Force | Out-Null
    if ($c.Preset) { Set-ItemProperty -Path $reg -Name 'lastPreset' -Value (Join-Path $shaders $c.Preset) }
    if ($c.Size)   { Set-ItemProperty -Path $reg -Name 'sourceOverride' -Value $c.Size }

    $p = Start-Process -FilePath $exe -ArgumentList @("`"$video`"", '--selftest', "$Seconds") -PassThru
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
