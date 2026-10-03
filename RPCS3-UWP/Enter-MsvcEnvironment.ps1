[CmdletBinding()]
param([string]$Msys2Root = 'C:\msys64')
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -or -not $visualStudioPath) { throw 'Visual Studio C++ tools were not found.' }
Import-Module (Join-Path $visualStudioPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $visualStudioPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$env:MSYSTEM = 'UCRT64'
$env:MSYS2_PATH_TYPE = 'inherit'
# Preserve cl/link priority over MSYS2's unrelated link.exe utility.
$env:PATH += ";$Msys2Root\ucrt64\bin;$Msys2Root\usr\bin"
$env:CC = 'cl'
$env:CXX = 'cl'
