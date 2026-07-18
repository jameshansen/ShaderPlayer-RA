# Builds the 32-bit proxy d3d9.dll. Drop the result next to SonicMania.exe to load it.
$ErrorActionPreference = "Stop"
$vc = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
$here = $PSScriptRoot
$cl = "cl /nologo /LD /EHsc /O2 /std:c++17 `"$here\d3d9_proxy.cpp`" /Fe:`"$here\d3d9.dll`" /Fo:`"$here\d3d9.obj`" /link d3d11.lib dxgi.lib /DEF:`"$here\d3d9.def`""
cmd /c "`"$vc`" x86 >nul && $cl"
if ($LASTEXITCODE -ne 0) { throw "proxy build failed" }
Write-Host "built d3d9.dll (x86)"
