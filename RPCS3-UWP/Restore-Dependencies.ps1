[CmdletBinding()]
param(
    [string]$UpstreamCommit = '431b169358debead4c4113a52ceae4ce8a79d315',
    [switch]$IncludeLlvm,
    [string[]]$Only
)
$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$metadataRoot = Join-Path $repositoryRoot 'build-uwp-ucrt64\upstream-metadata'
if (-not (Test-Path -LiteralPath (Join-Path $metadataRoot '.git'))) {
    & git clone --filter=blob:none --no-checkout --depth 1 https://github.com/RPCS3/rpcs3.git $metadataRoot
    if ($LASTEXITCODE) { throw 'Could not retrieve upstream metadata.' }
}
$tree = & git -C $metadataRoot ls-tree -r $UpstreamCommit
if ($LASTEXITCODE) { throw "Upstream metadata does not contain $UpstreamCommit" }
$pins = @{}
foreach ($line in $tree) {
    if ($line -match '^160000 commit ([0-9a-f]{40})\s+(.+)$') {
        $pins[$Matches[2]] = $Matches[1]
    }
}
$modulePath = $null
$modules = @()
foreach ($line in (Get-Content -LiteralPath (Join-Path $repositoryRoot '.gitmodules'))) {
    if ($line -match '^\s*path\s*=\s*(.+?)\s*$') { $modulePath = $Matches[1] }
    if ($line -match '^\s*url\s*=\s*(.+?)\s*$') {
        $url = $Matches[1]
        if ($url.StartsWith('../../')) { $url = 'https://github.com/' + $url.Substring(6) }
        $modules += [pscustomobject]@{ Path = $modulePath; Url = $url }
    }
}
foreach ($module in $modules) {
    if ($Only.Count -and $module.Path -notin $Only) { continue }
    if ($module.Path -eq '3rdparty/llvm/llvm' -and -not $IncludeLlvm) { continue }
    # SDL is supplied by the explicitly selected SDL3-uwp source tree.
    if ($module.Path -eq '3rdparty/libsdl-org/SDL') { continue }
    if (-not $pins.ContainsKey($module.Path)) { throw "Missing pinned revision: $($module.Path)" }
    $moduleTarget = [IO.Path]::GetFullPath((Join-Path $repositoryRoot $module.Path))
    if (-not $moduleTarget.StartsWith($repositoryRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Dependency path escapes the repository: $($module.Path)"
    }
    if (Test-Path -LiteralPath (Join-Path $moduleTarget '.git')) {
        $existingCommit = & git -C $moduleTarget rev-parse HEAD
        if ($LASTEXITCODE -eq 0 -and $existingCommit -eq $pins[$module.Path]) {
            Write-Output "Already restored: $($module.Path) $existingCommit"
            continue
        }
        throw "Existing checkout differs from requested pin: $($module.Path)"
    }
    if (Test-Path -LiteralPath $moduleTarget) {
        if ($null -ne (Get-ChildItem -LiteralPath $moduleTarget -Force | Select-Object -First 1)) {
            throw "Refusing to overwrite nonempty dependency: $moduleTarget"
        }
    }
    Write-Output "Restoring $($module.Path) at $($pins[$module.Path])"
    & git init --quiet $moduleTarget
    if ($LASTEXITCODE) { throw "git init failed: $moduleTarget" }
    & git -C $moduleTarget remote add origin $module.Url
    if ($LASTEXITCODE) { throw "git remote failed: $moduleTarget" }
    & git -C $moduleTarget fetch --quiet --depth 1 origin $pins[$module.Path]
    if ($LASTEXITCODE) { throw "git fetch failed: $moduleTarget" }
    & git -C $moduleTarget checkout --quiet --detach FETCH_HEAD
    if ($LASTEXITCODE) { throw "git checkout failed: $moduleTarget" }
}
