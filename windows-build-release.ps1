# Builds, then stages a clean release\ folder in the repo root with only the files
# needed to run: the exe, its runtime DLLs, the Qt plugin dirs windeployqt selected,
# and the shaders (loaded from .\shaders beside the exe in an installed build).
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = "$root/build/Release"
$release = "$root/release"
$shaders = "$root/extern/slang-shaders"

# Build first so the release always reflects current source. The hook binaries are built by their
# own script rather than by CMake, so they have to be asked for explicitly -- and they are part of
# the product, not a dev tool: without them the Hooks tab can only report that its DLLs are missing.
& "$root/hooks/inject/build.ps1"
& "$root/windows-build-binaries.ps1"

if (Test-Path $release) { Remove-Item $release -Recurse -Force }
New-Item -ItemType Directory -Path $release | Out-Null

# Mirror the deployed build output, minus the runtime log the app writes there.
# The executable name is set in CMakeLists.txt as ShaderPlayer-RA
$exeName = "ShaderPlayer-RA.exe"
robocopy $build $release /MIR /XF shaderplayer.log *.log $exeName | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy (binaries) failed ($LASTEXITCODE)" }

# The hook DLLs and the 32-bit injection helper, which an installed build looks for beside the exe
# (see hookDir() in hookspanel.cpp; the dev tree is found through SHADERPLAYER_HOOK_DIR instead,
# which is why a missing copy here does not show up until someone runs the staged build).
#
# Both bitnesses ship: the DLL has to match the game's, and a 64-bit process cannot call
# SetWindowsHookEx into a 32-bit one, which is what sphelper32.exe is for.
$hookBin = "$root/hooks/inject/bin"
foreach ($f in @('sphook32.dll', 'sphook64.dll', 'sphelper32.exe')) {
    if (-not (Test-Path "$hookBin/$f")) { throw "missing hook binary: $hookBin/$f" }
    Copy-Item "$hookBin/$f" $release -Force
}

# The Visual C++ runtime, deployed app-local. The exe imports MSVCP140/VCRUNTIME140, which are not
# part of Windows: on a machine that has never had a Visual Studio redistributable installed the app
# does not start, and the error names a DLL rather than saying what to install.
#
# App-local rather than requiring the user to run vc_redist.exe, because that is also the only form
# that works for a Microsoft Store package, which cannot run an installer.
#
# Preferred source is the VS redist folder; not every VS install has the loose DLLs (some ship only
# the vc_redist installers), so fall back to the copies Windows keeps in System32. Same files.
$vcDlls = @('MSVCP140.dll', 'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll')
$vcRedist = Get-ChildItem "$env:ProgramFiles/Microsoft Visual Studio/2022/*/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT" `
    -Directory -ErrorAction SilentlyContinue | Select-Object -Last 1
foreach ($dll in $vcDlls) {
    $src = if ($vcRedist -and (Test-Path "$($vcRedist.FullName)/$dll")) { "$($vcRedist.FullName)/$dll" }
           elseif (Test-Path "$env:SystemRoot/System32/$dll") { "$env:SystemRoot/System32/$dll" }
           else { throw "cannot find $dll to stage; install the VC++ redistributable or its VS component" }
    Copy-Item $src $release -Force
}

# Shaders load from .\shaders beside the exe (see shaderRoot() in mainwindow.cpp).
robocopy $shaders "$release/shaders" /MIR /XD .git | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy (shaders) failed ($LASTEXITCODE)" }

# robocopy uses exit codes 1-7 for success-with-changes; reset so the script reports OK.
$global:LASTEXITCODE = 0
Write-Host "Release staged at $release"
