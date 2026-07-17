# Generates an MSVC import library for libmpv.
# The upstream mpv-dev package ships libmpv.dll.a (MinGW format), which link.exe cannot read,
# so we rebuild the import lib from the DLL's own export table.
$ErrorActionPreference = 'Stop'

$mpvDir = Join-Path $PSScriptRoot '..\extern\mpv' | Resolve-Path
$dll    = Join-Path $mpvDir 'libmpv-2.dll'
$def    = Join-Path $mpvDir 'libmpv.def'
$lib    = Join-Path $mpvDir 'libmpv.lib'

$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# Run dumpbin/lib inside a vcvars environment and capture the exports.
$exports = cmd /c "`"$vcvars`" >nul 2>&1 && dumpbin /exports `"$dll`""
if (-not $exports) { throw 'dumpbin produced no output' }

# dumpbin export rows look like:  ordinal hint RVA name
$names = $exports |
    Select-String -Pattern '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S+)' |
    ForEach-Object { $_.Matches[0].Groups[1].Value }

if ($names.Count -eq 0) { throw 'no exports parsed from dumpbin output' }

# LIBRARY must name the actual DLL so imports bind to libmpv-2.dll, not libmpv.dll.
$lines = @('LIBRARY libmpv-2', 'EXPORTS') + ($names | ForEach-Object { "    $_" })
Set-Content -Path $def -Value $lines -Encoding ASCII

cmd /c "`"$vcvars`" >nul 2>&1 && lib /nologo /def:`"$def`" /machine:x64 /out:`"$lib`"" | Out-Null
if (-not (Test-Path $lib)) { throw 'lib.exe did not produce libmpv.lib' }

Write-Host "generated $lib from $($names.Count) exports"
