param([string]$OutputDirectory = (Join-Path $PSScriptRoot 'out'))
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ build tools not found.' }
$dev = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$source = Join-Path $PSScriptRoot 'dlssd_submission_gate_probe.cpp'
$object = Join-Path $OutputDirectory 'dlssd_submission_gate_probe.obj'
$output = Join-Path $OutputDirectory 'dlssd_submission_gate_probe.exe'
cmd.exe /d /c "call `"$dev`" -arch=amd64 -host_arch=amd64 && cl.exe /nologo /std:c++20 /EHsc /W4 /WX /O2 /I`"$PSScriptRoot`" `"$source`" /Fo`"$object`" /Fe:`"$output`""
if ($LASTEXITCODE -ne 0) { throw 'Submission gate probe build failed.' }
