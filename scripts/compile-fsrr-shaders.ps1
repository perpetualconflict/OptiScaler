[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$shaderRoot = Join-Path $repoRoot 'OptiScaler\denoisers\dx12\shaders'
$programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
$kitsRoot = Join-Path $programFilesX86 'Windows Kits\10\bin'
$fxc = Get-ChildItem -LiteralPath $kitsRoot -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\fxc.exe' } |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1

if (-not $fxc) {
    throw 'fxc.exe was not found. Install the Windows 10/11 SDK component from Visual Studio Build Tools.'
}

$shaders = @(
    @{
        Source = 'RrCanonicalize.hlsl'
        Header = 'RrCanonicalize_Shader.h'
        Variable = 'RrCanonicalize_cso'
    },
    @{
        Source = 'RrCompose.hlsl'
        Header = 'RrCompose_Shader.h'
        Variable = 'RrCompose_cso'
    }
)

foreach ($shader in $shaders) {
    $source = Join-Path $shaderRoot $shader.Source
    $header = Join-Path $shaderRoot $shader.Header
    & $fxc /nologo /T cs_5_1 /E CSMain /O3 /Fh $header /Vn $shader.Variable $source
    if ($LASTEXITCODE -ne 0) {
        throw "Shader compilation failed for $($shader.Source)."
    }
}

Write-Host "FSR-RR shaders compiled with $fxc"
