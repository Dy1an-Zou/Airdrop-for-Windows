[CmdletBinding()]
param(
    [string]$PackageDir = (Join-Path $PSScriptRoot '..'),
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$installRoot = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'XiatiaoAirDrop'
$dataRoot = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'XiatiaoAirDrop'
$radioRoot = Join-Path $dataRoot 'radio'
$taskName = 'XiatiaoAirDrop-Radio'
$hostArgument = '--xiatiao-radio-host'
$package = (Resolve-Path -LiteralPath $PackageDir).Path.TrimEnd('\')
$exe = Join-Path $installRoot 'XiatiaoAirDrop.exe'
$currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
$ownerSid = $currentIdentity.User.Value
$adminSid = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')
$systemSid = New-Object Security.Principal.SecurityIdentifier('S-1-5-18')
$usersSid = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-545')

function Assert-NoLink([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "路径含重解析点，停止安装：$cursor" }
        }
        $next = [IO.Path]::GetDirectoryName($cursor.TrimEnd('\'))
        if ($next -eq $cursor) { break }
        $cursor = $next
    }
}
function Safe-Child([string]$Root, [string]$Relative) {
    if ([string]::IsNullOrWhiteSpace($Relative) -or [IO.Path]::IsPathRooted($Relative) -or $Relative -match '(^|[\\/])\.\.?([\\/]|$)|[:\x00]') { throw "清单含非法相对路径：$Relative" }
    $path = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (-not $path.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw '清单路径逃离安装根。' }
    Assert-NoLink $path
    return $path
}
function Test-Package([string]$Root) {
    Assert-NoLink $Root
    $manifestFile = Join-Path $Root 'runtime-manifest.json'
    Assert-NoLink $manifestFile
    $manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 'xiatiao.runtime.v1') { throw '不支持的安装清单。' }
    if ($manifest.PSObject.Properties['radioBackend'] -and $manifest.radioBackend -cne 'mt7925') { throw '此旧脚本仅供 MT7925 历史包使用。RTL8852CE 请使用对应的 0.2.4 Preview 安装器。' }
    $expected = @{}
    foreach ($entry in $manifest.files.PSObject.Properties) {
        $path = Safe-Child $Root $entry.Name
        if ($entry.Value -notmatch '^[0-9a-fA-F]{64}$') { throw '无效文件摘要。' }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $entry.Value) { throw "安装包文件缺失或摘要错误：$($entry.Name)" }
        $expected[$path.ToLowerInvariant()] = $true
    }
    foreach ($item in Get-ChildItem -LiteralPath $Root -Recurse -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '安装包含重解析点。' }
        if (-not $item.PSIsContainer -and $item.FullName -ine $manifestFile -and -not $expected.ContainsKey($item.FullName.ToLowerInvariant())) { throw "安装包含未登记文件：$($item.FullName)" }
    }
    foreach ($required in @('XiatiaoAirDrop.exe','XiatiaoAirDrop.dll','XiatiaoAirDrop.deps.json','XiatiaoAirDrop.runtimeconfig.json','runtime/awdld.exe','runtime/winawdl_mt7925.exe','runtime/wintun.dll','firmware/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin','firmware/WIFI_RAM_CODE_MT7925_1_1.bin','python/python.exe','receiver/engine.py','licenses/THIRD-PARTY-NOTICES.md','assets/xiatiao-airdrop.ico')) {
        $path = Safe-Child $Root $required
        if (-not $expected.ContainsKey($path.ToLowerInvariant())) { throw "安装清单缺少必要项：$required" }
    }
    return $manifest
}
function Protect-Tree([string]$Root) {
    Assert-NoLink $Root
    $all = @((Get-Item -LiteralPath $Root -Force)) + @(Get-ChildItem -LiteralPath $Root -Recurse -Force)
    foreach ($item in $all) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '保护目录时发现链接。' }
        $acl = if ($item.PSIsContainer) { New-Object Security.AccessControl.DirectorySecurity } else { New-Object Security.AccessControl.FileSecurity }
        $acl.SetOwner($adminSid)
        $acl.SetAccessRuleProtection($true, $false)
        $inherit = if ($item.PSIsContainer) { [Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit' } else { [Security.AccessControl.InheritanceFlags]::None }
        foreach ($sid in @($adminSid, $systemSid)) { $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid, 'FullControl', $inherit, 'None', 'Allow'))) }
        $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($usersSid, 'ReadAndExecute', $inherit, 'None', 'Allow')))
        Set-Acl -LiteralPath $item.FullName -AclObject $acl
    }
}

$manifest = Test-Package $package
foreach ($path in @($installRoot, $dataRoot, $radioRoot)) { Assert-NoLink $path }
if (Test-Path -LiteralPath $installRoot) { throw '已存在安装目录；请退出软件并先卸载旧版，用户文件会保留。' }
if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) { throw '同名计划任务已存在，停止以避免覆盖。' }
$dotnet = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'dotnet\dotnet.exe'
if (-not (Test-Path -LiteralPath $dotnet -PathType Leaf)) { throw '需要 x64 .NET Desktop Runtime 10；当前未找到。' }
$runtimes = & $dotnet --list-runtimes
if ($LASTEXITCODE -ne 0 -or -not ($runtimes -match '^Microsoft\.WindowsDesktop\.App 10\.')) { throw '需要 x64 .NET Desktop Runtime 10；当前未检测到。' }
$startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Airdrop for Windows.lnk'
if (Test-Path -LiteralPath $startMenu) { throw '开始菜单已存在同名快捷方式，停止以避免覆盖。' }
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
if (Get-ItemProperty -LiteralPath $runKey -Name 'XiatiaoAirDrop' -ErrorAction SilentlyContinue) { throw '同名自启动项已存在，停止以避免覆盖。' }
if ($ValidateOnly) {
    [ordered]@{ status = 'validated'; installRoot = $installRoot; ownerSid = $ownerSid; task = $taskName; hostArgument = $hostArgument; mutations = 0 } | ConvertTo-Json
    return
}
if (-not (New-Object Security.Principal.WindowsPrincipal($currentIdentity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw '请以管理员身份运行安装脚本。' }

# A fresh, protected staging tree is verified before it becomes executable installation content.
$stage = $installRoot + '.installing-' + [Guid]::NewGuid().ToString('N')
Assert-NoLink $stage
[IO.Directory]::CreateDirectory($stage) | Out-Null
Protect-Tree $stage
foreach ($entry in $manifest.files.PSObject.Properties) {
    $source = Safe-Child $package $entry.Name
    $target = Safe-Child $stage $entry.Name
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
    Copy-Item -LiteralPath $source -Destination $target
}
# Preserve the already validated in-memory manifest, not a second mutable source read.
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $stage 'runtime-manifest.json') -Encoding utf8
Protect-Tree $stage
Test-Package $stage | Out-Null
if ([IO.Path]::GetFullPath($stage).Substring(0, $installRoot.Length) -ine $installRoot -or (Test-Path -LiteralPath $installRoot)) { throw '安装目标在复制后发生变化，停止。' }
Move-Item -LiteralPath $stage -Destination $installRoot
[IO.Directory]::CreateDirectory($radioRoot) | Out-Null
Protect-Tree $dataRoot

$registered = $false
$runAdded = $false
$shortcutAdded = $false
try {
    $action = New-ScheduledTaskAction -Execute $exe -Argument $hostArgument -WorkingDirectory $installRoot
    $principal = New-ScheduledTaskPrincipal -UserId $ownerSid -LogonType Interactive -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings -Description 'Airdrop for Windows受限无线后台；由当前用户按需启动，无自动触发器。' | Out-Null
    $registered = $true
    $scheduler = New-Object -ComObject 'Schedule.Service'
    $scheduler.Connect()
    $task = $scheduler.GetFolder('\').GetTask($taskName)
    $task.SetSecurityDescriptor("O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;$ownerSid)", 0)
    New-Item -Path $runKey -Force | Out-Null
    New-ItemProperty -LiteralPath $runKey -Name 'XiatiaoAirDrop' -PropertyType String -Value ('"' + $exe + '" --hidden') | Out-Null
    $runAdded = $true
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($startMenu)
    $shortcut.TargetPath = $exe
    $shortcut.WorkingDirectory = $installRoot
    $shortcut.IconLocation = Join-Path $installRoot 'assets\xiatiao-airdrop.ico'
    $shortcut.Description = 'Airdrop for Windows · AirDrop 接收'
    $shortcut.Save()
    $shortcutAdded = $true
    [ordered]@{ schema = 'xiatiao.install.v1'; installedUtc = [DateTime]::UtcNow.ToString('o'); ownerSid = $ownerSid; installRoot = $installRoot; task = $taskName; shortcut = $startMenu; manifestSha256 = (Get-FileHash -LiteralPath (Join-Path $installRoot 'runtime-manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant() } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $installRoot 'installation.json') -Encoding utf8
    Protect-Tree $installRoot
    $readback = Get-ScheduledTask -TaskName $taskName
    if ($readback.Actions.Execute -ine $exe -or $readback.Actions.Arguments -ne $hostArgument -or $readback.Principal.RunLevel -ne 'Highest' -or $readback.Principal.LogonType -ne 'Interactive' -or ($null -ne $readback.Triggers -and @($readback.Triggers).Count -ne 0)) { throw '计划任务读回与预期不符。' }
    [ordered]@{ status = 'installed'; installRoot = $installRoot; autostart = $true; taskStarted = $false; desktopOpened = $false; driverChanged = $false } | ConvertTo-Json
} catch {
    if ($shortcutAdded -and (Test-Path -LiteralPath $startMenu)) { Remove-Item -LiteralPath $startMenu }
    if ($runAdded) { Remove-ItemProperty -LiteralPath $runKey -Name 'XiatiaoAirDrop' -ErrorAction SilentlyContinue }
    if ($registered) { Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue }
    throw "安装未完成；已撤回任务、自启动和快捷方式。文件保留在 $installRoot，未启动任何无线功能。原因：$($_.Exception.Message)"
}
