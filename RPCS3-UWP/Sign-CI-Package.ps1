[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-MsvcEnvironment.ps1')
if (-not $env:RUNNER_TEMP) { throw 'Runner temporary directory is required.' }
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root 'build-uwp-msvc\ci-artifacts'
$key = Join-Path $env:RUNNER_TEMP 'rpcs3-uwp-signing.pfx'
$cert = $null
$addedTrust = $false
try {
    [xml]$manifest = Get-Content (Join-Path $PSScriptRoot 'FrontendHost\UWP-App\Package.appxmanifest') -Raw
    $signingMode = 'Configured certificate'
    if (-not $env:UWP_SIGNING_PFX_BASE64) {
        $signingMode = 'Temporary development certificate (new identity key for this run)'
        $rsa = [Security.Cryptography.RSA]::Create(2048)
        $generated = $null
        try {
            $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new(
                [string]$manifest.Package.Identity.Publisher, $rsa,
                [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
            $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
            $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
                [Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true))
            $usage = [Security.Cryptography.OidCollection]::new()
            [void]$usage.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
            $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($usage, $false))
            $generated = $request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-5), [DateTimeOffset]::UtcNow.AddMonths(3))
            $env:UWP_SIGNING_PFX_PASSWORD = [guid]::NewGuid().ToString('N')
            Write-Host "::add-mask::$env:UWP_SIGNING_PFX_PASSWORD"
            [IO.File]::WriteAllBytes($key, $generated.Export(
                [Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $env:UWP_SIGNING_PFX_PASSWORD))
        } finally {
            if ($generated) { $generated.Dispose() }
            $rsa.Dispose()
        }
        Write-Warning 'Signing with a temporary development certificate. Trust the included public .cer on the target device; use a persistent PFX secret for release updates.'
    } else {
    [IO.File]::WriteAllBytes($key, [Convert]::FromBase64String($env:UWP_SIGNING_PFX_BASE64))
    }
    $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($key,
        $env:UWP_SIGNING_PFX_PASSWORD, [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
    if (-not $cert.HasPrivateKey -or $cert.Subject -ne $manifest.Package.Identity.Publisher) {
        throw 'Certificate must contain a private key and match the manifest Publisher exactly.'
    }
    if ($cert.NotAfter -le [DateTime]::Now -or $cert.NotBefore -gt [DateTime]::Now) { throw 'Signing certificate is outside its validity period.' }
    $bundle = @(Get-ChildItem (Join-Path $root 'build-uwp-msvc\host') -Recurse -Filter '*.msixbundle')
    if ($bundle.Count -ne 1) { throw "Expected exactly one newly built MSIX bundle, found $($bundle.Count)." }
    New-Item -ItemType Directory -Path $output -Force | Out-Null
    $signed = Join-Path $output $bundle[0].Name
    Copy-Item -LiteralPath $bundle[0].FullName -Destination $signed
    $signArgs = @('sign', '/fd', 'SHA256', '/f', $key)
    if ($env:UWP_SIGNING_PFX_PASSWORD) { $signArgs += @('/p', $env:UWP_SIGNING_PFX_PASSWORD) }
    $signArgs += $signed
    & signtool.exe @signArgs
    if ($LASTEXITCODE) { throw "SignTool signing failed: $LASTEXITCODE" }
    $public = Join-Path $output 'RPCS3-UWP.cer'
    [IO.File]::WriteAllBytes($public, $cert.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    $store = [Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople', 'CurrentUser')
    $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
    try {
        if (-not ($store.Certificates | Where-Object Thumbprint -eq $cert.Thumbprint)) {
            $store.Add([Security.Cryptography.X509Certificates.X509Certificate2]::new($public))
            $addedTrust = $true
        }
    } finally { $store.Close() }
    & signtool.exe verify /pa /v $signed
    if ($LASTEXITCODE) { throw "SignTool verification failed: $LASTEXITCODE" }
    Copy-Item (Join-Path $root 'build-uwp-msvc\uwp-audit.json') $output
    $framework = 'C:\Program Files (x86)\Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx'
    Copy-Item -LiteralPath $framework -Destination $output
    "$signingMode`nSubject: $($cert.Subject)`nThumbprint: $($cert.Thumbprint)`nExpires: $($cert.NotAfter.ToUniversalTime().ToString('o'))" |
        Out-File (Join-Path $output 'signing-info.txt') -Encoding utf8
    $sdlCommit = git -C (Join-Path $root '.ci\SDL3_UWP') rev-parse HEAD
    if ($LASTEXITCODE) { throw 'Could not record SDL3_UWP revision.' }
    "RPCS3=$env:GITHUB_SHA`nSDL3_UWP=$sdlCommit" | Out-File (Join-Path $output 'revisions.txt') -Encoding utf8
    Get-ChildItem $output -File | Get-FileHash -Algorithm SHA256 |
        ForEach-Object { "$($_.Hash)  $([IO.Path]::GetFileName($_.Path))" } |
        Out-File (Join-Path $output 'SHA256SUMS.txt') -Encoding utf8
} finally {
    if ($addedTrust -and $cert) {
        $store = [Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople', 'CurrentUser')
        $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        try { $store.Remove($cert) } finally { $store.Close() }
    }
    if ($cert) { $cert.Dispose() }
    if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Force }
    Remove-Item Env:\UWP_SIGNING_PFX_BASE64 -ErrorAction SilentlyContinue
    Remove-Item Env:\UWP_SIGNING_PFX_PASSWORD -ErrorAction SilentlyContinue
}
