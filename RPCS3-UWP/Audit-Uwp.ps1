[CmdletBinding()]
param(
    [string]$Dll,
    [string[]]$StaticLibraries,
    [string]$Report,
    [string]$PackageManifest,
    [string]$VCLibsAppx = 'C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx',
    [string]$SupportedApis = 'C:\Program Files (x86)\Windows Kits\10\App Certification Kit\SupportedAPIs-x64.xml'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-MsvcEnvironment.ps1')
$repositoryRoot = Split-Path $PSScriptRoot -Parent
if (-not $Report) { $Report = Join-Path $repositoryRoot 'build-uwp-msvc\uwp-audit.json' }
if (-not $Dll -and -not $StaticLibraries) { $Dll = Join-Path $repositoryRoot 'build-uwp-msvc\core\bin\rpcs3-core.dll' }
if (-not (Test-Path -LiteralPath $SupportedApis)) { throw "SDK API whitelist unavailable: $SupportedApis" }
[xml]$apiXml = Get-Content -LiteralPath $SupportedApis -Raw
$names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$pairs = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$sdkModules = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$ordinalCache = @{}
foreach ($api in $apiXml.MODERN_SDK_WHITELIST.APIs.API) {
    [void]$names.Add($api.Name)
    [void]$pairs.Add("$($api.ModuleName)|$($api.Name)")
    [void]$sdkModules.Add($api.ModuleName)
    if ($api.Ordinal) { [void]$pairs.Add("$($api.ModuleName)|#$($api.Ordinal)") }
}
$findings = [Collections.Generic.List[object]]::new()
$imports = [Collections.Generic.List[object]]::new()
$exports = @()
$status = 'BLOCKED'
$frameworkImports = [Collections.Generic.List[object]]::new()
$frameworkPairs = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$frameworkIdentity = $null
if ($Dll -and (Test-Path -LiteralPath $Dll) -and (Test-Path -LiteralPath $VCLibsAppx)) {
    # C++ APP CRT exports are supplied by the Microsoft UWP framework, not the
    # OS API whitelist. Inspect the actual SDK package rather than allow a DLL
    # just because its filename happens to end in _APP.dll.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $frameworkDirectory = Join-Path $repositoryRoot ('build-uwp-msvc\audit-vclibs-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $frameworkDirectory | Out-Null
    $archive = [IO.Compression.ZipFile]::OpenRead($VCLibsAppx)
    try {
        $manifestEntry = $archive.GetEntry('AppxManifest.xml')
        if (-not $manifestEntry) { throw 'VCLibs package manifest missing' }
        $reader = [IO.StreamReader]::new($manifestEntry.Open())
        try { [xml]$frameworkXml = $reader.ReadToEnd() } finally { $reader.Dispose() }
        $frameworkIdentity = $frameworkXml.Package.Identity
        if ($frameworkIdentity.Name -ne 'Microsoft.VCLibs.140.00' -or
            $frameworkIdentity.Publisher -ne 'CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US' -or
            $frameworkIdentity.ProcessorArchitecture -ne 'x64' -or
            $frameworkXml.Package.Properties.Framework -ne 'true') {
            throw 'Not the expected Microsoft x64 UWP VCLibs framework'
        }
        foreach ($entry in $archive.Entries | Where-Object { $_.FullName -match '^[^/\\]+_app\.dll$' }) {
            $destination = Join-Path $frameworkDirectory $entry.Name
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination)
            $exportLines = & dumpbin.exe /nologo /exports $destination
            if ($LASTEXITCODE) { throw "Framework export inspection failed: $destination" }
            foreach ($line in $exportLines) {
                if ($line -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)') {
                    [void]$frameworkPairs.Add($entry.Name.ToLowerInvariant() + '|' + $Matches[1])
                }
            }
        }
    } finally { $archive.Dispose() }
}
if ($StaticLibraries) {
    foreach ($library in $StaticLibraries) {
        if (-not (Test-Path -LiteralPath $library)) { throw "Library missing: $library" }
        $symbols = & dumpbin.exe /nologo /symbols $library
        if ($LASTEXITCODE) { throw "dumpbin failed: $library" }
        foreach ($name in ($symbols | Where-Object { $_ -match '\bUNDEF\b' } | ForEach-Object {
            if ($_ -match '\|\s+__imp_(\S+)') { $Matches[1] }
        } | Sort-Object -Unique)) {
            $imports.Add(@{ file = $library; symbol = $name; supportedName = $names.Contains($name) })
            if (-not $names.Contains($name)) { $findings.Add(@{ file = $library; symbol = $name; reason = 'Unknown SDK import; review before link' }) }
        }
    }
    $status = 'PRELINK_REVIEW' # No DLL-to-symbol binding or transitive package audit yet.
}
if ($Dll) {
    if (-not (Test-Path -LiteralPath $Dll)) { $findings.Add(@{ file = $Dll; reason = 'Core DLL has not been linked' }) }
    else {
        $headers = & dumpbin.exe /nologo /headers $Dll
        if ($LASTEXITCODE) { throw 'DLL headers could not be read' }
        if (-not ($headers -match 'App Container')) { $findings.Add(@{ reason = 'Missing AppContainer PE characteristic' }) }
        $module = ''
        $lines = & dumpbin.exe /nologo /imports $Dll
        if ($LASTEXITCODE) { throw 'DLL imports could not be read' }
        foreach ($line in $lines) {
            if ($line -match '^\s+([\w.\-]+\.dll)\s*$') { $module = $Matches[1] }
            elseif ($module -and $line -match '\bOrdinal\s+(\d+)\b') {
                $name = '#' + $Matches[1]
                $supported = $pairs.Contains("$module|$name")
                $resolved = $null
                if (-not $supported -and $sdkModules.Contains($module)) {
                    if (-not $ordinalCache.ContainsKey($module)) {
                        $ordinalCache[$module] = @{}
                        $systemDll = Join-Path ([Environment]::SystemDirectory) $module
                        if (Test-Path -LiteralPath $systemDll) {
                            $systemExports = & dumpbin.exe /nologo /exports $systemDll
                            if ($LASTEXITCODE) { throw "System ordinal inspection failed: $systemDll" }
                            foreach ($entry in $systemExports) {
                                if ($entry -match '^\s+(\d+)\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)') {
                                    $ordinalCache[$module]['#' + $Matches[1]] = $Matches[2]
                                }
                            }
                        }
                    }
                    $resolved = $ordinalCache[$module][$name]
                    $supported = $resolved -and $pairs.Contains("$module|$resolved")
                }
                $imports.Add(@{ module = $module; symbol = $name; resolvedSymbol = $resolved; supported = [bool]$supported })
                if (-not $supported) { $findings.Add(@{ module = $module; symbol = $name; reason = 'Ordinal import absent from SDK whitelist' }) }
            }
            elseif ($module -and $line -match '^\s+[0-9A-F]+\s+([A-Za-z_?@][\w?@$]*)\s*$') {
                $name = $Matches[1]
                $supported = $pairs.Contains("$module|$name")
                $framework = $frameworkPairs.Contains($module.ToLowerInvariant() + '|' + $name)
                if ($framework) {
                    $frameworkImports.Add(@{ module = $module; symbol = $name })
                    $supported = $true
                }
                $imports.Add(@{ module = $module; symbol = $name; supported = $supported })
                if (-not $supported) { $findings.Add(@{ module = $module; symbol = $name; reason = 'Import absent from SDK whitelist; package dependency/API review required' }) }
            }
        }
        if (-not $imports.Count) { $findings.Add(@{ reason = 'No imports parsed; audit is inconclusive' }) }
        $exportLines = & dumpbin.exe /nologo /exports $Dll
        if ($LASTEXITCODE) { throw 'DLL exports could not be read' }
        $exports = @($exportLines | ForEach-Object { if ($_ -match '\b(rpcs3_core_\w+)\b') { $Matches[1] } } | Sort-Object -Unique)
        $apiHeader = Get-Content (Join-Path $repositoryRoot 'rpcs3\Embedded\core_api.h') -Raw
        $required = @([regex]::Matches($apiHeader, 'RPCS3_CORE_API[^\r\n;()]*\b(rpcs3_core_\w+)\s*\(') | ForEach-Object { $_.Groups[1].Value })
        if (-not $required.Count -or $required -notcontains 'rpcs3_core_version') {
            throw 'C ABI export declarations could not be parsed completely'
        }
        foreach ($name in $required) { if ($exports -notcontains $name) { $findings.Add(@{ symbol = $name; reason = 'Required C ABI export missing' }) } }
        if ($frameworkImports.Count) {
            if (-not $PackageManifest -or -not (Test-Path -LiteralPath $PackageManifest)) {
                $findings.Add(@{ reason = 'APP CRT imports resolved against SDK VCLibs, but host package dependency manifest has not been verified' })
            } else {
                [xml]$hostManifest = Get-Content -LiteralPath $PackageManifest -Raw
                $dependency = @($hostManifest.Package.Dependencies.PackageDependency | Where-Object {
                    $_.Name -eq $frameworkIdentity.Name -and $_.Publisher -eq $frameworkIdentity.Publisher
                })
                if (-not $dependency.Count -or @($dependency | Where-Object {
                    [version]$_.MinVersion -le [version]$frameworkIdentity.Version -and
                    (-not $_.ProcessorArchitecture -or $_.ProcessorArchitecture -eq 'x64')
                }).Count -eq 0) {
                    $findings.Add(@{ reason = 'Host manifest lacks a matching x64 UWP VCLibs dependency/version' })
                }
            }
        }
        $status = if ($findings.Count) { 'FAIL' } else { 'STATIC_IMPORTS_PASS' }
    }
}
@{ status = $status; whitelist = $SupportedApis; dll = $Dll; imports = @($imports.ToArray()); exports = $exports;
    packageManifest = $PackageManifest; frameworkPackage = $VCLibsAppx; frameworkImports = @($frameworkImports.ToArray());
    findings = @($findings.ToArray()); limitation = 'Static inspection only. No installed AppContainer execution, dynamic-load audit, packaging certification or Xbox validation.' } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Report
Write-Output "$status - $Report ($($findings.Count) findings)"
if ($status -eq 'BLOCKED') { exit 2 }
if ($findings.Count) { exit 1 }
exit 0
