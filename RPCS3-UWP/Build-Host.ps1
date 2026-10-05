[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$FrontendOnly,
    [switch]$ConfigureOnly,
    [switch]$MesaOpenGL,
    [string]$VcpkgRoot
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-MsvcEnvironment.ps1')
if (-not $VcpkgRoot) { $VcpkgRoot = $env:VCPKG_INSTALLATION_ROOT }
if (-not $VcpkgRoot) { $VcpkgRoot = Join-Path $visualStudioPath 'VC\vcpkg' }
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$hostBuild = Join-Path $repositoryRoot 'build-uwp-msvc\host'
$coreBuild = Join-Path $repositoryRoot 'build-uwp-msvc\core'
$withCore = if ($FrontendOnly) { 'OFF' } else { 'ON' }
$withMesa = if ($MesaOpenGL -and -not $FrontendOnly) { 'ON' } else { 'OFF' }
& 'C:\msys64\ucrt64\bin\cmake.exe' -S $PSScriptRoot -B $hostBuild `
    -G 'Visual Studio 18 2026' -A x64 `
    '-DCMAKE_SYSTEM_NAME=WindowsStore' '-DCMAKE_SYSTEM_VERSION=10.0' `
    '-DCMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION=10.0.26100.0' `
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake" `
    "-DVCPKG_MANIFEST_DIR=$PSScriptRoot/Frontend" `
    '-DVCPKG_TARGET_TRIPLET=x64-uwp-static' `
    "-DVCPKG_OVERLAY_TRIPLETS=$PSScriptRoot/Frontend/cmake/triplets" `
    "-DRPCS3_HOST_WITH_CORE=$withCore" `
    "-DRPCS3_HOST_MESA=$withMesa" `
    "-DRPCS3_CORE_DLL=$coreBuild/bin/rpcs3-core.dll" `
    "-DRPCS3_CORE_IMPLIB=$coreBuild/rpcs3/$Configuration/rpcs3-core.lib"
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if ($ConfigureOnly) { exit 0 }
& 'C:\msys64\ucrt64\bin\cmake.exe' --build $hostBuild --config $Configuration `
    --target RPCS3UwpApp -- /m:4 /verbosity:minimal
exit $LASTEXITCODE
