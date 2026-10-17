# SPDX-License-Identifier: MIT
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('verify-payload','preflight','install','uninstall-preflight','uninstall')][string]$Mode,
    [string]$PackageDir,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedManifestSha256,
    [string]$ExpectedOwnerSid,
    [string]$InstallerPath,
    [ValidateSet('preserve','enabled','disabled')][string]$AutoStart = 'preserve',
    [string]$ResultPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$script:resultPathApproved=$false

# Called by the installer; never elevates, opens UI, starts the application,
# installs a driver, changes security policy, or stops an application.
$legacyManifests = @('234f8a9655769cc036ddec42493b5bc69dd8d41d06c5640f65bd0fe777d2b067','0d64270845b33c202f1d3ad67d01e7ab5ebc395415c716057863769b6acd923f')
$hardwareId = 'PCI\VEN_14C3&DEV_7925&SUBSYS_60001A3B&REV_00'
$driverSha = '938148d69b14c911c18c4c7a9ef00703ad5625e46d876143b8f887df6762aafb'
$programFiles = [IO.Path]::GetFullPath([Environment]::GetFolderPath('ProgramFiles')).TrimEnd('\')
$installRoot = Join-Path $programFiles 'XiatiaoAirDrop'
$dataRoot = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'XiatiaoAirDrop'
$radioRoot = Join-Path $dataRoot 'radio'
$taskName = 'XiatiaoAirDrop-Radio'
$hostArgument = '--xiatiao-radio-host'
$exe = Join-Path $installRoot 'XiatiaoAirDrop.exe'
$shortcutPath = Join-Path ([Environment]::GetFolderPath('Programs')) 'Airdrop for Windows.lnk'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$runValue = '"' + $exe + '" --hidden'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$ownerSid = $identity.User.Value
$setupParent = [IO.Path]::GetFullPath([Environment]::GetFolderPath('CommonProgramFiles')).TrimEnd('\')
$setupRoot = Join-Path $setupParent 'AirdropForWindowsSetup'
$setupExe = Join-Path $setupRoot 'Setup.exe'
$uninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\AirdropForWindows'
$pins = @{
    'runtime/awdld.exe'='cc4cc3bdffb8ae3ed18dd9d7d7056fd1769ba1e55b9338619a2eb552d5f29130'
    'runtime/winawdl_mt7925.exe'='79e1bc7422c699cd6b0dfd378c9c910dc1d6d3654d748aa904e7232e9f880df2'
    'runtime/wintun.dll'='e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce'
    'firmware/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin'='8eb46014d2a6b4124472eee7476d995008a6f40b1daffef87eb42f30d98699e1'
    'firmware/WIFI_RAM_CODE_MT7925_1_1.bin'='23ff53b4bb639b30481e2e06bb1688569ad1ba971b897936db539882abfbd120'
}

function Fail([string]$Code, [string]$Message) { throw "$Code`: $Message" }
function Resolve-RadioBackend($Manifest) {
    $id = if ($Manifest.PSObject.Properties['radioBackend']) { $Manifest.radioBackend } else { 'mt7925' }
    if ($id -cnotin @('mt7925','rtl8852ce')) { Fail 'RADIO_BACKEND' 'Unknown radio backend.' }
    $source = if ($id -ceq 'mt7925') { 'mt7925-transfer-runtime-r39' } else { 'rtl8852ce-winawdl-v0.3.0' }
    if ($Manifest.sourceRuntime -cne $source) { Fail 'RADIO_SOURCE' 'Radio backend and runtime provenance differ.' }
    return $id
}
function Get-RadioPins([string]$Backend) {
    if ($Backend -ceq 'mt7925') { return $pins }
    if ($Backend -cne 'rtl8852ce') { Fail 'RADIO_BACKEND' 'Unknown radio backend.' }
    return @{
        'runtime/awdld.exe'='ae9c4ba3eb8583e3792b4d6c10cd40191c44ed8440d1bc942cbafeb99cd91af9'
        'runtime/winawdl_radio.exe'='55ec78b2a3e632f4b022c444085660a9915286b2ce9d7549d68dbd69c1bdfb77'
        'runtime/wintun.dll'='e5da8447dc2c320edc0fc52fa01885c103de8c118481f683643cacc3220dafce'
    }
}
function Assert-RealtekIdentity($Device) {
    if ($Device.instance -notmatch '\APCI\\VEN_10EC&DEV_C852(?:&SUBSYS_[0-9A-F]{8})?(?:&REV_[0-9A-F]{2})?\\[A-Z0-9&_]+\z' -or $Device.instance.Length -gt 240) { Fail 'DEVICE_ID' 'Invalid RTL8852CE instance.' }
    $classId=[guid]::Empty
    if ($Device.status -cne 'OK' -or [int]$Device.problem -ne 0 -or $Device.service -ine 'WUDFRd' -or
        -not [guid]::TryParse([string]$Device.classGuid,[ref]$classId) -or $classId -ne [guid]'42c93386-3172-4834-9a73-d5dbe38443f8' -or
        @($Device.hardwareIds|Where-Object{$_ -imatch '\APCI\\VEN_10EC&DEV_C852(?:&SUBSYS_[0-9A-F]{8}(?:&REV_[0-9A-F]{2})?|&REV_[0-9A-F]{2}|&CC_[0-9A-F]{4}(?:[0-9A-F]{2})?)?\z'}).Count -eq 0) { Fail 'DEVICE_DRIVER' 'RTL8852CE requires the winawdl_regproxy experimental driver. No driver was changed. / RTL8852CE 实验驱动未就绪。' }
}
function Get-RealtekDriverImageHash([byte[]]$Image) {
    # Match upstream code while permitting the machine-local Authenticode signature.
    if($Image.Length -lt 110592 -or $Image.Length -gt 1159168 -or [BitConverter]::ToInt32($Image,60) -ne 264 -or [BitConverter]::ToUInt32($Image,264) -ne 0x4550 -or [BitConverter]::ToUInt16($Image,288) -ne 0x20b){Fail 'DRIVER_IMAGE' 'Unexpected RTL8852CE PE image.'}
    $offset=[BitConverter]::ToUInt32($Image,432);$length=[BitConverter]::ToUInt32($Image,436)
    if(($offset -eq 0 -and ($length -ne 0 -or $Image.Length -ne 110592)) -or ($offset -ne 0 -and ($offset -ne 110592 -or $length -lt 8 -or [uint64]$offset+$length -ne [uint64]$Image.Length))){Fail 'DRIVER_IMAGE' 'Invalid RTL8852CE certificate range.'}
    $copy=New-Object byte[] 110592;[Array]::Copy($Image,$copy,110592)
    [Array]::Clear($copy,352,4);[Array]::Clear($copy,432,8)
    return Hash-Bytes $copy
}
function Assert-RealtekRequirements {
    $system=[Environment]::SystemDirectory
    foreach($dll in @('vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll')){
        if(-not (Test-Path -LiteralPath (Join-Path $system $dll))){Fail 'VC_RUNTIME' 'RTL8852CE requires Microsoft Visual C++ 2015-2022 x64 Redistributable.'}
    }
    $priv=& (Join-Path $system 'whoami.exe') /priv /fo csv /nh
    if($LASTEXITCODE -ne 0 -or -not ($priv -match 'SeLockMemoryPrivilege')){Fail 'LOCK_MEMORY' 'RTL8852CE requires Lock pages in memory for this account. Sign out and sign in after granting it. / 需为当前账户分配“锁定内存页”，再注销并重新登录。'}
    $devices=@(Get-PnpDevice -PresentOnly | Where-Object {$_.InstanceId -match '^PCI\\VEN_10EC&DEV_C852(?:&|\\)'})
    if($devices.Count -ne 1){Fail 'DEVICE_COUNT' 'Exactly one dedicated RTL8852CE is required. / 需要唯一一块专用 RTL8852CE。'}
    $device=$devices[0]
    $state=[pscustomobject]@{
        instance=$device.InstanceId;status=$device.Status
        service=(Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Service').Data
        problem=(Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode').Data
        classGuid=[string](Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ClassGuid').Data
        hardwareIds=@((Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds').Data)
    }
    Assert-RealtekIdentity $state
    $inf=[string](Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_DriverInfPath').Data
    if($inf -notmatch '^oem[0-9]+\.inf$'){Fail 'DRIVER_PATH' 'Expected an installed OEM driver INF.'}
    $packages=@(Get-WindowsDriver -Online -Driver $inf -ErrorAction Stop)
    if($packages.Count -ne 1){Fail 'DRIVER_PATH' 'Ambiguous driver package.'}
    $original=[string]$packages[0].OriginalFileName
    $driverStore=[IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('Windows')) 'System32\DriverStore\FileRepository')).TrimEnd('\')+'\'
    if(-not [IO.Path]::IsPathRooted($original) -or -not ([IO.Path]::GetFullPath($original)).StartsWith($driverStore,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($original) -ine 'winawdl_regproxy.inf'){Fail 'DRIVER_PATH' 'RTL8852CE driver package is outside its expected driver store path.'}
    $driverPath=Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($original))) 'winawdl_regproxy.dll'
    Assert-NoLink $driverPath
    $expected='ed0bc40efe1fa5d069ec9c42ab5f8181de9981b986aed710c92ebb5d615c23d7'
    if((Get-RealtekDriverImageHash ([IO.File]::ReadAllBytes($driverPath))) -ine $expected){Fail 'DRIVER_HASH' 'RTL8852CE driver differs from pinned winawdl 0.3.0.'}
    return [pscustomobject]@{backend='rtl8852ce';hardwareId='PCI\VEN_10EC&DEV_C852';instanceId=$device.InstanceId;driverSha256=$expected}
}
function Hash-Bytes([byte[]]$Bytes) {
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant() }
    finally { $hash.Dispose() }
}
function Assert-NoLink([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { Fail 'PATH_LINK' "Reparse point is not permitted: $cursor" }
        }
        $next = [IO.Path]::GetDirectoryName($cursor.TrimEnd('\'))
        if ($next -eq $cursor) { break }
        $cursor = $next
    }
}
function Assert-ProtectedItem([string]$Path) {
    Assert-NoLink $Path
    $acl=Get-Acl -LiteralPath $Path
    $trusted=@('S-1-5-18','S-1-5-32-544')
    $owner=$acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
    if($owner -notin $trusted){Fail 'UNTRUSTED_OWNER' "Installation content is not owned by Administrators or SYSTEM: $Path"}
    $writeMask=[Security.AccessControl.FileSystemRights]'Write, Delete, DeleteSubdirectoriesAndFiles, ChangePermissions, TakeOwnership'
    foreach($rule in $acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){
        if($rule.AccessControlType -eq 'Allow' -and ($rule.FileSystemRights -band $writeMask) -ne 0 -and $rule.IdentityReference.Value -notin $trusted){Fail 'WRITABLE_INSTALLATION' "Installation content is writable by an untrusted principal: $Path"}
    }
}
function Safe-Child([string]$Root, [string]$Relative) {
    if ([string]::IsNullOrWhiteSpace($Relative) -or [IO.Path]::IsPathRooted($Relative) -or
        $Relative -match '(^|[\\/])\.\.?([\\/]|$)|[:\x00*?<>|"\r\n]' -or
        $Relative -match '[. ]([\\/]|$)|[\\/]{2}|[\\/]$' -or
        $Relative -match '(^|[\\/])(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])([.]|[\\/]|$)') { Fail 'MANIFEST_PATH' "Invalid package path: $Relative" }
    $path = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (-not $path.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { Fail 'MANIFEST_ESCAPE' 'Package path leaves its root.' }
    Assert-NoLink $path
    return $path
}
function Get-TreeItems([string]$Root) {
    Assert-NoLink $Root
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($Root)
    while ($pending.Count) {
        $parent = $pending.Pop()
        Assert-NoLink $parent
        foreach ($item in Get-ChildItem -LiteralPath $parent -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { Fail 'PATH_LINK' "Package tree contains a link: $($item.FullName)" }
            $item
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
        }
    }
}
function Read-VerifiedTree([string]$Root, [string]$ManifestSha, [bool]$AllowReceipt) {
    Assert-NoLink $Root
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { Fail 'PACKAGE_MISSING' "Package directory is missing: $Root" }
    $manifestPath = Join-Path $Root 'runtime-manifest.json'
    Assert-NoLink $manifestPath
    $bytes = [IO.File]::ReadAllBytes($manifestPath)
    if ((Hash-Bytes $bytes) -ine $ManifestSha) { Fail 'MANIFEST_HASH' 'Package manifest does not match this installer.' }
    $manifest = [Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xFEFF) | ConvertFrom-Json
    if ($manifest.schema -ne 'xiatiao.runtime.v1' -or $manifest.product -ne 'XiatiaoAirDrop') { Fail 'MANIFEST_PRODUCT' 'Unexpected package manifest format or product.' }
    $backend=Resolve-RadioBackend $manifest
    $selectedPins=Get-RadioPins $backend
    $known = @{}
    $directories = @{}
    foreach ($entry in $manifest.files.PSObject.Properties) {
        $relative = $entry.Name.Replace('\','/').ToLowerInvariant()
        $path = Safe-Child $Root $relative
        if ($relative -in @('runtime-manifest.json','installation.json') -or $known.ContainsKey($relative) -or $entry.Value -notmatch '^[0-9a-fA-F]{64}$') { Fail 'MANIFEST_ENTRY' "Duplicate, reserved or invalid package entry: $relative" }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $entry.Value) { Fail 'FILE_HASH' "Package file is missing or changed: $relative" }
        $known[$relative] = [string]$entry.Value
        $parent = [IO.Path]::GetDirectoryName($relative.Replace('/','\'))
        while ($parent) { $directories[$parent.Replace('\','/').ToLowerInvariant()]=$true; $parent=[IO.Path]::GetDirectoryName($parent) }
    }
    foreach ($item in Get-TreeItems $Root) {
        $relative = $item.FullName.Substring($Root.TrimEnd('\').Length).TrimStart('\').Replace('\','/').ToLowerInvariant()
        if ($item.PSIsContainer) {
            if (-not $directories.ContainsKey($relative)) { Fail 'EXTRA_DIRECTORY' "Unregistered directory is preserved; operation stopped: $relative" }
        } elseif ($relative -ne 'runtime-manifest.json' -and -not ($AllowReceipt -and $relative -eq 'installation.json') -and -not $known.ContainsKey($relative)) { Fail 'EXTRA_FILE' "Unregistered file is preserved; operation stopped: $relative" }
    }
    foreach ($required in @('xiatiaoairdrop.exe','xiatiaoairdrop.dll','xiatiaoairdrop.deps.json','xiatiaoairdrop.runtimeconfig.json','python/python.exe','receiver/engine.py','receiver/sender.py','receiver/airdrop-beacon.ps1','assets/xiatiao-airdrop.ico','assets/xiatiao-airdrop.png','licenses/third-party-notices.md')) {
        if (-not $known.ContainsKey($required)) { Fail 'MISSING_COMPONENT' "Required package component is missing: $required" }
    }
    foreach ($name in $selectedPins.Keys) { if (-not $known.ContainsKey($name) -or $known[$name] -ine $selectedPins[$name]) { Fail 'NATIVE_PIN' "Native component differs from the supported release: $name" } }
    if ($AllowReceipt -and -not (Test-Path -LiteralPath (Join-Path $Root 'installation.json') -PathType Leaf)) { Fail 'RECEIPT_MISSING' 'Installation receipt is missing.' }
    return [pscustomobject]@{Root=$Root; Manifest=$manifest; Bytes=$bytes; Sha=$ManifestSha.ToLowerInvariant(); Files=$known; Backend=$backend}
}
function Read-Installed {
    if (-not (Test-Path -LiteralPath $installRoot)) { return $null }
    Assert-NoLink $installRoot
    $receiptPath = Join-Path $installRoot 'installation.json'
    Assert-NoLink $receiptPath
    $receiptBytes = [IO.File]::ReadAllBytes($receiptPath)
    $receipt = [Text.Encoding]::UTF8.GetString($receiptBytes).TrimStart([char]0xFEFF) | ConvertFrom-Json
    if ($receipt.schema -ne 'xiatiao.install.v1' -or $receipt.installRoot -ine $installRoot -or $receipt.ownerSid -cne $ownerSid -or $receipt.task -cne $taskName -or $receipt.shortcut -ine $shortcutPath -or $receipt.manifestSha256 -notmatch '^[0-9a-fA-F]{64}$') { Fail 'INSTALL_IDENTITY' 'Installation identity does not match this Windows user. / 请使用安装时的管理员账户。' }
    $tree = Read-VerifiedTree $installRoot $receipt.manifestSha256 $true
    Assert-ProtectedItem $installRoot
    foreach($item in Get-TreeItems $installRoot){Assert-ProtectedItem $item.FullName}
    $tree | Add-Member -NotePropertyName Receipt -NotePropertyValue $receipt
    $tree | Add-Member -NotePropertyName ReceiptSha -NotePropertyValue (Hash-Bytes $receiptBytes)
    return $tree
}
function Assert-Owner([bool]$Elevated) {
    if ($ExpectedOwnerSid -notmatch '^S-1-\d+(-\d+){1,15}$' -or $ExpectedOwnerSid -cne $ownerSid) { Fail 'OWNER_CHANGED' 'Run Setup from the same administrator account; changing credentials is unsupported. / 请用当前管理员账户运行。' }
    if ('S-1-5-32-544' -notin @($identity.Groups | ForEach-Object {$_.Value})) { Fail 'ADMIN_ACCOUNT' 'The current Windows account must belong to Administrators. / 当前账户需要属于管理员组。' }
    if ($Elevated -and -not (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { Fail 'ADMIN_REQUIRED' 'Administrator approval is required. / 需要管理员权限。' }
    if (-not [Environment]::Is64BitProcess -or -not [Environment]::Is64BitOperatingSystem) { Fail 'X64_REQUIRED' 'Use the 64-bit Windows installer and 64-bit PowerShell.' }
}
function Assert-Requirements([string]$Backend='mt7925') {
    $os = Get-CimInstance Win32_OperatingSystem
    $architecture = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE
    if ($architecture -ine 'AMD64' -or [int]$os.BuildNumber -lt 26100) { Fail 'WINDOWS_VERSION' 'Windows x64 build 26100 or newer is required. / 需要 Windows 11 24H2 或更新的 x64 系统。' }
    $dotnet = Join-Path $programFiles 'dotnet\dotnet.exe'
    Assert-NoLink $dotnet
    if (-not (Test-Path -LiteralPath $dotnet -PathType Leaf)) { Fail 'DOTNET_REQUIRED' '.NET Desktop Runtime 10 x64 is required. Install it from Microsoft, then run Setup again.' }
    $runtimes = & $dotnet --list-runtimes
    if ($LASTEXITCODE -ne 0 -or -not ($runtimes -match '^Microsoft\.WindowsDesktop\.App 10\.\d+\.\d+ \[')) { Fail 'DOTNET_REQUIRED' '.NET Desktop Runtime 10 x64 is required. / 缺少 .NET 桌面运行时 10 x64。' }
    if ($Backend -ceq 'rtl8852ce') { return Assert-RealtekRequirements }
    if ($Backend -cne 'mt7925') { Fail 'RADIO_BACKEND' 'Unknown radio backend.' }
    $candidates = @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId.StartsWith($hardwareId + '\', [StringComparison]::OrdinalIgnoreCase) })
    if ($candidates.Count -ne 1) { Fail 'DEVICE_COUNT' 'Exactly one supported MT7925 with the tested experimental driver is required. Setup does not install drivers. / 需要唯一受支持且已配置实验驱动的 MT7925。' }
    $device = $candidates[0]
    $ids = @((Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_HardwareIds').Data)
    $service = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_Service').Data
    $problem = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode').Data
    if ($hardwareId -notin $ids -or $service -cne 'mt7925_bringup' -or $device.Status -cne 'OK' -or [int]$problem -ne 0) { Fail 'DEVICE_DRIVER' 'The supported MT7925 experimental driver is not ready; no driver was changed.' }
    $image = [string](Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\mt7925_bringup').ImagePath
    if (-not $image.StartsWith('\SystemRoot\System32\DriverStore\FileRepository\', [StringComparison]::OrdinalIgnoreCase)) { Fail 'DRIVER_PATH' 'Experimental driver path does not match the supported installation.' }
    $driverPath = Join-Path ([Environment]::GetFolderPath('Windows')) $image.Substring('\SystemRoot\'.Length)
    $driverPath=[IO.Path]::GetFullPath($driverPath)
    $driverStore=[IO.Path]::GetFullPath((Join-Path ([Environment]::GetFolderPath('Windows')) 'System32\DriverStore\FileRepository')).TrimEnd('\')+'\'
    if(-not $driverPath.StartsWith($driverStore,[StringComparison]::OrdinalIgnoreCase)){Fail 'DRIVER_PATH' 'Experimental driver path escapes the driver store.'}
    Assert-NoLink $driverPath
    if ((Get-FileHash -LiteralPath $driverPath -Algorithm SHA256).Hash -ine $driverSha) { Fail 'DRIVER_HASH' 'Experimental driver does not match the tested release; Setup will not replace it.' }
    return [pscustomobject]@{hardwareId=$hardwareId; instanceId=$device.InstanceId; driverSha256=$driverSha}
}
function Get-Bindings([bool]$RequireExisting) {
    $task = Get-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction SilentlyContinue
    if ($RequireExisting -and -not $task) { Fail 'TASK_MISSING' 'Existing helper task is missing; repair it before updating.' }
    if ($task) {
        $principalSid = $task.Principal.UserId
        if ($principalSid -notmatch '^S-1-') { $principalSid=(New-Object Security.Principal.NTAccount($principalSid)).Translate([Security.Principal.SecurityIdentifier]).Value }
        if (@($task.Actions).Count -ne 1 -or $task.Actions[0].Execute -ine $exe -or $task.Actions[0].Arguments -cne $hostArgument -or $task.Actions[0].WorkingDirectory -ine $installRoot -or $principalSid -cne $ownerSid -or $task.Principal.RunLevel -ne 'Highest' -or $task.Principal.LogonType -ne 'Interactive' -or ($null -ne $task.Triggers -and @($task.Triggers).Count -ne 0)) { Fail 'TASK_COLLISION' 'The helper task differs from this product; it will not be overwritten.' }
    }
    $run = Get-ItemProperty -LiteralPath $runKey -Name 'XiatiaoAirDrop' -ErrorAction SilentlyContinue
    if ($run -and $run.XiatiaoAirDrop -cne $runValue) { Fail 'STARTUP_COLLISION' 'The startup entry belongs to another command; it will not be changed.' }
    $shortcutExists = Test-Path -LiteralPath $shortcutPath
    if ($shortcutExists) {
        Assert-NoLink $shortcutPath
        $shell=New-Object -ComObject WScript.Shell
        $shortcut=$shell.CreateShortcut($shortcutPath)
        if ($shortcut.TargetPath -ine $exe -or $shortcut.Arguments -ne '') { Fail 'SHORTCUT_COLLISION' 'The Start Menu shortcut points elsewhere; it will not be changed.' }
    }
    if (-not $RequireExisting -and ($task -or $run -or $shortcutExists)) { Fail 'ORPHAN_BINDINGS' 'Product registrations exist without a verified installation; no existing entry was replaced.' }
    return [pscustomobject]@{Task=$task; Run=[bool]$run; Shortcut=[bool]$shortcutExists}
}
function Assert-Idle {
    $active = @(Get-CimInstance Win32_Process | Where-Object {
        $_.Name -in @('XiatiaoAirDrop.exe','winawdl_mt7925.exe','winawdl_radio.exe','awdld.exe') -or
        ($_.Name -in @('python.exe','pythonw.exe') -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith($installRoot + '\',[StringComparison]::OrdinalIgnoreCase))
    })
    if ($active.Count) { Fail 'APP_RUNNING' 'Exit Airdrop for Windows from its tray menu and wait for the helper to close. Setup never force-stops it. / 请先从托盘退出软件。' }
    $task=Get-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction SilentlyContinue
    if ($task -and $task.State -eq 'Running') { Fail 'HELPER_RUNNING' 'The radio helper is still running. Wait for normal shutdown.' }
    foreach ($name in @('XiatiaoAirDrop-TCP','XiatiaoAirDrop-mDNS')) { if (Get-NetFirewallRule -Name $name -ErrorAction SilentlyContinue) { Fail 'RADIO_ACTIVE' 'Radio firewall rules remain active; let the helper finish normal shutdown.' } }
}
function Protect-Tree([string]$Root) {
    Assert-NoLink $Root
    $admins=New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')
    $system=New-Object Security.Principal.SecurityIdentifier('S-1-5-18')
    $users=New-Object Security.Principal.SecurityIdentifier('S-1-5-32-545')
    foreach ($item in @((Get-Item -LiteralPath $Root -Force)) + @(Get-TreeItems $Root)) {
        $acl=if($item.PSIsContainer){New-Object Security.AccessControl.DirectorySecurity}else{New-Object Security.AccessControl.FileSecurity}
        $acl.SetOwner($admins); $acl.SetAccessRuleProtection($true,$false)
        $inherit=if($item.PSIsContainer){[Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'}else{[Security.AccessControl.InheritanceFlags]::None}
        foreach($sid in @($admins,$system)){ $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'FullControl',$inherit,'None','Allow'))) }
        $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($users,'ReadAndExecute',$inherit,'None','Allow')))
        Set-Acl -LiteralPath $item.FullName -AclObject $acl
    }
}
function Assert-TransactionPath([string]$Path,[string]$Token) {
    $full=[IO.Path]::GetFullPath($Path)
    if([IO.Path]::GetDirectoryName($full) -ine $programFiles -or [IO.Path]::GetFileName($full) -notmatch ('^XiatiaoAirDrop\.(setup-stage|setup-backup|setup-failed|uninstall)-'+[regex]::Escape($Token)+'$')){Fail 'TRANSACTION_PATH' 'Unexpected transaction directory.'}
    Assert-NoLink $full
}
function Assert-FrozenComponents($Old,$New) {
    # One paired, content-pinned example-name cleanup. Both files must match
    # the exact old/new pair; this is not a general receiver update allowance.
    $exampleNameCleanup = (
        $Old.Files['receiver/receive_trial.py'] -ieq '07261546dc3bbf20604699f98236b8d7f9c0335333b272e40ff624f898fdab6d' -and
        $New.Files['receiver/receive_trial.py'] -ieq '5d83b392de7ab14346c6814c6a77c081f5da1a1ddae3803e7c449d02a630c936' -and
        $Old.Files['receiver/manifest.json'] -ieq '55f632ff5b86e6b42ed67253722062e10f5a9d4679678637e98d450e3206814a' -and
        $New.Files['receiver/manifest.json'] -ieq '0b6121fbe150e4852ea7a4975fb2fed45481cb3573599682001f1629fe9035a9'
    )
    foreach($prefix in @('runtime/','firmware/','python/','receiver/','assets/','source/app/assets/')){
        $before=@($Old.Files.Keys|Where-Object{$_.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)}|Sort-Object)
        $after=@($New.Files.Keys|Where-Object{$_.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)}|Sort-Object)
        if(($before -join "`n") -cne ($after -join "`n")){Fail 'FROZEN_FILES' "Protected file set changed: $prefix"}
        foreach($name in $before){if($Old.Files[$name] -ine $New.Files[$name]){
            if($exampleNameCleanup -and $name -in @('receiver/receive_trial.py','receiver/manifest.json')){continue}
            Fail 'FROZEN_HASH' "Protected runtime changed: $name"
        }}
    }
    if($Old.Manifest.sourceRuntime -cne $New.Manifest.sourceRuntime -or $Old.Manifest.sourceReceiver -cne $New.Manifest.sourceReceiver){Fail 'FROZEN_SOURCE' 'Radio and transfer engine provenance must remain unchanged.'}
}
function Remove-VerifiedTree([string]$Root,$Tree,[string]$ReceiptSha,[string]$Token) {
    # No recursive delete. Unknown content or any hash drift stops deletion.
    Assert-TransactionPath $Root $Token
    Read-VerifiedTree $Root $Tree.Sha $true | Out-Null
    $receiptPath=Join-Path $Root 'installation.json'
    if((Get-FileHash -LiteralPath $receiptPath -Algorithm SHA256).Hash -ine $ReceiptSha){Fail 'RECEIPT_CHANGED' 'Receipt changed before cleanup; backup preserved.'}
    foreach($name in $Tree.Files.Keys){
        $path=Safe-Child $Root $name
        if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $Tree.Files[$name]){Fail 'CLEANUP_CHANGED' 'File changed during cleanup; remaining files preserved.'}
        Remove-Item -LiteralPath $path -Force
    }
    foreach($name in @('runtime-manifest.json','installation.json')){
        $path=Safe-Child $Root $name
        $expected=if($name -eq 'installation.json'){$ReceiptSha}else{$Tree.Sha}
        if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $expected){Fail 'CLEANUP_CHANGED' 'Metadata changed during cleanup.'}
        Remove-Item -LiteralPath $path -Force
    }
    foreach($directory in @(Get-TreeItems $Root | Where-Object{$_.PSIsContainer}|Sort-Object {$_.FullName.Length} -Descending)){
        Assert-NoLink $directory.FullName
        if(@(Get-ChildItem -LiteralPath $directory.FullName -Force).Count){Fail 'CLEANUP_NONEMPTY' 'New or unknown content appeared; directory preserved.'}
        Remove-Item -LiteralPath $directory.FullName -Force
    }
    Assert-TransactionPath $Root $Token
    if(@(Get-ChildItem -LiteralPath $Root -Force).Count){Fail 'CLEANUP_NONEMPTY' 'Unknown root content was preserved.'}
    Remove-Item -LiteralPath $Root -Force
}
function Acquire-Guards {
    $guards=New-Object 'System.Collections.Generic.List[System.Threading.Mutex]'
    try{
        foreach($name in @("Global\XiatiaoAirDrop.Setup", "Local\XiatiaoAirDrop.App.$ownerSid", "Local\XiatiaoAirDrop.Radio.$ownerSid.host")){
            $created=$false; $guard=New-Object Threading.Mutex($true,$name,[ref]$created)
            if(-not $created){$guard.Dispose();Fail 'APP_LOCKED' 'Another installer, app or helper is active. Exit it normally before retrying.'}
            $guards.Add($guard)
        }
        return ,$guards
    }catch{foreach($guard in $guards){try{$guard.ReleaseMutex()}catch{};$guard.Dispose()};throw}
}
function Register-Helper([ref]$Created) {
    $action=New-ScheduledTaskAction -Execute $exe -Argument $hostArgument -WorkingDirectory $installRoot
    $principal=New-ScheduledTaskPrincipal -UserId $ownerSid -LogonType Interactive -RunLevel Highest
    $settings=New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $taskName -TaskPath '\' -Action $action -Principal $principal -Settings $settings -Description 'Airdrop for Windows restricted radio helper. Starts on demand for the installing user; no automatic triggers.' | Out-Null
    $Created.Value=$true
    $scheduler=New-Object -ComObject 'Schedule.Service';$scheduler.Connect()
    $scheduler.GetFolder('\').GetTask($taskName).SetSecurityDescriptor("O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;$ownerSid)",0)
}
function Set-Startup([bool]$Enabled) {
    $current=Get-ItemProperty -LiteralPath $runKey -Name 'XiatiaoAirDrop' -ErrorAction SilentlyContinue
    if($current -and $current.XiatiaoAirDrop -cne $runValue){Fail 'STARTUP_COLLISION' 'Startup entry changed during Setup.'}
    if($Enabled){New-Item -Path $runKey -Force|Out-Null;New-ItemProperty -LiteralPath $runKey -Name 'XiatiaoAirDrop' -PropertyType String -Value $runValue -Force|Out-Null}
    elseif($current){Remove-ItemProperty -LiteralPath $runKey -Name 'XiatiaoAirDrop'}
}
function Write-Shortcut {
    Assert-NoLink $shortcutPath
    if(Test-Path -LiteralPath $shortcutPath){Fail 'SHORTCUT_COLLISION' 'Shortcut appeared during Setup.'}
    $shell=New-Object -ComObject WScript.Shell;$shortcut=$shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath=$exe;$shortcut.WorkingDirectory=$installRoot;$shortcut.IconLocation=Join-Path $installRoot 'assets\xiatiao-airdrop.ico'
    $shortcut.Description='Airdrop for Windows';$shortcut.Save()
}
function Assert-SetupPath([string]$Path,[string]$Token) {
    $full=[IO.Path]::GetFullPath($Path)
    if($full -ine $setupRoot -and ([IO.Path]::GetDirectoryName($full) -ine $setupParent -or [IO.Path]::GetFileName($full) -notmatch ('^AirdropForWindowsSetup\.(stage|backup|failed)-'+[regex]::Escape($Token)+'$'))){Fail 'SETUP_PATH' 'Unexpected uninstaller directory.'}
    Assert-NoLink $full
}
function Read-SetupTree([string]$Root,[string]$ManifestSha,[string]$InstallerSha) {
    Assert-NoLink $Root
    $items=@(Get-TreeItems $Root)
    if($items.Count -ne 2 -or @($items|Where-Object{$_.PSIsContainer -or $_.Name -notin @('Setup.exe','setup-receipt.json')}).Count){Fail 'SETUP_EXTRA' 'Uninstaller directory contains unknown content; it was preserved.'}
    $receiptPath=Join-Path $Root 'setup-receipt.json'
    $bytes=[IO.File]::ReadAllBytes($receiptPath)
    $receipt=[Text.Encoding]::UTF8.GetString($bytes).TrimStart([char]0xFEFF)|ConvertFrom-Json
    if($receipt.schema -cne 'airdrop.setup.v1' -or $receipt.ownerSid -cne $ownerSid -or $receipt.setupRoot -ine $setupRoot -or $receipt.manifestSha256 -ine $ManifestSha -or $receipt.installerSha256 -ine $InstallerSha -or (Get-FileHash -LiteralPath (Join-Path $Root 'Setup.exe') -Algorithm SHA256).Hash -ine $InstallerSha){Fail 'SETUP_IDENTITY' 'Uninstaller files or receipt do not match the installed release.'}
    return [pscustomobject]@{Root=$Root;ManifestSha=$ManifestSha;InstallerSha=$InstallerSha;ReceiptSha=(Hash-Bytes $bytes)}
}
function Get-ArpValues([string]$InstallerSha) {
    return [ordered]@{DisplayName='Airdrop for Windows';DisplayVersion='0.2.4';Publisher='Dy1an-Zou';InstallLocation=$installRoot;DisplayIcon=($exe+',0');UninstallString=('"'+$setupExe+'" --uninstall');NoModify=1;NoRepair=1;InstallOwnerSid=$ownerSid;SetupSha256=$InstallerSha}
}
function Assert-Arp([string]$InstallerSha) {
    if(-not (Test-Path -LiteralPath $uninstallKey)){Fail 'UNINSTALL_REGISTRATION' 'Apps uninstall registration is missing.'}
    $key=Get-Item -LiteralPath $uninstallKey
    $expected=Get-ArpValues $InstallerSha
    if(@($key.GetSubKeyNames()).Count -or @($key.GetValueNames()).Count -ne $expected.Count){Fail 'UNINSTALL_REGISTRATION' 'Apps uninstall registration has unknown content.'}
    foreach($name in $expected.Keys){if($key.GetValue($name) -cne $expected[$name]){Fail 'UNINSTALL_REGISTRATION' 'Apps uninstall registration was changed; it was preserved.'}}
}
function Write-Arp([string]$InstallerSha) {
    $expected=Get-ArpValues $InstallerSha
    New-Item -Path $uninstallKey -Force|Out-Null
    foreach($name in $expected.Keys){$kind=if($name -in @('NoModify','NoRepair')){'DWord'}else{'String'};New-ItemProperty -LiteralPath $uninstallKey -Name $name -Value $expected[$name] -PropertyType $kind -Force|Out-Null}
    Assert-Arp $InstallerSha
}
function Get-SetupState($Old) {
    $hasSetup=$Old -and $Old.Receipt.PSObject.Properties['setupSha256']
    if($hasSetup){
        if($Old.Receipt.setupRoot -ine $setupRoot -or $Old.Receipt.setupSha256 -notmatch '^[0-9a-fA-F]{64}$'){Fail 'SETUP_RECEIPT' 'Product receipt has an invalid uninstaller identity.'}
        $state=Read-SetupTree $setupRoot $Old.Sha $Old.Receipt.setupSha256
        Assert-ProtectedItem $setupRoot
        foreach($item in Get-TreeItems $setupRoot){Assert-ProtectedItem $item.FullName}
        Assert-Arp $state.InstallerSha
        return $state
    }
    if((Test-Path -LiteralPath $setupRoot) -or (Test-Path -LiteralPath $uninstallKey)){Fail 'SETUP_COLLISION' 'An unregistered uninstaller folder or Apps entry exists; it was preserved.'}
    return $null
}
function Read-InstallerSource {
    if([string]::IsNullOrWhiteSpace($InstallerPath) -or -not [IO.Path]::IsPathRooted($InstallerPath) -or $InstallerPath.StartsWith('\\')){Fail 'INSTALLER_REQUIRED' 'InstallerPath must name the current local Setup executable.'}
    $path=[IO.Path]::GetFullPath($InstallerPath);Assert-NoLink $path
    if([IO.Path]::GetExtension($path) -ine '.exe' -or -not (Test-Path -LiteralPath $path -PathType Leaf)){Fail 'INSTALLER_REQUIRED' 'Setup executable was not found.'}
    return [pscustomobject]@{Path=$path;Sha=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
function Stage-Setup([string]$Root,$Source,[string]$ManifestSha,[string]$Token) {
    Assert-SetupPath $Root $Token
    if(Test-Path -LiteralPath $Root){Fail 'SETUP_STAGE' 'Uninstaller staging path is occupied.'}
    [IO.Directory]::CreateDirectory($Root)|Out-Null;Protect-Tree $Root
    Copy-Item -LiteralPath $Source.Path -Destination (Join-Path $Root 'Setup.exe')
    $receipt=[ordered]@{schema='airdrop.setup.v1';ownerSid=$ownerSid;setupRoot=$setupRoot;manifestSha256=$ManifestSha;installerSha256=$Source.Sha}
    [IO.File]::WriteAllText((Join-Path $Root 'setup-receipt.json'),($receipt|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    Protect-Tree $Root;Read-SetupTree $Root $ManifestSha $Source.Sha|Out-Null
}
function Remove-SetupTree([string]$Root,$State,[string]$Token) {
    Assert-SetupPath $Root $Token
    $current=Read-SetupTree $Root $State.ManifestSha $State.InstallerSha
    if($current.ReceiptSha -ine $State.ReceiptSha){Fail 'SETUP_CHANGED' 'Uninstaller receipt changed before cleanup.'}
    # An executing Setup.exe may be locked. In that case leave both files intact.
    Remove-Item -LiteralPath (Join-Path $Root 'Setup.exe') -Force
    Remove-Item -LiteralPath (Join-Path $Root 'setup-receipt.json') -Force
    if(@(Get-ChildItem -LiteralPath $Root -Force).Count){Fail 'SETUP_EXTRA' 'Unknown uninstaller content was preserved.'}
    Remove-Item -LiteralPath $Root -Force
}
function Install-Payload($New,$Old,$Bindings,[bool]$StartOnLogin,$Installer,$OldSetup) {
    $token=[guid]::NewGuid().ToString('N')
    $stage=$installRoot+'.setup-stage-'+$token;$backup=$installRoot+'.setup-backup-'+$token;$failed=$installRoot+'.setup-failed-'+$token
    foreach($path in @($stage,$backup,$failed)){Assert-TransactionPath $path $token;if(Test-Path -LiteralPath $path){Fail 'STAGE_EXISTS' 'Transaction path is already occupied.'}}
    $setupStage=$setupRoot+'.stage-'+$token;$setupBackup=$setupRoot+'.backup-'+$token;$setupFailed=$setupRoot+'.failed-'+$token
    foreach($path in @($setupStage,$setupBackup,$setupFailed)){Assert-SetupPath $path $token;if(Test-Path -LiteralPath $path){Fail 'SETUP_STAGE' 'Uninstaller transaction path is occupied.'}}
    $guards=Acquire-Guards;$backedUp=$false;$swapped=$false;$taskAdded=$false;$shortcutAdded=$false;$startupTouched=$false;$committed=$false;$setupBackedUp=$false;$setupSwapped=$false;$arpTouched=$false
    try{
        Assert-Idle
        [IO.Directory]::CreateDirectory($stage)|Out-Null;Protect-Tree $stage
        foreach($entry in $New.Manifest.files.PSObject.Properties){$source=Safe-Child $New.Root $entry.Name;$target=Safe-Child $stage $entry.Name;[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))|Out-Null;Copy-Item -LiteralPath $source -Destination $target}
        [IO.File]::WriteAllBytes((Join-Path $stage 'runtime-manifest.json'),$New.Bytes)
        $receipt=[ordered]@{schema='xiatiao.install.v1';installedUtc=[DateTime]::UtcNow.ToString('o');ownerSid=$ownerSid;installRoot=$installRoot;task=$taskName;shortcut=$shortcutPath;manifestSha256=$New.Sha;version='0.2.4';updateKind='public-setup';updateToken=$token;setupRoot=$setupRoot;setupSha256=$Installer.Sha}
        if($Old){$receipt.installedUtc=$Old.Receipt.installedUtc;$receipt.previousManifestSha256=$Old.Sha;$receipt.updatedUtc=[DateTime]::UtcNow.ToString('o')}
        [IO.File]::WriteAllText((Join-Path $stage 'installation.json'),($receipt|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
        Protect-Tree $stage;Read-VerifiedTree $stage $New.Sha $true|Out-Null
        if($Old){Read-VerifiedTree $installRoot $Old.Sha $true|Out-Null;if((Get-FileHash -LiteralPath (Join-Path $installRoot 'installation.json') -Algorithm SHA256).Hash -ine $Old.ReceiptSha){Fail 'RECEIPT_CHANGED' 'Old receipt changed before replacement.'}}
        Stage-Setup $setupStage $Installer $New.Sha $token
        Assert-Idle;Get-Bindings ([bool]$Old)|Out-Null;Get-SetupState $Old|Out-Null
        if($Old){Move-Item -LiteralPath $installRoot -Destination $backup;$backedUp=$true}
        Assert-NoLink $installRoot;if(Test-Path -LiteralPath $installRoot){Fail 'TARGET_CHANGED' 'Installation path appeared during Setup.'}
        Move-Item -LiteralPath $stage -Destination $installRoot;$swapped=$true
        Read-VerifiedTree $installRoot $New.Sha $true|Out-Null
        foreach($path in @($dataRoot,$radioRoot)){
            Assert-NoLink $path
            if(Test-Path -LiteralPath $path){Assert-ProtectedItem $path}
            else{[IO.Directory]::CreateDirectory($path)|Out-Null;Protect-Tree $path}
        }
        if(-not $Bindings.Task){Register-Helper ([ref]$taskAdded)}
        if(-not $Bindings.Shortcut){Write-Shortcut;$shortcutAdded=$true}
        $startupTouched=$true;Set-Startup $StartOnLogin
        Get-Bindings $true|Out-Null
        if($OldSetup){Move-Item -LiteralPath $setupRoot -Destination $setupBackup;$setupBackedUp=$true}
        Assert-NoLink $setupRoot;if(Test-Path -LiteralPath $setupRoot){Fail 'SETUP_COLLISION' 'Uninstaller target appeared during Setup.'}
        Move-Item -LiteralPath $setupStage -Destination $setupRoot;$setupSwapped=$true
        Read-SetupTree $setupRoot $New.Sha $Installer.Sha|Out-Null
        if($OldSetup){Assert-Arp $OldSetup.InstallerSha}elseif(Test-Path -LiteralPath $uninstallKey){Fail 'UNINSTALL_REGISTRATION' 'An unrelated Apps entry appeared during Setup.'}
        $arpTouched=$true;Write-Arp $Installer.Sha
        Read-VerifiedTree $installRoot $New.Sha $true|Out-Null
        $committed=$true
        $cleanupWarning=$null
        if($Old){try{Remove-VerifiedTree $backup $Old $Old.ReceiptSha $token}catch{$cleanupWarning=$_.Exception.Message}}
        if($OldSetup){try{Remove-SetupTree $setupBackup $OldSetup $token}catch{$cleanupWarning=([string]$cleanupWarning+' '+$_.Exception.Message).Trim()}}
        return [ordered]@{status=$(if($Old){'updated'}else{'installed'});version='0.2.4';installRoot=$installRoot;manifestSha256=$New.Sha;ownerSid=$ownerSid;autostart=$StartOnLogin;backup=$(if(Test-Path -LiteralPath $backup){$backup}else{$null});setupBackup=$(if(Test-Path -LiteralPath $setupBackup){$setupBackup}else{$null});cleanupWarning=$cleanupWarning;driverChanged=$false;applicationStarted=$false;userDataPreserved=$true}
    }catch{
        $cause=$_.Exception.Message
        if(-not $committed){
            try{
                Assert-Idle
                if($arpTouched){if($OldSetup){Write-Arp $OldSetup.InstallerSha}elseif(Test-Path -LiteralPath $uninstallKey){$key=Get-Item -LiteralPath $uninstallKey;$allowed=(Get-ArpValues $Installer.Sha).Keys;if(@($key.GetSubKeyNames()).Count -or @($key.GetValueNames()|Where-Object{$_ -notin $allowed}).Count){Fail 'ARP_ROLLBACK' 'Unknown uninstall registration preserved.'};Remove-Item -LiteralPath $uninstallKey}}
                if($setupSwapped){Assert-SetupPath $setupFailed $token;Move-Item -LiteralPath $setupRoot -Destination $setupFailed}
                if($setupBackedUp){Read-SetupTree $setupBackup $OldSetup.ManifestSha $OldSetup.InstallerSha|Out-Null;if(Test-Path -LiteralPath $setupRoot){Fail 'SETUP_ROLLBACK' 'Uninstaller rollback target occupied.'};Move-Item -LiteralPath $setupBackup -Destination $setupRoot}
                if($startupTouched){Set-Startup $Bindings.Run}
                if($shortcutAdded){Assert-NoLink $shortcutPath;$shell=New-Object -ComObject WScript.Shell;if($shell.CreateShortcut($shortcutPath).TargetPath -ine $exe){Fail 'SHORTCUT_CHANGED' 'Shortcut changed during rollback.'};Remove-Item -LiteralPath $shortcutPath}
                if($taskAdded){$task=Get-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction SilentlyContinue;if($task -and $task.Actions.Execute -ieq $exe -and $task.Actions.Arguments -ceq $hostArgument -and $task.State -ne 'Running'){Unregister-ScheduledTask -TaskName $taskName -TaskPath '\' -Confirm:$false}}
                if($swapped){Assert-TransactionPath $failed $token;Assert-NoLink $installRoot;Move-Item -LiteralPath $installRoot -Destination $failed}
                if($backedUp){Read-VerifiedTree $backup $Old.Sha $true|Out-Null;if((Get-FileHash -LiteralPath (Join-Path $backup 'installation.json') -Algorithm SHA256).Hash -ine $Old.ReceiptSha){Fail 'ROLLBACK_RECEIPT' 'Backup receipt changed.'};if(Test-Path -LiteralPath $installRoot){Fail 'ROLLBACK_OCCUPIED' 'Rollback target is occupied.'};Move-Item -LiteralPath $backup -Destination $installRoot;Read-VerifiedTree $installRoot $Old.Sha $true|Out-Null}
            }catch{Fail 'ROLLBACK_INCOMPLETE' "$cause; rollback: $($_.Exception.Message). Files preserved at $backup; $stage; $failed"}
        }
        Fail 'INSTALL_FAILED' "$cause; original installation restored or untouched. Diagnostic files preserved at $stage; $failed"
    }finally{foreach($guard in $guards){try{$guard.ReleaseMutex()}catch [ApplicationException]{};$guard.Dispose()}}
}
function Uninstall-Product($Old,$Bindings,$SetupState) {
    $token=[guid]::NewGuid().ToString('N');$quarantine=$installRoot+'.uninstall-'+$token
    Assert-TransactionPath $quarantine $token
    if(Test-Path -LiteralPath $quarantine){Fail 'UNINSTALL_PATH' 'Uninstall transaction path already exists.'}
    $guards=Acquire-Guards;$moved=$false;$deleting=$false;$taskRemoved=$false;$shortcutRemoved=$false;$startupRemoved=$false
    $shortcutBytes=if($Bindings.Shortcut){[IO.File]::ReadAllBytes($shortcutPath)}else{$null}
    try{
        Assert-Idle;Get-Bindings $true|Out-Null
        Read-VerifiedTree $installRoot $Old.Sha $true|Out-Null
        if((Get-FileHash -LiteralPath (Join-Path $installRoot 'installation.json') -Algorithm SHA256).Hash -ine $Old.ReceiptSha){Fail 'RECEIPT_CHANGED' 'Receipt changed before uninstall.'}
        Move-Item -LiteralPath $installRoot -Destination $quarantine;$moved=$true
        if($Bindings.Run){Set-Startup $false;$startupRemoved=$true}
        if($Bindings.Shortcut){Assert-NoLink $shortcutPath;if((Hash-Bytes ([IO.File]::ReadAllBytes($shortcutPath))) -cne (Hash-Bytes $shortcutBytes)){Fail 'SHORTCUT_CHANGED' 'Shortcut changed before removal.'};Remove-Item -LiteralPath $shortcutPath;$shortcutRemoved=$true}
        Unregister-ScheduledTask -TaskName $taskName -TaskPath '\' -Confirm:$false;$taskRemoved=$true
        Read-VerifiedTree $quarantine $Old.Sha $true|Out-Null
        $deleting=$true;Remove-VerifiedTree $quarantine $Old $Old.ReceiptSha $token
        $setupPending=$false;$setupWarning=$null
        if($SetupState){
            Assert-Arp $SetupState.InstallerSha
            Remove-Item -LiteralPath $uninstallKey
            try{Remove-SetupTree $setupRoot $SetupState $token}catch{$setupPending=$true;$setupWarning=$_.Exception.Message}
        }
        return [ordered]@{status='uninstalled';manifestSha256=$Old.Sha;userFilesPreserved=$true;userSettingsPreserved=$true;programDataPreserved=$true;driverPreserved=$true;securityPolicyUnchanged=$true;setupCleanupPending=$setupPending;setupFolder=$(if($setupPending){$setupRoot}else{$null});setupCleanupWarning=$setupWarning}
    }catch{
        $cause=$_.Exception.Message
        if($moved -and -not $deleting){
            try{
                Read-VerifiedTree $quarantine $Old.Sha $true|Out-Null
                if(Test-Path -LiteralPath $installRoot){Fail 'ROLLBACK_OCCUPIED' 'Uninstall rollback target occupied.'}
                Move-Item -LiteralPath $quarantine -Destination $installRoot
                if($taskRemoved){$restoredTask=$false;Register-Helper ([ref]$restoredTask)}
                if($startupRemoved){Set-Startup $true}
                if($shortcutRemoved){Assert-NoLink $shortcutPath;if(Test-Path -LiteralPath $shortcutPath){Fail 'SHORTCUT_COLLISION' 'Shortcut appeared during rollback.'};[IO.File]::WriteAllBytes($shortcutPath,$shortcutBytes)}
            }catch{Fail 'ROLLBACK_INCOMPLETE' "$cause; rollback: $($_.Exception.Message). Files preserved at $quarantine"}
        }
        Fail 'UNINSTALL_FAILED' "$cause; remaining program files preserved at $quarantine. User data and drivers were not deleted."
    }finally{foreach($guard in $guards){try{$guard.ReleaseMutex()}catch [ApplicationException]{};$guard.Dispose()}}
}
function Write-Result($Value) {
    $json=$Value|ConvertTo-Json -Depth 8
    if($ResultPath -and $script:resultPathApproved){
        $path=[IO.Path]::GetFullPath($ResultPath);Assert-NoLink $path
        $bytes=[Text.UTF8Encoding]::new($false).GetBytes($json)
        $stream=[IO.File]::Open($path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush()}finally{$stream.Dispose()}
    }
    Write-Output $json
}

try {
    # Reject an unusable result path before any product mutation.
    if($ResultPath){
        $resultFull=[IO.Path]::GetFullPath($ResultPath);Assert-NoLink $resultFull
        foreach($protectedRoot in @($installRoot,$setupRoot,$dataRoot)){if($resultFull -ieq $protectedRoot -or $resultFull.StartsWith($protectedRoot+'\',[StringComparison]::OrdinalIgnoreCase)){Fail 'RESULT_PATH' 'Result file must remain outside product, uninstaller and radio data directories.'}}
        if((Test-Path -LiteralPath $resultFull) -or -not (Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($resultFull)) -PathType Container)){Fail 'RESULT_PATH' 'Result file must be a new file in an existing directory.'}
        $script:resultPathApproved=$true
    }
    if($Mode -in @('uninstall','uninstall-preflight')){
        Assert-Owner ($Mode -eq 'uninstall')
        $old=Read-Installed
        if(-not $old -or $old.Sha -ine $ExpectedManifestSha256){Fail 'UNINSTALL_VERSION' 'This uninstaller does not match the installed version.'}
        $bindings=Get-Bindings $true;$setupState=Get-SetupState $old;Assert-Idle
        if($Mode -eq 'uninstall-preflight'){$result=[ordered]@{status='ready';mode=$Mode;manifestSha256=$old.Sha;ownerSid=$ownerSid;mutations=0}}
        else{$result=Uninstall-Product $old $bindings $setupState}
    }else{
        if([string]::IsNullOrWhiteSpace($PackageDir)){Fail 'PACKAGE_REQUIRED' 'PackageDir is required.'}
        $package=[IO.Path]::GetFullPath($PackageDir).TrimEnd('\');Assert-NoLink $package
        if($package -ieq $installRoot -or $package.StartsWith($installRoot+'\',[StringComparison]::OrdinalIgnoreCase)){Fail 'PACKAGE_LOCATION' 'Payload cannot be the installed product.'}
        $new=Read-VerifiedTree $package $ExpectedManifestSha256 $false
        $version=[Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $package 'XiatiaoAirDrop.dll'))
        if($version.FileVersion -ne '0.2.4.0' -or $version.ProductVersion -notmatch '^0\.2\.4(\+|$)'){Fail 'PAYLOAD_VERSION' 'This installer requires the 0.2.4 payload.'}
        if($Mode -eq 'verify-payload'){
            $result=[ordered]@{status='verified';version='0.2.4';manifestSha256=$new.Sha;files=$new.Files.Count;mutations=0}
        }else{
            Assert-Owner ($Mode -eq 'install');$device=Assert-Requirements $new.Backend;$old=Read-Installed
            if($old){if($old.Sha -notin $legacyManifests -and $old.Sha -ine $new.Sha){Fail 'UPGRADE_VERSION' 'Only the verified 0.2.2 / 0.2.3 installation or this exact 0.2.4 release can be updated.'};Assert-FrozenComponents $old $new}
            $bindings=Get-Bindings ([bool]$old);$setupState=Get-SetupState $old;$installer=Read-InstallerSource;Assert-Idle
            $startOnLogin=if($AutoStart -eq 'preserve'){if($old){$bindings.Run}else{$true}}else{$AutoStart -eq 'enabled'}
            if($Mode -eq 'preflight'){$result=[ordered]@{status='ready';version='0.2.4';manifestSha256=$new.Sha;installRoot=$installRoot;ownerSid=$ownerSid;upgrade=[bool]$old;autostart=$startOnLogin;device=$device;mutations=0}}
            else{$result=Install-Payload $new $old $bindings $startOnLogin $installer $setupState}
        }
    }
    Write-Result $result
    exit 0
} catch {
    $failure=[ordered]@{status='failed';mode=$Mode;error=$_.Exception.Message;driverChanged=$false;applicationStarted=$false}
    try{Write-Result $failure}catch{Write-Output ($failure|ConvertTo-Json -Depth 5);Write-Warning $_.Exception.Message}
    exit 1
}
