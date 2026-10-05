[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-MsvcEnvironment.ps1')
if (-not $env:GITHUB_ENV -or -not $env:RUNNER_TEMP) { throw 'This script requires a GitHub Actions runner.' }
$root = Split-Path $PSScriptRoot -Parent
$sdl = Join-Path $root '.ci\SDL3_UWP'
foreach ($file in @((Join-Path $sdl 'CMakeLists.txt'), (Join-Path $root '3rdparty\mesa\meson.build'),
    (Join-Path $env:VCToolsInstallDir 'lib\x64\store\vcruntime.lib'),
    'C:\Program Files (x86)\Windows Kits\10\App Certification Kit\SupportedAPIs-x64.xml')) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Required build dependency missing: $file" }
}
$vclibs = 'C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx'
if (-not (Test-Path -LiteralPath $vclibs)) { throw "SDK VCLibs redistributable missing: $vclibs" }
$runtime = Join-Path $env:RUNNER_TEMP 'rpcs3-uwp-vclibs'
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::ExtractToDirectory($vclibs, $runtime)
if (-not (Test-Path (Join-Path $runtime 'vcruntime140_app.dll'))) { throw 'Extracted VCLibs package does not contain the UWP runtime.' }
"RPCS3_UWP_VCLIBS_ROOT=$runtime" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
"RPCS3_SDL3_SOURCE_DIR=$sdl" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
Write-Host 'MSVC Store libraries, SDK audit database, vendored Mesa and SDL3_UWP are available.'
