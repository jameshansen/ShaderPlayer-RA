# Downloads all third-party dependencies into extern\ so the project can be built
# from a fresh checkout (extern\ is not committed to git). See extern\README.md
# for what each piece is.
#
# Needs on the machine: git, 7-Zip (7z.exe), and Visual Studio 2022 with the C++
# workload (for lib.exe / dumpbin, used to build the MSVC import lib for libmpv).
# Run from an ordinary PowerShell; the script locates the tools itself.
#
# Re-running is cheap: each dependency is skipped if already present. Pass -Force
# to re-download and rebuild everything.
[CmdletBinding()]
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is glacial with the progress bar on.

# --- pinned versions (bump these to update; keep $mpvPkg's git hash in sync with $mpvTag) ---
$librashaderVer = 'v0.12.0'
$mpvTag = '20260610'
$mpvPkg = "mpv-dev-x86_64-$mpvTag-git-304426c.7z"

$root = $PSScriptRoot
$extern = Join-Path $root 'extern'
$tmp = Join-Path $env:TEMP 'shaderplayer-externs'
New-Item -ItemType Directory -Force $extern, $tmp | Out-Null

# --- locate tools ---
function Find-Tool($name, $fallbacks) {
    $p = (Get-Command $name -ErrorAction SilentlyContinue).Source
    if ($p) { return $p }
    foreach ($f in $fallbacks) { if (Test-Path $f) { return $f } }
    throw "$name not found. Install it or put it on PATH."
}
$git = Find-Tool 'git.exe' @()
$sevenz = Find-Tool '7z.exe' @('C:\Program Files\7-Zip\7z.exe')

# dumpbin/lib live in the MSVC toolchain; find it via vswhere and put it on PATH
# so both exes and the DLLs they need resolve.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'vswhere not found; install Visual Studio 2022 with the C++ workload.' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$msvcBin = Get-ChildItem "$vs\VC\Tools\MSVC\*\bin\Hostx64\x64" -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $msvcBin) { throw "MSVC x64 tools not found under $vs." }
$env:PATH = "$($msvcBin.FullName);$env:PATH"

function Get-File($url, $out) {
    if (Test-Path $out) { return }
    Write-Host "  downloading $(Split-Path $out -Leaf)"
    Invoke-WebRequest $url -OutFile $out
}

# --- slang-shaders (git checkout) ---
$shaders = Join-Path $extern 'slang-shaders'
if ($Force -or -not (Test-Path (Join-Path $shaders 'bilinear.slangp'))) {
    Write-Host 'slang-shaders...'
    if (Test-Path (Join-Path $shaders '.git')) { & $git -C $shaders pull --ff-only }
    else { & $git clone --depth 1 https://github.com/libretro/slang-shaders $shaders }
    if ($LASTEXITCODE -ne 0) { throw "git failed on slang-shaders ($LASTEXITCODE)" }
} else { Write-Host 'slang-shaders: present, skipping' }

# --- librashader (release zip; already ships the MSVC import lib + header) ---
$lrDir = Join-Path $extern 'librashader'
if ($Force -or -not (Test-Path (Join-Path $lrDir 'librashader.dll.lib'))) {
    Write-Host 'librashader...'
    $zip = Join-Path $tmp "librashader-$librashaderVer.zip"
    Get-File "https://github.com/SnowflakePowered/librashader/releases/download/librashader-$librashaderVer/librashader-x86_64-windows-$librashaderVer-optimized.zip" $zip
    New-Item -ItemType Directory -Force $lrDir | Out-Null
    Expand-Archive $zip -DestinationPath $lrDir -Force   # zip is flat, files land directly in $lrDir
} else { Write-Host 'librashader: present, skipping' }

# --- libmpv (7z dev package; MSVC import lib must be generated from the DLL) ---
$mpvDir = Join-Path $extern 'mpv'
if ($Force -or -not (Test-Path (Join-Path $mpvDir 'libmpv.lib'))) {
    Write-Host 'libmpv...'
    $arc = Join-Path $tmp $mpvPkg
    Get-File "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/$mpvTag/$mpvPkg" $arc
    $stage = Join-Path $tmp 'mpv-stage'
    New-Item -ItemType Directory -Force $stage | Out-Null
    & $sevenz x -o"$stage" $arc -y | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z failed on $mpvPkg ($LASTEXITCODE)" }
    New-Item -ItemType Directory -Force (Join-Path $mpvDir 'include\mpv') | Out-Null
    Copy-Item "$stage\include\mpv\*.h" (Join-Path $mpvDir 'include\mpv') -Force
    Copy-Item "$stage\libmpv-2.dll", "$stage\libmpv.dll.a" $mpvDir -Force

    # The package ships no .def and no MSVC .lib, so scrape the DLL's exports and
    # build the import lib with lib.exe (reproduces the vendored libmpv.lib exactly).
    $names = & dumpbin /exports "$mpvDir\libmpv-2.dll" |
        ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\w+)\s*$') { $Matches[1] } } |
        Where-Object { $_ }
    if ($names.Count -lt 100) { throw "libmpv export scrape looks wrong ($($names.Count) names); aborting." }
    $def = Join-Path $mpvDir 'libmpv.def'
    "EXPORTS`r`n" + ($names -join "`r`n") | Set-Content -Encoding ascii $def
    & lib /def:$def /name:libmpv-2.dll /machine:x64 "/out:$mpvDir\libmpv.lib" | Out-Null
    if (-not (Test-Path "$mpvDir\libmpv.lib")) { throw 'lib.exe did not produce libmpv.lib' }
} else { Write-Host 'libmpv: present, skipping' }

Write-Host "`nAll externs are in $extern"
