param([string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\web'))
$ErrorActionPreference = 'Stop'
# MSBuild may inherit another PowerShell edition's module search path.
Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Utility\Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
$projectRoot = Split-Path -Parent $PSScriptRoot
$upstream = Join-Path $projectRoot 'vendor\tenhou4'
$webSource = Join-Path $projectRoot 'src\web'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$assets = Join-Path $OutputDirectory 'assets'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$pin = 'a2997e3553a101ec4b6648f62cf2a56b5d5dcb8a6fa7656d341c886176daebe1'
$upstreamScript = Join-Path $upstream 'latest.js'
if ((Get-FileHash -LiteralPath $upstreamScript -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin) {
    throw 'Review the upstream renderer before changing the pin.'
}
$source = [IO.File]::ReadAllText($upstreamScript, $utf8)
function Assert-Unique([string]$Text, [string]$Marker) {
    if ([regex]::Matches($Text, [regex]::Escape($Marker)).Count -ne 1) {
        throw ('Upstream boundary changed: ' + $Marker)
    }
}
$marker = '(function(){function f(){z(T.Bb.style,{transition:"opacity 500ms ease-out"'
Assert-Unique $source $marker
$source = $source.Substring(0, $source.IndexOf($marker, [StringComparison]::Ordinal))
# Windows owns audio and the table. Keep the upstream lobby DOM/CSS generators.
$boundaries = @(
    @('B.AudioContext=B.AudioContext||B.webkitAudioContext;', 'var Tb=function()'),
    @('z(T,function(){function f(){T.Bb.style.display="";', 'var nc=function()')
)
foreach ($pair in $boundaries) {
    Assert-Unique $source $pair[0]
    Assert-Unique $source $pair[1]
    $left = $source.IndexOf($pair[0], [StringComparison]::Ordinal)
    $right = $source.IndexOf($pair[1], $left, [StringComparison]::Ordinal)
    $source = $source.Substring(0, $left) + $source.Substring($right)
}
# The Web lobby keeps Advanced open below 1 kyu, but still excludes players
# who meet both the 7-dan and R2000 thresholds. Native entry rules stay intact.
$advancedRule = '16<=v[0]&&2E3<=v[2]?!1:9<=v[0]||60<=u'
Assert-Unique $source $advancedRule
$source = $source.Replace($advancedRule, 'Z.desktopWeb3?!(16<=v[0]&&2E3<=v[2]):(' + $advancedRule + ')')
$replacements = [ordered]@{
    'https://cdn.tenhou.net/3/res/img/bglogow_0921x0651.png' = 'assets/logo.png'
    'https://cdn.tenhou.net/4/res/img/bg_lobby0.png' = 'assets/bg_lobby0.png'
    'https://cdn.tenhou.net/4/res/img/bg_lobby1.png' = 'assets/bg_lobby1.png'
    'https://cdn.tenhou.net/4/res/img/bg_lobby2.png' = 'assets/bg_lobby2.png'
    'https://cdn.tenhou.net/3/res/font/icons_20200502.ttf' = 'assets/icons.ttf'
    'https://cdn.tenhou.net/3/res/font/cwTeXQKaiT-Medium_20200430.ttf' = 'assets/kai.ttf'
}
foreach ($replacement in $replacements.GetEnumerator()) {
    if (!$source.Contains($replacement.Key)) { throw ('Missing upstream resource: ' + $replacement.Key) }
    $source = $source.Replace($replacement.Key, $replacement.Value)
}
$adapter = [IO.File]::ReadAllText((Join-Path $webSource 'adapter.js'), $utf8).Replace("`r`n", "`n")
New-Item -ItemType Directory -Path $assets -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $webSource 'index.html') -Destination (Join-Path $OutputDirectory 'index.html') -Force
[IO.File]::WriteAllText((Join-Path $assets 'renderer.js'), $source + "`n" + $adapter + "`n})();`n", $utf8)
$languages = [ordered]@{}
foreach ($language in @('zh', 'ja', 'en')) {
    $languages[$language] = [IO.File]::ReadAllText((Join-Path $upstream ($language + '.json')), $utf8) | ConvertFrom-Json
}
$translations = @{
    '45' = '鸣牌提示音'; '46' = '右键跳过'; '47' = '右键摸切'
    '48' = '左键双击跳过'; '49' = '左键双击摸切'; '50' = 'ESC 跳过'; '51' = 'ESC 摸切'
    '204' = '显示此设备上的全部牌谱'
    '214' = @('一般', '上級', '特上', '鳳凰', '若葉', '銀', '琥珀', '孔雀')
    '290' = '比赛排名'; '291' = 'TOOLS'
}
foreach ($key in $translations.Keys) {
    $languages.zh | Add-Member -NotePropertyName $key -NotePropertyValue $translations[$key] -Force
}
foreach ($property in $languages.zh.PSObject.Properties) {
    if ($property.Value -is [string]) { $property.Value = $property.Value.Replace('凤凰', '鳳凰').Replace('雀庄', '雀荘') }
}
$json = ConvertTo-Json -InputObject $languages -Depth 32 -Compress
[IO.File]::WriteAllText((Join-Path $assets 'languages.js'), 'window.tenhouLanguages=' + $json + ';', $utf8)
Get-ChildItem -LiteralPath $upstream -File | Where-Object { $_.Extension -in @('.png', '.jpg', '.ttf') } | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $assets $_.Name) -Force
}
Write-Output ('Built pinned 4K lobby: ' + $OutputDirectory)
