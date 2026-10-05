[CmdletBinding()]
param([int]$Jobs = 4, [switch]$Clean)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-MsvcEnvironment.ps1')
# Do not mix Git-for-Windows/runner Unix utilities with MSYS2's bash/make.
# The MSVC bin directory stays first so link.exe is the COFF linker, not
# MSYS2's filesystem utility. Force make recipes to use the same bash too.
$msvcBin = Split-Path (Get-Command cl.exe -ErrorAction Stop).Source -Parent
$env:PATH = "$msvcBin;C:\msys64\usr\bin;C:\msys64\ucrt64\bin;$env:PATH"
$env:SHELL = 'C:/msys64/usr/bin/bash.exe'
$env:LC_ALL = 'C'
$env:VSLANG = '1033'
foreach ($utility in @('bash.exe', 'make.exe', 'sed.exe', 'awk.exe')) {
    $resolved = (Get-Command $utility -ErrorAction Stop).Source
    if (-not $resolved.StartsWith('C:\msys64\usr\bin\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "FFmpeg requires MSYS2 $utility, resolved instead: $resolved"
    }
}
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$source = Join-Path $repositoryRoot 'build-uwp-msvc\ffmpeg-source'
$expectedCommit = '140fd653aed8cad774f991ba083e2d01e86420c7'
if (-not (Test-Path "$source\configure")) {
    & git clone --depth 1 --branch n8.0 https://github.com/FFmpeg/FFmpeg.git $source
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
$actualCommit = & git -C $source rev-parse HEAD
if ($LASTEXITCODE -or $actualCommit.Trim() -ne $expectedCommit) {
    throw "FFmpeg source must be n8.0 commit $expectedCommit; existing checkout was preserved"
}
$build = Join-Path $repositoryRoot 'build-uwp-msvc\ffmpeg-build'
$prefix = Join-Path $repositoryRoot 'build-uwp-msvc\ffmpeg-uwp'
New-Item -ItemType Directory -Force $build | Out-Null
$sourceUnix = (& C:\msys64\usr\bin\cygpath.exe -u $source).Trim()
$prefixUnix = (& C:\msys64\usr\bin\cygpath.exe -u $prefix).Trim()
# No desktop DLL loading, device capture, networking or hardware decoders.
# File protocol is also off: RPCS3 must feed codecs through its custom AVIO.
$arguments = @("$sourceUnix/configure", "--prefix=$prefixUnix", '--toolchain=msvc',
    '--target-os=win32', '--arch=x86_64', '--enable-static', '--disable-shared',
    '--disable-programs', '--disable-doc', '--disable-autodetect', '--disable-asm',
    '--disable-everything', '--disable-network', '--disable-devices', '--disable-hwaccels',
    '--extra-cflags=-DWINAPI_FAMILY=WINAPI_FAMILY_APP -MD', '--extra-ldflags=-APPCONTAINER',
    '--enable-decoder=aac,aac_latm,atrac3,atrac3p,atrac9,mp3,pcm_s16le,pcm_s8,h264,mpeg4,mpeg2video,mjpeg,mjpegb',
    '--enable-encoder=pcm_s16le,ac3,aac,ffv1,mpeg4,mjpeg',
    '--enable-muxer=avi,h264,mjpeg,mp4',
    '--enable-demuxer=h264,m4v,mp3,mpegvideo,mpegps,mjpeg,mov,avi,aac,pmp,oma,pcm_s16le,pcm_s8,wav',
    '--enable-parser=h264,mpeg4video,mpegaudio,mpegvideo,mjpeg,aac,aac_latm',
    '--enable-bsf=mjpeg2jpeg,h264_metadata,av1_metadata', '--enable-filter=aresample,anull,abuffer,abuffersink')
Push-Location $build
try {
    & C:\msys64\usr\bin\bash.exe @arguments
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    if ($Clean) {
        & C:\msys64\usr\bin\make.exe 'SHELL=C:/msys64/usr/bin/bash.exe' clean
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
    & C:\msys64\usr\bin\make.exe 'SHELL=C:/msys64/usr/bin/bash.exe' "-j$Jobs"
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    & C:\msys64\usr\bin\make.exe 'SHELL=C:/msys64/usr/bin/bash.exe' install
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
    foreach ($component in @('avcodec', 'avformat', 'avutil', 'avfilter', 'swscale', 'swresample')) {
        $library = Join-Path $prefix "lib\lib$component.a"
        $directives = & dumpbin.exe /nologo /directives $library
        if ($LASTEXITCODE) { throw "Cannot inspect FFmpeg CRT directives: $library" }
        if ($directives -match '/DEFAULTLIB:LIBCMT(D)?\b') {
            throw "Desktop static CRT requested by $library; rebuild with -Clean and -MD"
        }
    }
    Write-Output 'PASS: installed FFmpeg COFF libraries do not request LIBCMT'
    exit 0
} finally { Pop-Location }
