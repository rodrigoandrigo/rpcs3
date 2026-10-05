[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$ConfigureOnly,
    [switch]$ExperimentalD3D12,
    [switch]$MesaOpenGL,
    [string]$FfmpegRoot,
    [string]$SdlSource = $env:RPCS3_SDL3_SOURCE_DIR
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-MsvcEnvironment.ps1')
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$coreBuild = Join-Path $repositoryRoot 'build-uwp-msvc\core'
$cmake = 'C:\msys64\ucrt64\bin\cmake.exe'
$emptyPkgConfig = Join-Path $repositoryRoot 'build-uwp-msvc\empty-pkgconfig'
New-Item -ItemType Directory -Path $emptyPkgConfig -Force | Out-Null
$env:PKG_CONFIG_LIBDIR = $emptyPkgConfig
$env:PKG_CONFIG_PATH = $emptyPkgConfig
if (-not $FfmpegRoot) { $FfmpegRoot = Join-Path $repositoryRoot 'build-uwp-msvc\ffmpeg-uwp' }
if (-not $SdlSource) { $SdlSource = 'C:\Users\rodri\Dev1\Projetos\SDL3-uwp' }
if (-not (Test-Path (Join-Path $SdlSource 'CMakeLists.txt'))) { throw "SDL3_UWP source missing: $SdlSource" }
$d3d12Option = if ($ExperimentalD3D12) { 'ON' } else { 'OFF' }
$mesaOption = if ($MesaOpenGL) { 'ON' } else { 'OFF' }
if ($MesaOpenGL) { $d3d12Option = 'ON' }
& $cmake -S $repositoryRoot -B $coreBuild -G 'Visual Studio 18 2026' -A x64 `
    '-DCMAKE_SYSTEM_NAME=WindowsStore' '-DCMAKE_SYSTEM_VERSION=10.0' `
    '-DCMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION=10.0.26100.0' `
    '-DRPCS3_BUILD_CORE_DLL=ON' '-DWITH_LLVM=OFF' '-DUSE_VULKAN=OFF' `
    "-DRPCS3_UWP_FFMPEG_ROOT=$FfmpegRoot" `
    "-DRPCS3_UWP_D3D12=$d3d12Option" `
    "-DRPCS3_UWP_MESA=$mesaOption" `
    '-DUSE_LTO=OFF' '-DUSE_NATIVE_INSTRUCTIONS=OFF' '-DUSE_FAUDIO=OFF' `
    '-DUSE_SYSTEM_ZLIB=OFF' '-DUSE_SYSTEM_CURL=OFF' '-DUSE_SYSTEM_LIBUSB=OFF' `
    '-DUSE_LIBEVDEV=OFF' '-DUSE_SDL=ON' '-DUSE_SYSTEM_SDL=OFF' `
    "-DRPCS3_SDL3_SOURCE_DIR=$SdlSource" `
    '-DSDL_SHARED=OFF' '-DSDL_STATIC=ON' '-DSDL_TESTS=OFF' `
    '-DSDL_HIDAPI_LIBUSB=OFF' '-DSDL_VULKAN=OFF' '-DSDL_RENDER_VULKAN=OFF' `
    '-DSDL_OPENGL=OFF' '-DSDL_OPENGLES=OFF' '-DSDL_CEMU_UWP=ON' `
    '-DSDL_VIDEO=OFF' '-DSDL_RENDER=OFF' '-DSDL_GPU=OFF' '-DSDL_CAMERA=OFF' `
    '-DSDL_AUDIO=OFF' '-DSDL_POWER=OFF' '-DSDL_DIALOG=OFF' '-DSDL_HIDAPI=OFF' `
    '-DSDL_HAPTIC=OFF' '-DSDL_SENSOR=OFF' '-DSDL_TRAY=OFF'
if ($LASTEXITCODE) { exit $LASTEXITCODE }
if ($ConfigureOnly) { exit 0 }
& $cmake --build $coreBuild --config $Configuration --target rpcs3_core -- /m:4 /verbosity:minimal
exit $LASTEXITCODE
