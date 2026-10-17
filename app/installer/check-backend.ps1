# SPDX-License-Identifier: MIT
# Offline fixture checks only. Imports function ASTs, never the backend entrypoint.
[CmdletBinding()]
param([string]$BackendPath)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if([string]::IsNullOrWhiteSpace($BackendPath)){$BackendPath=Join-Path $PSScriptRoot 'setup-backend.ps1'}
$tokens=$null;$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($BackendPath,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw ($parseErrors|Out-String)}
$definitions=@($ast.FindAll({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst]},$false))
. ([scriptblock]::Create(($definitions|ForEach-Object{$_.Extent.Text}) -join "`n"))
$testRoot=Join-Path ([IO.Path]::GetFullPath($env:TEMP)) ('airdrop-setup-check-'+[guid]::NewGuid().ToString('N'))
if(Test-Path -LiteralPath $testRoot){throw 'Fixture directory already exists'}
[IO.Directory]::CreateDirectory($testRoot)|Out-Null
$passed=New-Object 'System.Collections.Generic.List[string]'
function Expect([string]$Name,[bool]$Condition){if(-not $Condition){throw "CHECK_FAILED: $Name"};$passed.Add($Name)}
function Reject([string]$Name,[scriptblock]$Action,[string]$Code){
    $caught=$null
    try{& $Action|Out-Null}catch{$caught=$_.Exception.Message}
    Expect $Name ($null -ne $caught -and $caught.StartsWith($Code+':',[StringComparison]::Ordinal))
}
$required=@('xiatiaoairdrop.exe','xiatiaoairdrop.dll','xiatiaoairdrop.deps.json','xiatiaoairdrop.runtimeconfig.json','python/python.exe','receiver/engine.py','receiver/sender.py','receiver/airdrop-beacon.ps1','assets/xiatiao-airdrop.ico','assets/xiatiao-airdrop.png','licenses/third-party-notices.md','runtime/awdld.exe','runtime/winawdl_mt7925.exe','runtime/wintun.dll','firmware/WIFI_MT7925_PATCH_MCU_1_1_hdr.bin','firmware/WIFI_RAM_CODE_MT7925_1_1.bin')
$fixture=Join-Path $testRoot 'payload'
[IO.Directory]::CreateDirectory($fixture)|Out-Null
$files=[ordered]@{}
foreach($name in $required){$path=Join-Path $fixture $name;[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))|Out-Null;[IO.File]::WriteAllText($path,('fixture:'+ $name));$files[$name]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
# Pins are fixture contents: these checks exercise manifest/hash/path contracts,
# and do not claim to validate a real native binary or installed device.
$pins=@{}
foreach($name in $files.Keys){if($name.StartsWith('runtime/') -or $name.StartsWith('firmware/')){$pins[$name]=$files[$name]}}
function Write-FixtureManifest($Members){
    $manifest=[ordered]@{schema='xiatiao.runtime.v1';product='XiatiaoAirDrop';sourceRuntime='fixture-runtime';sourceReceiver='fixture-receiver';files=$Members}
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($manifest|ConvertTo-Json -Depth 6))
    [IO.File]::WriteAllBytes((Join-Path $fixture 'runtime-manifest.json'),$bytes)
    return (Hash-Bytes $bytes)
}
$sha=Write-FixtureManifest $files
$tree=Read-VerifiedTree $fixture $sha $false
Expect 'valid complete fixture' ($tree.Files.Count -eq $required.Count)
foreach($invalid in @('../escape','C:\escape','runtime/../escape','runtime//escape','runtime/a:b','runtime/con.txt','runtime/file.','runtime/a?/b')){
    Reject ('reject member '+$invalid) {Safe-Child $fixture $invalid} 'MANIFEST_PATH'
}
Reject 'manifest hash mismatch' {Read-VerifiedTree $fixture ('0'*64) $false} 'MANIFEST_HASH'
$enginePath=Join-Path $fixture 'receiver/engine.py'
$original=[IO.File]::ReadAllBytes($enginePath)
[IO.File]::WriteAllText($enginePath,'tampered')
Reject 'modified member with valid manifest digest' {Read-VerifiedTree $fixture $sha $false} 'FILE_HASH'
[IO.File]::WriteAllBytes($enginePath,$original)
$extra=Join-Path $fixture 'user-photo.png';[IO.File]::WriteAllText($extra,'do not delete')
Reject 'unknown user file' {Read-VerifiedTree $fixture $sha $false} 'EXTRA_FILE'
Remove-Item -LiteralPath $extra
$extraDir=Join-Path $fixture 'user-folder';[IO.Directory]::CreateDirectory($extraDir)|Out-Null
Reject 'unknown empty directory' {Read-VerifiedTree $fixture $sha $false} 'EXTRA_DIRECTORY'
Remove-Item -LiteralPath $extraDir
$duplicate=[ordered]@{};foreach($name in $files.Keys){$duplicate[$name]=$files[$name]};$duplicate['receiver\engine.py']=$files['receiver/engine.py']
$duplicateSha=Write-FixtureManifest $duplicate
Reject 'duplicate normalized member' {Read-VerifiedTree $fixture $duplicateSha $false} 'MANIFEST_ENTRY'
$sha=Write-FixtureManifest $files
Reject 'missing receipt on installed tree' {Read-VerifiedTree $fixture $sha $true} 'RECEIPT_MISSING'
$changedFiles=@{};foreach($name in $tree.Files.Keys){$changedFiles[$name]=$tree.Files[$name]};$changedFiles['receiver/engine.py']='1'*64
$changed=[pscustomobject]@{Files=$changedFiles;Manifest=$tree.Manifest}
Reject 'receiver cannot change during desktop update' {Assert-FrozenComponents $tree $changed} 'FROZEN_HASH'
$missingFiles=@{};foreach($name in $tree.Files.Keys){if($name -ne 'python/python.exe'){$missingFiles[$name]=$tree.Files[$name]}}
Reject 'protected component cannot disappear' {Assert-FrozenComponents $tree ([pscustomobject]@{Files=$missingFiles;Manifest=$tree.Manifest})} 'FROZEN_FILES'
$cleanupOldFiles=@{};$cleanupNewFiles=@{}
foreach($name in $tree.Files.Keys){$cleanupOldFiles[$name]=$tree.Files[$name];$cleanupNewFiles[$name]=$tree.Files[$name]}
$cleanupOldFiles['receiver/receive_trial.py']='07261546dc3bbf20604699f98236b8d7f9c0335333b272e40ff624f898fdab6d'
$cleanupNewFiles['receiver/receive_trial.py']='5d83b392de7ab14346c6814c6a77c081f5da1a1ddae3803e7c449d02a630c936'
$cleanupOldFiles['receiver/manifest.json']='55f632ff5b86e6b42ed67253722062e10f5a9d4679678637e98d450e3206814a'
$cleanupNewFiles['receiver/manifest.json']='0b6121fbe150e4852ea7a4975fb2fed45481cb3573599682001f1629fe9035a9'
$cleanupOld=[pscustomobject]@{Files=$cleanupOldFiles;Manifest=$tree.Manifest}
$cleanupNew=[pscustomobject]@{Files=$cleanupNewFiles;Manifest=$tree.Manifest}
Assert-FrozenComponents $cleanupOld $cleanupNew
Expect 'exact paired example-name cleanup allowed' $true
Assert-FrozenComponents $cleanupNew $cleanupNew
Expect 'same sanitized release remains allowed' $true
$cleanupNewFiles['receiver/receive_trial.py']='2'*64
Reject 'example-name exception refuses wrong new file hash' {Assert-FrozenComponents $cleanupOld $cleanupNew} 'FROZEN_HASH'
$cleanupNewFiles['receiver/receive_trial.py']='5d83b392de7ab14346c6814c6a77c081f5da1a1ddae3803e7c449d02a630c936'
$cleanupNewFiles['receiver/manifest.json']='3'*64
Reject 'example-name exception refuses wrong new manifest hash' {Assert-FrozenComponents $cleanupOld $cleanupNew} 'FROZEN_HASH'
$cleanupNewFiles['receiver/manifest.json']=$cleanupOldFiles['receiver/manifest.json']
Reject 'example-name exception refuses an unpaired change' {Assert-FrozenComponents $cleanupOld $cleanupNew} 'FROZEN_HASH'
$cleanupNewFiles['receiver/manifest.json']='0b6121fbe150e4852ea7a4975fb2fed45481cb3573599682001f1629fe9035a9'
$cleanupOldFiles['receiver/receive_trial.py']='4'*64
Reject 'example-name exception refuses a different old file' {Assert-FrozenComponents $cleanupOld $cleanupNew} 'FROZEN_HASH'
$cleanupOldFiles['receiver/receive_trial.py']='07261546dc3bbf20604699f98236b8d7f9c0335333b272e40ff624f898fdab6d'
$cleanupNewFiles['receiver/engine.py']='5'*64
Reject 'paired cleanup does not allow other receiver changes' {Assert-FrozenComponents $cleanupOld $cleanupNew} 'FROZEN_HASH'

# Transaction rollback using actual fixture directories; OS registrations,
# elevation, ACL writes, process enumeration and named app mutexes are mocked.
# A startup write failure must restore the old app and its exact receipt.
$programFiles=Join-Path $testRoot 'program-files';[IO.Directory]::CreateDirectory($programFiles)|Out-Null
$installRoot=Join-Path $programFiles 'XiatiaoAirDrop'
$setupParent=Join-Path $testRoot 'common-files';[IO.Directory]::CreateDirectory($setupParent)|Out-Null
$setupRoot=Join-Path $setupParent 'AirdropForWindowsSetup'
$dataRoot=Join-Path $testRoot 'program-data';$radioRoot=Join-Path $dataRoot 'radio'
$ownerSid='S-1-5-21-1-2-3-1001';$taskName='FixtureOnly';$exe=Join-Path $installRoot 'XiatiaoAirDrop.exe';$shortcutPath=Join-Path $testRoot 'Fixture.lnk'
$hostArgument='--xiatiao-radio-host';$uninstallKey='HKCU:\DO-NOT-USE-FIXTURE';$runKey='HKCU:\DO-NOT-USE-FIXTURE';$runValue='fixture'
function Assert-Idle {}
function Protect-Tree([string]$Root) {}
function Assert-ProtectedItem([string]$Path) {}
function Acquire-Guards {return ,(New-Object 'System.Collections.Generic.List[System.Threading.Mutex]')}
function Get-Bindings([bool]$RequireExisting) {return [pscustomobject]@{Task=$null;Run=$false;Shortcut=$false}}
function Get-SetupState($Old) {return $null}
function Set-Startup([bool]$Enabled) {if($Enabled){throw 'FIXTURE_STARTUP_WRITE_FAILURE'}}
[IO.Directory]::CreateDirectory($installRoot)|Out-Null
foreach($item in Get-ChildItem -LiteralPath $fixture -Force){Copy-Item -LiteralPath $item.FullName -Destination $installRoot -Recurse}
$oldReceipt=[ordered]@{schema='xiatiao.install.v1';installedUtc='2026-01-01T00:00:00Z';ownerSid=$ownerSid;installRoot=$installRoot;task=$taskName;shortcut=$shortcutPath;manifestSha256=$sha}
[IO.File]::WriteAllText((Join-Path $installRoot 'installation.json'),($oldReceipt|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
$old=Read-VerifiedTree $installRoot $sha $true
$old|Add-Member -NotePropertyName Receipt -NotePropertyValue ([pscustomobject]$oldReceipt)
$old|Add-Member -NotePropertyName ReceiptSha -NotePropertyValue ((Get-FileHash -LiteralPath (Join-Path $installRoot 'installation.json') -Algorithm SHA256).Hash.ToLowerInvariant())
$installerFile=Join-Path $testRoot 'FixtureSetup.exe';[IO.File]::WriteAllText($installerFile,'fixture only, not executable')
$installer=[pscustomobject]@{Path=$installerFile;Sha=(Get-FileHash -LiteralPath $installerFile -Algorithm SHA256).Hash.ToLowerInvariant()}
$bindings=[pscustomobject]@{Task=$true;Run=$false;Shortcut=$true}
[IO.File]::WriteAllText($shortcutPath,'fixture shortcut')
Reject 'startup write failure reports failed update' {Install-Payload $tree $old $bindings $true $installer $null} 'INSTALL_FAILED'
$restored=Read-VerifiedTree $installRoot $sha $true
Expect 'rollback restores old files' ($restored.Files.Count -eq $old.Files.Count)
Expect 'rollback preserves exact old receipt' ((Get-FileHash -LiteralPath (Join-Path $installRoot 'installation.json') -Algorithm SHA256).Hash -ieq $old.ReceiptSha)
Expect 'rollback does not register an uninstaller' (-not (Test-Path -LiteralPath $setupRoot))

# Exercise the deletion gate itself: an extra file must prevent every delete.
$deleteToken=[guid]::NewGuid().ToString('N')
$deleteRoot=$installRoot+'.setup-backup-'+$deleteToken
[IO.Directory]::CreateDirectory($deleteRoot)|Out-Null
foreach($item in Get-ChildItem -LiteralPath $installRoot -Force){Copy-Item -LiteralPath $item.FullName -Destination $deleteRoot -Recurse}
[IO.File]::WriteAllText((Join-Path $deleteRoot 'private-note.txt'),'must survive')
$beforeDelete=@(Get-ChildItem -LiteralPath $deleteRoot -Recurse -File).Count
Reject 'cleanup refuses unknown file before deleting anything' {Remove-VerifiedTree $deleteRoot $old $old.ReceiptSha $deleteToken} 'EXTRA_FILE'
Expect 'unknown-content cleanup preserves complete old tree' (@(Get-ChildItem -LiteralPath $deleteRoot -Recurse -File).Count -eq $beforeDelete)
Reject 'cleanup refuses a nontransaction root' {Remove-VerifiedTree $installRoot $old $old.ReceiptSha $deleteToken} 'TRANSACTION_PATH'

# Successful staging and uninstall use only fixture registrations. The real
# installer functions still execute their copy/hash/swap/deletion paths.
$uninstallKey=Join-Path $testRoot 'fixture-arp.json'
function Register-Helper([ref]$Created){$Created.Value=$true}
function Write-Shortcut {[IO.File]::WriteAllText($shortcutPath,'fixture shortcut')}
function Set-Startup([bool]$Enabled) {}
function Write-Arp([string]$InstallerSha){[IO.File]::WriteAllText($uninstallKey,$InstallerSha)}
function Assert-Arp([string]$InstallerSha){if(-not (Test-Path -LiteralPath $uninstallKey) -or [IO.File]::ReadAllText($uninstallKey) -cne $InstallerSha){throw 'fixture ARP differs'}}
function Unregister-ScheduledTask {param($TaskName,$TaskPath,[switch]$Confirm)}
$installed=Install-Payload $tree $old $bindings $true $installer $null
Expect 'successful transaction returns updated' ($installed.status -eq 'updated')
Expect 'successful transaction cleans its verified old backup' ($null -eq $installed.backup)
$newInstalled=Read-VerifiedTree $installRoot $sha $true
$newReceipt=Get-Content -LiteralPath (Join-Path $installRoot 'installation.json') -Raw|ConvertFrom-Json
$newInstalled|Add-Member -NotePropertyName Receipt -NotePropertyValue $newReceipt
$newInstalled|Add-Member -NotePropertyName ReceiptSha -NotePropertyValue ((Get-FileHash -LiteralPath (Join-Path $installRoot 'installation.json') -Algorithm SHA256).Hash.ToLowerInvariant())
$setupState=Read-SetupTree $setupRoot $sha $installer.Sha
$userData=Join-Path $testRoot 'user-received-photo.jpg';[IO.File]::WriteAllText($userData,'user photo retained')
$uninstalled=Uninstall-Product $newInstalled ([pscustomobject]@{Task=$true;Run=$true;Shortcut=$true}) $setupState
Expect 'uninstall removes only verified product tree' ($uninstalled.status -eq 'uninstalled' -and -not (Test-Path -LiteralPath $installRoot))
Expect 'uninstall preserves user file and ProgramData' ((Test-Path -LiteralPath $userData) -and (Test-Path -LiteralPath $dataRoot))
Expect 'uninstall removes verified uninstaller fixture and registration' (-not (Test-Path -LiteralPath $setupRoot) -and -not (Test-Path -LiteralPath $uninstallKey))
function Register-Helper([ref]$Created){throw 'FIXTURE_REGISTRATION_FAILURE'}
Reject 'fresh helper registration failure reports failed install' {Install-Payload $tree $null ([pscustomobject]@{Task=$null;Run=$false;Shortcut=$false}) $true $installer $null} 'INSTALL_FAILED'
Expect 'failed fresh install leaves no executable product root' (-not (Test-Path -LiteralPath $installRoot))
$script:resultPathApproved=$false;$ResultPath=Join-Path $testRoot 'rejected-result.json'
Write-Result ([ordered]@{status='failed';error='rejected path'})|Out-Null
Expect 'rejected result path is not written in error reporting' (-not (Test-Path -LiteralPath $ResultPath))
[ordered]@{status='passed';checks=$passed.Count;names=@($passed.ToArray());fixtureRoot=$testRoot;realInstallationTouched=$false;devicesTouched=$false;processesStopped=$false;boundary='implementation fixture checks, not real installation acceptance'}|ConvertTo-Json -Depth 5
