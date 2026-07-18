# Builds the standalone 64-bit hook worker and stages the librashader DLL beside it.
$ErrorActionPreference = "Stop"
$vc = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
$here = $PSScriptRoot
$lib = Resolve-Path "$here\..\..\extern\librashader"
$cl = "cl /nologo /EHsc /O2 /std:c++17 /I`"$lib`" `"$here\worker.cpp`" /Fe:`"$here\worker.exe`" /Fo:`"$here\worker.obj`" /link `"$lib\librashader.dll.lib`" d3d11.lib dxgi.lib"
cmd /c "`"$vc`" x64 >nul && $cl"
if ($LASTEXITCODE -ne 0) { throw "worker build failed" }
Copy-Item "$lib\librashader.dll" "$here\librashader_capi.dll" -Force
Write-Host "built worker.exe (x64)"
