[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PublishDir,
    [Parameter(Mandatory)][string]$PythonDir,
    [Parameter(Mandatory)][string]$RuntimeDir,
    [Parameter(Mandatory)][string]$FirmwareDir,
    [Parameter(Mandatory)][string]$OutputDir
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Packaging is an offline operation: no task, service, adapter, firewall or process is started.
$appRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repoRoot = [IO.Path]::GetFullPath((Join-Path $appRoot '..'))
$sourceRuntime = (Resolve-Path -LiteralPath $RuntimeDir).Path
$sourceReceiver = Join-Path $repoRoot 'airdrop'
$sourceFirmware = (Resolve-Path -LiteralPath $FirmwareDir).Path
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

foreach ($source in @($publish, $python, $sourceRuntime, $sourceReceiver, $sourceFirmware, (Join-Path $appRoot 'assets'), (Join-Path $appRoot 'licenses'), (Join-Path $appRoot 'design-sources'))) {
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
foreach ($entry in $runtimeHashes.GetEnumerator()) { Assert-Hash (Join-Path $sourceRuntime $entry.Key) $entry.Value }
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
foreach ($folder in @('runtime', 'firmware', 'scripts')) { [IO.Directory]::CreateDirectory((Join-Path $output $folder)) | Out-Null }
foreach ($entry in $runtimeHashes.GetEnumerator()) { Copy-Item -LiteralPath (Join-Path $sourceRuntime $entry.Key) -Destination (Join-Path $output "runtime\$($entry.Key)") }
foreach ($entry in $firmwareHashes.GetEnumerator()) { Copy-Item -LiteralPath (Join-Path $sourceFirmware $entry.Key) -Destination (Join-Path $output "firmware\$($entry.Key)") }
Copy-Item -LiteralPath (Join-Path $sourceFirmware 'LICENCE.mediatek') -Destination (Join-Path $output 'firmware\LICENCE.mediatek')
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
foreach ($folder in @('src', 'assets', 'licenses', 'scripts', 'design-sources')) {
    Copy-Tree (Join-Path $appRoot $folder) (Join-Path $sourceApp $folder)
}
foreach ($name in @('XiatiaoAirDrop.csproj', 'XiatiaoAirDrop.sln', 'Directory.Build.props', 'NuGet.config', 'app.manifest')) {
    Copy-Item -LiteralPath (Join-Path $appRoot $name) -Destination (Join-Path $sourceApp $name)
}
foreach ($name in @('README.md', 'LICENSE', '.gitignore', '.gitattributes')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $name) -Destination (Join-Path $sourceRoot $name)
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
    sourceRuntime = 'mt7925-transfer-runtime-r39'
    sourceReceiver = 'source/airdrop'
    files = $files
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'runtime-manifest.json') -Encoding utf8
[ordered]@{ status = 'packaged'; directory = $output; files = $files.Count; manifestSha256 = (Get-FileHash -LiteralPath (Join-Path $output 'runtime-manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant(); networkStarted = $false } | ConvertTo-Json
