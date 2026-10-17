[CmdletBinding()]
param([switch]$ValidateOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$installRoot = [IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'XiatiaoAirDrop'))
$taskName = 'XiatiaoAirDrop-Radio'
$hostArgument = '--xiatiao-radio-host'
$exe = Join-Path $installRoot 'XiatiaoAirDrop.exe'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (Test-Path -LiteralPath $installRoot -PathType Container)) { throw '未找到Airdrop for Windows安装目录。' }
$rootItem = Get-Item -LiteralPath $installRoot -Force
if ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint -or $rootItem.FullName -ine $installRoot) { throw '安装根不是预期的普通目录，停止卸载。' }
$children = @(Get-ChildItem -LiteralPath $installRoot -Recurse -Force)
if (@($children | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw '安装目录含链接，停止卸载。' }
$receipt = Get-Content -LiteralPath (Join-Path $installRoot 'installation.json') -Raw | ConvertFrom-Json
if ($receipt.schema -ne 'xiatiao.install.v1' -or $receipt.installRoot -ine $installRoot -or $receipt.ownerSid -ne $identity.User.Value) { throw '请使用安装时的 Windows 用户卸载；安装身份或目录不符。' }
$manifestPath = Join-Path $installRoot 'runtime-manifest.json'
if ((Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash -ine $receipt.manifestSha256) { throw '安装清单已变化，停止自动删除。' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$known = @{}
foreach ($entry in $manifest.files.PSObject.Properties) { $known[$entry.Name.Replace('\', '/').ToLowerInvariant()] = $true }
$known['runtime-manifest.json'] = $true
$known['installation.json'] = $true
foreach ($file in $children | Where-Object { -not $_.PSIsContainer }) {
    $relative = $file.FullName.Substring($installRoot.Length).TrimStart('\').Replace('\', '/').ToLowerInvariant()
    if (-not $known.ContainsKey($relative)) { throw "发现非安装包文件，为避免删除用户内容已停止：$relative" }
}
$active = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -in @('XiatiaoAirDrop.exe', 'winawdl_mt7925.exe', 'awdld.exe') })
if ($active.Count) { throw '请先从托盘退出Airdrop for Windows，等待无线后台正常结束后再卸载；卸载器不会强制终止驱动相关进程。' }
foreach ($ruleName in @('XiatiaoAirDrop-TCP', 'XiatiaoAirDrop-mDNS')) {
    if (Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue) { throw "无线后台仍有临时规则 $ruleName，请先让助手正常关闭再卸载。" }
}
$task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($task -and ($task.Actions.Execute -ine $exe -or $task.Actions.Arguments -ne $hostArgument -or $task.State -eq 'Running')) { throw '计划任务仍运行或动作不是本软件，停止卸载。' }
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$run = Get-ItemProperty -LiteralPath $runKey -Name 'XiatiaoAirDrop' -ErrorAction SilentlyContinue
$expectedRun = '"' + $exe + '" --hidden'
if ($run -and $run.XiatiaoAirDrop -ne $expectedRun) { throw '自启动项已被更改，停止以避免删除其他设置。' }
$startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'Airdrop for Windows.lnk'
if ($receipt.shortcut -ine $startMenu) { throw '快捷方式位置与安装记录不符。' }
if (Test-Path -LiteralPath $startMenu) {
    $shell = New-Object -ComObject WScript.Shell
    if ($shell.CreateShortcut($startMenu).TargetPath -ine $exe) { throw '快捷方式已指向其他软件，停止卸载。' }
}
if ($ValidateOnly) { [ordered]@{ status = 'validated'; installRoot = $installRoot; mutations = 0; userFilesPreserved = $true; driverPreserved = $true } | ConvertTo-Json; return }
if (-not (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw '请以管理员身份运行卸载脚本。' }
if ($task) { Unregister-ScheduledTask -TaskName $taskName -Confirm:$false }
if ($run) { Remove-ItemProperty -LiteralPath $runKey -Name 'XiatiaoAirDrop' }
if (Test-Path -LiteralPath $startMenu) { Remove-Item -LiteralPath $startMenu }
# The one recursive removal is limited to the exact fixed Program Files product root.
# Its tree was checked above for reparse points and any files outside the package manifest.
$verifiedRoot = (Resolve-Path -LiteralPath $installRoot).Path
if ($verifiedRoot -ine (Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'XiatiaoAirDrop')) { throw '删除前路径复核失败。' }
Remove-Item -LiteralPath $verifiedRoot -Recurse -Force
[ordered]@{ status = 'uninstalled'; userFilesPreserved = $true; userSettingsPreserved = $true; programDataPreserved = $true; driverPreserved = $true; testSigningUnchanged = $true } | ConvertTo-Json
