param(
    [string]$Destination = (Join-Path (Split-Path -Parent $PSScriptRoot) 'tenhou'),
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$clientSource = Join-Path $projectRoot 'vendor\client'
$webSource = Join-Path $projectRoot 'build\web'
$Destination = [IO.Path]::GetFullPath($Destination)
if ($Destination.Equals($clientSource,[StringComparison]::OrdinalIgnoreCase) -or
    $Destination.StartsWith($clientSource + '\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Destination must not overwrite the bundled client source.'
}
$clientFiles = @('launcher.exe','tenhou.exe')
foreach ($name in $clientFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $clientSource $name) -PathType Leaf)) {
        throw "Bundled client file missing: $name"
    }
}
$runtimeFiles = @(
    "$projectRoot\build\$Configuration\TenhouEnhanced.exe",
    "$projectRoot\build\$Configuration\tenhou_patch.dll",
    "$projectRoot\vendor\webview2\bin\x86\WebView2Loader.dll"
)
foreach ($source in $runtimeFiles) {
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Runtime file missing: $source. Check vendor inputs and run scripts/build.ps1 first."
    }
}
foreach ($asset in @('index.html','assets\renderer.js','assets\languages.js')) {
    if (!(Test-Path -LiteralPath (Join-Path $webSource $asset) -PathType Leaf)) {
        throw 'Web output missing. Run scripts/build.ps1 first.'
    }
}
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
foreach ($name in $clientFiles) {
    $target = Join-Path $Destination $name
    if (!(Test-Path -LiteralPath $target)) {
        Copy-Item -LiteralPath (Join-Path $clientSource $name) -Destination $target
    }
}
foreach ($source in $runtimeFiles) {
    $target = Join-Path $Destination (Split-Path -Leaf $source)
    if ((Test-Path -LiteralPath $target) -and
        (Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $target).Hash) { continue }
    Copy-Item -LiteralPath $source -Destination $target
}
New-Item -ItemType Directory -Path "$Destination\web" -Force | Out-Null
Copy-Item -Path "$webSource\*" -Destination "$Destination\web" -Recurse -Force
if (!(Test-Path -LiteralPath "$Destination\config.ini")) { New-Item -ItemType File -Path "$Destination\config.ini" | Out-Null }
if (!(Test-Path -LiteralPath "$Destination\enhanced.ini")) {
    Set-Content -LiteralPath "$Destination\enhanced.ini" -Value @('[lobby]', 'webview2=1', '', '[network]', 'web3=0') -Encoding ASCII
}
New-Item -ItemType Directory -Path "$Destination\mjlog","$Destination\cache" -Force | Out-Null
Write-Output "Ready: $Destination\TenhouEnhanced.exe"
