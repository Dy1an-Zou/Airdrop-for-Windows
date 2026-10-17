[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PublishDir,
    [Parameter(Mandatory)][string]$PythonDir,
    [Parameter(Mandatory)][string]$RuntimeDir,
    [string]$FirmwareDir,
    [Parameter(Mandatory)][string]$OutputDir,
    [ValidateSet('mt7925','rtl8852ce')][string]$Backend='mt7925',
    [string]$RealtekPackageDir,
    [string]$RealtekSourceDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Packaging is an offline operation: no task, service, adapter, firewall or process is started.
$appRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $appRoot '..'))
$sourceRuntime = (Resolve-Path -LiteralPath $(if($Backend -eq 'rtl8852ce'){$RealtekPackageDir}else{$RuntimeDir})).Path
$sourceReceiver = Join-Path $repoRoot 'airdrop'
$sourceFirmware = if($Backend -eq 'mt7925'){(Resolve-Path -LiteralPath $FirmwareDir).Path}else{$null}
$realtekSources = if($Backend -eq 'rtl8852ce'){(Resolve-Path -LiteralPath $RealtekSourceDir).Path}else{$null}
$publish = (Resolve-Path -LiteralPath $PublishDir).Path
$python = (Resolve-Path -LiteralPath $PythonDir).Path
$output = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $output) { throw "输出目录已存在，请指定一个新的打包目录：$output" }

function Assert-OrdinaryTree([string]$Root) {
    $item = Get-Item -LiteralPath $Root -Force
    if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "源目录无效或为链接：$Root" }
    foreach ($child in Get-ChildItem -LiteralPath $Root -Recurse -Force) {
        if ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "源树含链接，不打包：$($child.FullName)" }
    }
}
function Assert-Hash([string]$Path, [string]$Expected) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "缺少文件：$Path" }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Expected) { throw "来源哈希不符：$Path" }
}
function Copy-Tree([string]$Source, [string]$Destination) {
    Assert-OrdinaryTree $Source
    [IO.Directory]::CreateDirectory($Destination) | Out-Null
    foreach ($file in Get-ChildItem -LiteralPath $Source -Recurse -File -Force) {
        $relative = $file.FullName.Substring($Source.TrimEnd('\').Length).TrimStart('\')
        if ($relative -match '(^|\\)(__pycache__|\.git|\.pytest_cache)(\\|$)' -or $file.Extension -eq '.pdb') { continue }
        $target = Join-Path $Destination $relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $target
    }
}

foreach ($source in (@($publish, $python, $sourceRuntime, $sourceReceiver, $sourceFirmware, $realtekSources, (Join-Path $appRoot 'assets'), (Join-Path $appRoot 'licenses'), (Join-Path $appRoot 'design-sources')) | Where-Object {$_})) {
    Assert-OrdinaryTree $source
    if ($output.StartsWith($source.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -or $source.StartsWith($output.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '输出目录不能与输入树重叠。' }
}
foreach ($name in @('XiatiaoAirDrop.exe', 'XiatiaoAirDrop.dll', 'XiatiaoAirDrop.deps.json', 'XiatiaoAirDrop.runtimeconfig.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $publish $name) -PathType Leaf)) { throw "发布目录缺少 $name" }
}
if (-not (Test-Path -LiteralPath (Join-Path $python 'python.exe') -PathType Leaf)) { throw '便携 Python 缺少 python.exe。' }
$runtimeHashes = [ordered]@{
    'awdld.exe' = 'cc4cc3bdffb8ae3ed18dd9d7d7056fd1769ba1e55b9338619a2eb552d5f29130'
    'winawdl_mt7925.exe' = '79e1bc7422c699cd6b0dfd378c9c910dc1d6d3654d748aa904e7232e9f880df2'
    'wintun.dll' = 'e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce'
}
$firmwareHashes = [ordered]@{
    'WIFI_MT7925_PATCH_MCU_1_1_hdr.bin' = '8eb46014d2a6b4124472eee7476d995008a6f40b1daffef87eb42f30d98699e1'
    'WIFI_RAM_CODE_MT7925_1_1.bin' = '23ff53b4bb639b30481e2e06bb1688569ad1ba971b897936db539882abfbd120'
}
$runtimePaths=@{'awdld.exe'='awdld.exe';'winawdl_mt7925.exe'='winawdl_mt7925.exe';'wintun.dll'='wintun.dll'}
$realtekSourceHashes=@{}
if($Backend -eq 'rtl8852ce'){
    $runtimeHashes=[ordered]@{
        'awdld.exe'='ae9c4ba3eb8583e3792b4d6c10cd40191c44ed8440d1bc942cbafeb99cd91af9'
        'winawdl_radio.exe'='55ec78b2a3e632f4b022c444085660a9915286b2ce9d7549d68dbd69c1bdfb77'
        'wintun.dll'='e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce'
    }
    $runtimePaths=@{'awdld.exe'='awdl/awdld.exe';'winawdl_radio.exe'='radio/winawdl_radio.exe';'wintun.dll'='awdl/wintun.dll'}
    $firmwareHashes=[ordered]@{}
    $realtekSourceHashes=@{
        'winawdl-6f91199e864609f8524ace832fe4af3d94f431f4.zip'='2b8e3f2bc712ea23bee9f0860e4fdde60c15225d3ce22bf24167beba0a4a2216'
        'devourer-8d936de2a3805ce126e5236349afe40e645aaf9b.zip'='064fd294b9bd3db3f16bef01bc5ff6d20a3d1aac74a17680e974bc5ff81bf4ea'
        'owl-da255a70f221784c836d943dd3f243bc798f223b.zip'='0f57557f28e805b2e279746d3cdd5c9528d436638cfbd6efabf161d215434dac'
        'radiotap-94984dd829a605a2d7af55241145c159607f3b30.zip'='c7f94f729f2830d40856540f9752540a24f850d6ad910bbb2836c95dae78fcac'
    }
    foreach($entry in $realtekSourceHashes.GetEnumerator()){Assert-Hash (Join-Path $realtekSources $entry.Key) $entry.Value}
}
foreach ($entry in $runtimeHashes.GetEnumerator()) { Assert-Hash (Join-Path $sourceRuntime $runtimePaths[$entry.Key]) $entry.Value }
foreach ($entry in $firmwareHashes.GetEnumerator()) { Assert-Hash (Join-Path $sourceFirmware $entry.Key) $entry.Value }
if (-not (Test-Path -LiteralPath (Join-Path $sourceReceiver 'engine.py') -PathType Leaf)) { throw '接收源码缺少 engine.py。' }

Copy-Tree $publish $output
Copy-Tree $python (Join-Path $output 'python')
Copy-Tree $sourceReceiver (Join-Path $output 'receiver')
$pathFiles = @(Get-ChildItem -LiteralPath (Join-Path $output 'python') -Filter 'python*._pth' -File)
if ($pathFiles.Count -ne 1) { throw '便携 Python 应包含一个明确的隔离路径配置文件。' }
# Only the packaged copy receives the fixed sibling engine path; no site or user path is enabled.
$paths = @(Get-Content -LiteralPath $pathFiles[0].FullName)
if ('../receiver' -notin $paths) { Add-Content -LiteralPath $pathFiles[0].FullName -Value '../receiver' -Encoding ascii }
foreach ($folder in @('runtime', 'scripts')) { [IO.Directory]::CreateDirectory((Join-Path $output $folder)) | Out-Null }
if($Backend -eq 'mt7925'){[IO.Directory]::CreateDirectory((Join-Path $output 'firmware'))|Out-Null}
foreach ($entry in $runtimeHashes.GetEnumerator()) { Copy-Item -LiteralPath (Join-Path $sourceRuntime $runtimePaths[$entry.Key]) -Destination (Join-Path $output "runtime\$($entry.Key)") }
foreach ($entry in $firmwareHashes.GetEnumerator()) { Copy-Item -LiteralPath (Join-Path $sourceFirmware $entry.Key) -Destination (Join-Path $output "firmware\$($entry.Key)") }
if($Backend -eq 'mt7925'){Copy-Item -LiteralPath (Join-Path $sourceFirmware 'LICENCE.mediatek') -Destination (Join-Path $output 'firmware\LICENCE.mediatek')}
foreach ($folder in @('licenses', 'design-sources')) { Copy-Tree (Join-Path $appRoot $folder) (Join-Path $output $folder) }
[IO.Directory]::CreateDirectory((Join-Path $output 'assets')) | Out-Null
foreach ($asset in @('xiatiao-airdrop.png', 'xiatiao-airdrop.ico')) { Copy-Item -LiteralPath (Join-Path $appRoot "assets\$asset") -Destination (Join-Path $output "assets\$asset") }
foreach ($script in @('install-xiatiao.ps1', 'uninstall-xiatiao.ps1')) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination (Join-Path $output "scripts\$script") }

# Ship the actual edited source of every component beside the binaries.
# Explicit source folders avoid accidentally copying local runtime data or outputs.
$sourceRoot = Join-Path $output 'source'
[IO.Directory]::CreateDirectory($sourceRoot) | Out-Null
foreach ($folder in @('airdrop', 'awdl', 'radio', 'driver', 'upstream', 'docs', 'artwork')) {
    Copy-Tree (Join-Path $repoRoot $folder) (Join-Path $sourceRoot $folder)
}
$sourceApp = Join-Path $sourceRoot 'app'
foreach ($folder in @('src', 'assets', 'licenses', 'scripts', 'design-sources', 'installer', 'tests')) {
    Copy-Tree (Join-Path $appRoot $folder) (Join-Path $sourceApp $folder)
}
foreach ($name in @('XiatiaoAirDrop.csproj', 'XiatiaoAirDrop.sln', 'Directory.Build.props', 'NuGet.config', 'app.manifest')) {
    Copy-Item -LiteralPath (Join-Path $appRoot $name) -Destination (Join-Path $sourceApp $name)
}
foreach ($name in @('README.md', 'README.en.md', 'LICENSE', '.gitignore', '.gitattributes')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $name) -Destination (Join-Path $sourceRoot $name)
}
if($Backend -eq 'rtl8852ce'){
    $upstreamSource=Join-Path $sourceRoot 'rtl8852ce-upstream'
    [IO.Directory]::CreateDirectory($upstreamSource)|Out-Null
    foreach($entry in $realtekSourceHashes.GetEnumerator()){Copy-Item -LiteralPath (Join-Path $realtekSources $entry.Key) -Destination (Join-Path $upstreamSource $entry.Key)}
    Copy-Tree (Join-Path $sourceRuntime 'licenses') (Join-Path $output 'licenses/rtl8852ce-upstream')
}

$files = [ordered]@{}
foreach ($file in Get-ChildItem -LiteralPath $output -Recurse -File -Force | Sort-Object FullName) {
    $relative = $file.FullName.Substring($output.TrimEnd('\').Length).TrimStart('\').Replace('\', '/')
    $files[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest = [ordered]@{
    schema = 'xiatiao.runtime.v1'
    product = 'XiatiaoAirDrop'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    radioBackend = $Backend
    sourceRuntime = $(if($Backend -eq 'rtl8852ce'){'rtl8852ce-winawdl-v0.3.0'}else{'mt7925-transfer-runtime-r39'})
    sourceReceiver = 'source/airdrop'
    files = $files
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'runtime-manifest.json') -Encoding utf8
[ordered]@{ status = 'packaged'; directory = $output; files = $files.Count; manifestSha256 = (Get-FileHash -LiteralPath (Join-Path $output 'runtime-manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant(); networkStarted = $false } | ConvertTo-Json
