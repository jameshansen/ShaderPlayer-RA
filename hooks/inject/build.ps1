# Builds the injectable hook DLL in both bitnesses, plus the 32-bit injection helper.
#
# Both are needed: the DLL must match the game's bitness, and SetWindowsHookEx must be called from a
# process of that same bitness, which 64-bit ShaderPlayer cannot do for a 32-bit game.
$ErrorActionPreference = "Stop"
$vc = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
$here = $PSScriptRoot
$out = "$here\bin"
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Build($arch, $suffix) {
    $cl = "cl /nologo /LD /EHsc /O2 /std:c++17 `"$here\sphook.cpp`" " +
          "/Fe:`"$out\sphook$suffix.dll`" /Fo:`"$out\sphook$suffix.obj`" " +
          "/link d3d9.lib d3d11.lib dxgi.lib user32.lib /DEF:`"$here\sphook.def`""
    # vcvarsall gripes about a missing vswhere on stderr but still works; that noise would otherwise
    # trip $ErrorActionPreference in any caller that pipes this script.
    cmd /c "`"$vc`" $arch >nul 2>nul && $cl"
    if ($LASTEXITCODE -ne 0) { throw "sphook$suffix build failed" }
    Write-Host "built sphook$suffix.dll ($arch)"
}

Build "x86" "32"
Build "x64" "64"

$cl = "cl /nologo /EHsc /O2 /std:c++17 `"$here\helper32.cpp`" " +
      "/Fe:`"$out\sphelper32.exe`" /Fo:`"$out\helper32.obj`" /link user32.lib"
cmd /c "`"$vc`" x86 >nul 2>nul && $cl"
if ($LASTEXITCODE -ne 0) { throw "sphelper32 build failed" }
Write-Host "built sphelper32.exe (x86)"
