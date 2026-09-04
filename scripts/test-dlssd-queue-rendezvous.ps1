[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
    Select-Object -First 1
if (-not $vsInstall) {
    throw 'Visual Studio C++ build tools were not found.'
}
$devCmd = Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat'
$source = Join-Path $repoRoot 'native\dlssd_queue_rendezvous_probe.cpp'
$outputDirectory = Join-Path $repoRoot 'x64\Release\rendezvous-probe'
$object = Join-Path $outputDirectory 'dlssd_queue_rendezvous_probe.obj'
$output = Join-Path $outputDirectory 'dlssd_queue_rendezvous_probe.exe'
$helper = Join-Path $repoRoot 'x64\Release\a\OptiScaler\DlssdQueueRendezvousHip.dll'
if (-not (Test-Path -LiteralPath $helper)) {
    throw 'Build the Release package before running the rendezvous probe.'
}
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$command = "call `"$devCmd`" -arch=amd64 -host_arch=amd64 -vcvars_ver=14.44 && " +
    "cl.exe /nologo /std:c++20 /EHsc /W4 /WX /O2 `"$source`" /Fo`"$object`" /Fe:`"$output`" d3d12.lib dxgi.lib"
& cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) {
    throw 'DLSS-D queue-rendezvous standalone probe did not build.'
}
& $output $helper
if ($LASTEXITCODE -ne 0) {
    throw 'DLSS-D queue-rendezvous standalone probe failed.'
}
