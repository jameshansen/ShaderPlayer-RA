# Builds the stand-in game used by hooktest.ps1.
#
# Both bitnesses: 64-bit exercises the direct injection path, 32-bit exercises the sphelper32 detour,
# and those are different enough to be worth a target each.
$ErrorActionPreference = "Stop"
$vc = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
$here = $PSScriptRoot
$out = "$here\bin"
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Build($arch, $suffix) {
    $cl = "cl /nologo /EHsc /O2 /std:c++17 `"$here\testgame.cpp`" " +
          "/Fe:`"$out\testgame$suffix.exe`" /Fo:`"$out\testgame$suffix.obj`" " +
          "/link d3d9.lib d3d11.lib dxgi.lib opengl32.lib gdi32.lib user32.lib"
    # vcvarsall gripes about a missing vswhere on stderr but still works; that noise would otherwise
    # trip $ErrorActionPreference in any caller that pipes this script.
    cmd /c "`"$vc`" $arch >nul 2>nul && $cl"
    if ($LASTEXITCODE -ne 0) { throw "testgame$suffix build failed" }
    Write-Host "built testgame$suffix.exe ($arch)"
}

Build "x64" "64"
Build "x86" "32"
