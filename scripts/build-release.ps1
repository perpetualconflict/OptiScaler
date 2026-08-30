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
