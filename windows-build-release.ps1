# Builds, then stages a clean release\ folder in the repo root with only the files
# needed to run: the exe, its runtime DLLs, the Qt plugin dirs windeployqt selected,
# and the shaders (loaded from .\shaders beside the exe in an installed build).
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = "$root/build/Release"
$release = "$root/release"
$shaders = "$root/extern/slang-shaders"

# Build first so the release always reflects current source.
& "$root/windows-build-binaries.ps1"

if (Test-Path $release) { Remove-Item $release -Recurse -Force }
New-Item -ItemType Directory -Path $release | Out-Null

# Mirror the deployed build output, minus the runtime log the app writes there.
# The executable name is set in CMakeLists.txt as ShaderPlayer-RA
$exeName = "ShaderPlayer-RA.exe"
robocopy $build $release /MIR /XF shaderplayer.log *.log $exeName | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy (binaries) failed ($LASTEXITCODE)" }

# Shaders load from .\shaders beside the exe (see shaderRoot() in mainwindow.cpp).
robocopy $shaders "$release/shaders" /MIR /XD .git | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy (shaders) failed ($LASTEXITCODE)" }

# robocopy uses exit codes 1-7 for success-with-changes; reset so the script reports OK.
$global:LASTEXITCODE = 0
Write-Host "Release staged at $release"
