[CmdletBinding()]
param(
    [ValidateSet('Build', 'Rebuild', 'Clean')][string]$Action = 'Build',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('x64')][string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$coreBuild = Join-Path $repositoryRoot 'build-uwp-msvc\core'
$hostBuild = Join-Path $repositoryRoot 'build-uwp-msvc\host'
$cmake = 'C:\msys64\ucrt64\bin\cmake.exe'

if ($Action -in @('Clean', 'Rebuild')) {
    foreach ($directory in @($hostBuild, $coreBuild)) {
        if (Test-Path -LiteralPath (Join-Path $directory 'CMakeCache.txt')) {
            & $cmake --build $directory --config $Configuration --target clean -- /m:4 /verbosity:minimal
            if ($LASTEXITCODE) { exit $LASTEXITCODE }
        }
    }
    if ($Action -eq 'Clean') { exit 0 }
}

& (Join-Path $PSScriptRoot 'Build-Core.ps1') -Configuration $Configuration -ExperimentalD3D12
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'Build-Host.ps1') -Configuration $Configuration
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$certificate = Join-Path $PSScriptRoot 'RPCS3-UWP_TemporaryKey.pfx'
$package = Get-ChildItem -LiteralPath $hostBuild -Filter '*.msixbundle' -File -Recurse |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $package) {
    $package = Get-ChildItem -LiteralPath $hostBuild -Filter '*_x64.msix' -File -Recurse |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
if (-not $package) { throw 'The RPCS3-UWP MSIX package was not generated.' }
if (-not (Test-Path -LiteralPath $certificate -PathType Leaf)) {
    throw "Package certificate not found: $certificate"
}
$signTool = (Get-Command signtool.exe -ErrorAction Stop).Source
& $signTool sign /fd SHA256 /f $certificate $package.FullName
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $signTool verify /pa $package.FullName
exit $LASTEXITCODE
