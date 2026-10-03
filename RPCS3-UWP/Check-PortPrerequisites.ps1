[CmdletBinding()]
param(
    [string]$Msys2Root = 'C:\msys64',
    [string]$Sdl3Source = 'C:\Users\rodri\Dev1\Projetos\SDL3-uwp',
    [string]$FrontendSource
)

$ErrorActionPreference = 'Stop'
$FrontendSource = if ($FrontendSource) { $FrontendSource } else { Join-Path $PSScriptRoot 'Frontend' }
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$missing = [System.Collections.Generic.List[string]]::new()
foreach ($relativeTool in @('ucrt64.exe', 'ucrt64\bin\gcc.exe', 'ucrt64\bin\g++.exe', 'ucrt64\bin\cmake.exe', 'ucrt64\bin\ninja.exe')) {
    $toolPath = Join-Path $Msys2Root $relativeTool
    if (-not (Test-Path -LiteralPath $toolPath -PathType Leaf)) {
        $missing.Add($toolPath)
    }
}
foreach ($sourceFile in @(
    (Join-Path $Sdl3Source 'CMakeLists.txt'),
    (Join-Path $Sdl3Source 'include\SDL3\SDL.h'),
    (Join-Path $FrontendSource 'UWP-ImGuiFrontend\include\UwpImGuiFrontend\Host.h'),
    (Join-Path $PSScriptRoot 'FrontendHost\UWP-App\D3D12Renderer.cpp')
)) {
    if (-not (Test-Path -LiteralPath $sourceFile -PathType Leaf)) {
        $missing.Add($sourceFile)
    }
}

# A source archive omits Git submodules even when their directories exist.
# Do not configure against silently missing headers or substitute desktop DLLs.
$modulePaths = Get-Content -LiteralPath (Join-Path $repositoryRoot '.gitmodules') |
    ForEach-Object {
        if ($_ -match '^\s*path\s*=\s*(.+?)\s*$') { $Matches[1] }
    }
foreach ($modulePath in $modulePaths) {
    if ($modulePath -in @('3rdparty/llvm/llvm', '3rdparty/libsdl-org/SDL', '3rdparty/opencv/opencv')) {
        continue
    }
    $absoluteModulePath = Join-Path $repositoryRoot $modulePath
    $hasFiles = Test-Path -LiteralPath $absoluteModulePath -PathType Container
    if ($hasFiles) {
        $hasFiles = $null -ne (Get-ChildItem -LiteralPath $absoluteModulePath -File -Recurse | Select-Object -First 1)
    }
    if (-not $hasFiles) { $missing.Add("Empty submodule: $modulePath") }
}

if ($missing.Count -gt 0) {
    $missing | ForEach-Object { Write-Output "MISSING: $_" }
    Write-Error 'The source snapshot is incomplete. Restore its matching submodules before building the RPCS3 DLL.'
    exit 1
}
Write-Output 'Source/tool checks passed. This does not validate an AppContainer DLL or an Xbox runtime.'
