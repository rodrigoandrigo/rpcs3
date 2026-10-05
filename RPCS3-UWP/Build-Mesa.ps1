[CmdletBinding()]
param([string]$Python = 'python', [int]$Jobs = 4,
    [string]$RuntimeRoot = $env:RPCS3_UWP_VCLIBS_ROOT)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-MsvcEnvironment.ps1')
$env:PATH += ';C:\msys64\usr\bin;C:\msys64\ucrt64\bin'
# Meson's native compiler checks execute small Store-CRT binaries. Make the
# installed framework visible to those checks without installing anything.
if (-not $RuntimeRoot) {
$mesaFramework = Get-AppxPackage -Name Microsoft.VCLibs.140.00 |
    Where-Object { $_.Architecture -eq 'X64' -and [version]$_.Version -ge [version]'14.0.33519.0' } |
    Sort-Object Version -Descending | Select-Object -First 1
if (-not $mesaFramework) { throw 'An installed x64 Microsoft.VCLibs.140.00 UWP framework >= 14.0.33519.0 is required for Meson compiler checks.' }
$RuntimeRoot = $mesaFramework.InstallLocation
}
if (-not (Test-Path (Join-Path $RuntimeRoot 'vcruntime140_app.dll'))) { throw "UWP runtime missing: $RuntimeRoot" }
$env:PATH += ';' + $RuntimeRoot
$mesaRepoRoot = Split-Path $PSScriptRoot -Parent
$mesaBuildRoot = Join-Path $mesaRepoRoot 'build-uwp-msvc\mesa'
$mesaSourceRoot = Join-Path $mesaRepoRoot '3rdparty\mesa'
$mesaStoreCRT = (Join-Path $env:VCToolsInstallDir 'lib\x64\store').Replace('\', '/')
if (-not (Test-Path -LiteralPath $mesaStoreCRT)) { throw "UWP C++ runtime libraries missing: $mesaStoreCRT" }
$mesaArguments = @('setup', $mesaBuildRoot, $mesaSourceRoot,
    "--native-file=$(Join-Path $PSScriptRoot 'mesa-uwp-x64.ini')",
    '-Dbuildtype=release', '-Ddefault_library=static', '-Duwp=true',
    '-Dplatforms=windows', '-Dopengl=true', '-Dgallium-drivers=d3d12',
    '-Dgallium-d3d12-graphics=enabled', '-Dgallium-d3d12-video=disabled',
    '-Dvulkan-drivers=', '-Dllvm=disabled', '-Dglx=disabled', '-Degl=disabled',
    '-Dgles1=disabled', '-Dgles2=disabled', '-Dgbm=disabled',
    '-Dshared-glapi=disabled', '-Dmicrosoft-clc=disabled', '-Dbuild-tests=false',
    '-Dgallium-wgl-dll-name=gallium_wgl', '-Dzstd=disabled',
    '-Dspirv-tools=disabled', '-Dforce_fallback_for=zlib')
# /MD still emits the usual import-library names; prefer their Store variants
# explicitly so Mesa imports *_app.dll, not the desktop VC runtime.
$mesaArguments += "-Dc_link_args=['/APPCONTAINER','WindowsApp.lib','/LIBPATH:$mesaStoreCRT']"
$mesaArguments += "-Dcpp_link_args=['/APPCONTAINER','WindowsApp.lib','/LIBPATH:$mesaStoreCRT','/MAP']"
if (Test-Path (Join-Path $mesaBuildRoot 'meson-private\coredata.dat')) {
    $mesaArguments += '--reconfigure'
}
& $Python -m mesonbuild.mesonmain @mesaArguments
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $Python -m mesonbuild.mesonmain compile -C $mesaBuildRoot -j $Jobs
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$dxil = Join-Path $env:WindowsSdkDir 'Redist\D3D\x64\dxil.dll'
if (-not (Test-Path -LiteralPath $dxil -PathType Leaf)) { throw "Windows SDK DXIL redistributable missing: $dxil" }
Copy-Item -LiteralPath $dxil -Destination (Join-Path $mesaBuildRoot 'src\gallium\targets\libgl-gdi\dxil.dll')
foreach ($artifact in @('src/gallium/targets/libgl-gdi/opengl32.lib',
    'src/gallium/targets/libgl-gdi/opengl32.dll', 'src/gallium/targets/wgl/gallium_wgl.dll',
    'src/gallium/targets/libgl-gdi/dxil.dll')) {
    if (-not (Test-Path (Join-Path $mesaBuildRoot $artifact))) { throw "Mesa build did not produce required artifact: $artifact" }
}
exit 0
