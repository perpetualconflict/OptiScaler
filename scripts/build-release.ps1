[CmdletBinding()]
param(
    [switch]$SkipShaderCompile
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

# Some launchers construct a process environment containing both Path and PATH.
# MSBuild's .NET Framework tool tasks reject that case-insensitive duplicate.
$normalizedPath = $env:Path
Remove-Item Env:PATH -ErrorAction SilentlyContinue
$env:Path = $normalizedPath

if (-not $SkipShaderCompile) {
    & (Join-Path $PSScriptRoot 'compile-fsrr-shaders.ps1')
}
& (Join-Path $PSScriptRoot 'validate-fsrr-profiles.ps1')

$programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
$vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe was not found. Install Visual Studio 2022 Build Tools with Desktop development with C++.'
}

$msbuild = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' |
    Select-Object -First 1
if (-not $msbuild) {
    throw 'MSBuild was not found. Install Visual Studio 2022 Build Tools with the v143 C++ toolset.'
}

$driveLetter = 'O'
while (Get-PSDrive -Name $driveLetter -ErrorAction SilentlyContinue) {
    $driveLetter = [char]([int][char]$driveLetter + 1)
    if ($driveLetter -gt 'Z') {
        throw 'No unused drive letter is available for the space-safe build path.'
    }
}

$drive = $driveLetter + ':'
try {
    & subst $drive $repoRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Could not create temporary build drive $drive."
    }
    & $msbuild "$drive\OptiScaler.sln" /m /p:Configuration=Release /p:Platform=x64
    if ($LASTEXITCODE -ne 0) {
        throw 'OptiScaler Release x64 build failed.'
    }
}
finally {
    & subst $drive /d 2>$null
}

# Build the small, diagnostic-only HIP companion after the normal package has
# been produced. It targets RDNA 4 and relies on the AMD driver HIP runtime;
# no ROCm runtime binary is redistributed with OptiScaler.
$hipRoot = 'C:\Program Files\AMD\ROCm\7.1'
$hipcc = Join-Path $hipRoot 'bin\hipcc.exe'
$hipSource = Join-Path $repoRoot 'native\dlssd_queue_rendezvous_hip.cpp'
$hipOutputDirectory = Join-Path $repoRoot 'x64\Release\a\OptiScaler'
$hipOutput = Join-Path $hipOutputDirectory 'DlssdQueueRendezvousHip.dll'
$vsInstall = & $vswhere -latest -version '[17.0,19.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
    Select-Object -First 1
$devCmd = if ($vsInstall) { Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat' } else { $null }
if (-not (Test-Path -LiteralPath $hipcc)) {
    throw "HIP compiler was not found at $hipcc. The queue-rendezvous diagnostic companion cannot be built."
}
if (-not $devCmd -or -not (Test-Path -LiteralPath $devCmd)) {
    throw 'Visual Studio developer environment was not found for the HIP companion build.'
}
New-Item -ItemType Directory -Force -Path $hipOutputDirectory | Out-Null
$hipCommand = "call `"$devCmd`" -arch=amd64 -host_arch=amd64 -vcvars_ver=14.44 && " +
    "`"$hipcc`" --offload-arch=gfx1201 -std=c++20 -O2 -shared `"$hipSource`" " +
    '-Xlinker /EXPORT:DLSSD_RENDEZVOUS_Initialize ' +
    '-Xlinker /EXPORT:DLSSD_RENDEZVOUS_Execute ' +
    '-Xlinker /EXPORT:DLSSD_RENDEZVOUS_ExecuteWorkload ' +
    '-Xlinker /EXPORT:DLSSD_RENDEZVOUS_Synchronize ' +
    '-Xlinker /EXPORT:DLSSD_RENDEZVOUS_Destroy ' +
    '-Xlinker /EXPORT:DLSSD_RENDEZVOUS_GetLastErrorStatus ' +
    '-Xlinker /EXPORT:DLSSD_RENDEZVOUS_GetLastErrorText ' +
    "-o `"$hipOutput`""
& cmd.exe /d /c $hipCommand
if ($LASTEXITCODE -ne 0) {
    throw 'DLSS-D queue-rendezvous HIP companion build failed.'
}
Write-Host "DLSS-D queue-rendezvous HIP companion built at $hipOutput"
