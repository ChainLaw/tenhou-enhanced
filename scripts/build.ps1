param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++ and CMake.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Install Visual Studio Build Tools with Desktop development with C++.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S $projectRoot -B "$projectRoot\build" -A Win32
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& $cmake --build "$projectRoot\build" --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'Build failed' }
& (Join-Path $PSScriptRoot 'deploy.ps1') -Configuration $Configuration
