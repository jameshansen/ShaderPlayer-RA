# Existing build process: configure + build ShaderPlayer in Release.
# The exe and its runtime DLLs (via the CMake POST_BUILD step + windeployqt)
# land in build\Release\, runnable in place.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$qt = 'C:/Qt/6.8.3/msvc2022_64'

# Prefer cmake on PATH; fall back to the default Windows install location.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = 'C:/Program Files/CMake/bin/cmake.exe' }
if (-not (Test-Path $cmake)) { throw "cmake not found on PATH or at $cmake" }

& $cmake -S $root -B "$root/build" -G "Visual Studio 17 2022" -A x64 -D CMAKE_PREFIX_PATH="$qt"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

& $cmake --build "$root/build" --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

Write-Host "Built: $root\build\Release\ShaderPlayer.exe"
