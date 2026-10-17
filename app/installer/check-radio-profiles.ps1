# SPDX-License-Identifier: MIT
# Imports only pure functions; does not run installer entrypoints or hardware checks.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$DriverFile,[string]$BackendPath)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if([string]::IsNullOrWhiteSpace($BackendPath)){$BackendPath=Join-Path $PSScriptRoot 'setup-backend.ps1'}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($BackendPath,[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors|Out-String)}
$functions=$ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst]},$false)
. ([scriptblock]::Create(($functions|ForEach-Object{$_.Extent.Text}) -join "`n"))
$checks=0
function Expect([bool]$Pass,[string]$Name){$script:checks++;if(-not $Pass){throw "CHECK_FAILED: $Name"}}
function Reject([scriptblock]$Action,[string]$Code){$message=$null;try{& $Action|Out-Null}catch{$message=$_.Exception.Message};Expect ($message -and $message.StartsWith($Code+':')) $Code}
$mt=[pscustomobject]@{sourceRuntime='mt7925-transfer-runtime-r39'}
$rtl=[pscustomobject]@{sourceRuntime='rtl8852ce-winawdl-v0.3.0';radioBackend='rtl8852ce'}
Expect ((Resolve-RadioBackend $mt) -ceq 'mt7925') 'legacy MT package'
Expect ((Resolve-RadioBackend $rtl) -ceq 'rtl8852ce') 'RTL package'
foreach($id in @('','RTL8852CE','rtl8852be','../radio')){
    Reject {Resolve-RadioBackend ([pscustomobject]@{sourceRuntime=$rtl.sourceRuntime;radioBackend=$id})} 'RADIO_BACKEND'
}
Reject {Resolve-RadioBackend ([pscustomobject]@{sourceRuntime=$mt.sourceRuntime;radioBackend='rtl8852ce'})} 'RADIO_SOURCE'
Reject {Resolve-RadioBackend ([pscustomobject]@{sourceRuntime=$rtl.sourceRuntime})} 'RADIO_SOURCE'
$rp=Get-RadioPins 'rtl8852ce'
Expect ($rp.Count -eq 3 -and $rp.ContainsKey('runtime/winawdl_radio.exe') -and -not $rp.ContainsKey('runtime/winawdl_mt7925.exe')) 'RTL native pair'
Reject {Get-RadioPins 'unknown'} 'RADIO_BACKEND'
$fields=@{instance='PCI\VEN_10EC&DEV_C852&SUBSYS_12345678&REV_01\6&ABC&0&0020';status='OK';problem=0;service='WUDFRd';classGuid='{42c93386-3172-4834-9a73-d5dbe38443f8}';hardwareIds=@('PCI\VEN_10EC&DEV_C852')}
Assert-RealtekIdentity ([pscustomobject]$fields);Expect $true 'RTL identity'
$full=$fields.Clone();$full.hardwareIds=@('PCI\VEN_10EC&DEV_C852&SUBSYS_12345678&REV_01','PCI\VEN_10EC&DEV_C852&CC_028000')
Assert-RealtekIdentity ([pscustomobject]$full);Expect $true 'Windows full hardware IDs without generic compatible ID'
foreach($entry in @{status='ERROR';problem=10;service='rtwlane';classGuid=[guid]::Empty;hardwareIds=@('PCI\VEN_10EC&DEV_B852')}.GetEnumerator()){
    $copy=$fields.Clone();$copy[$entry.Key]=$entry.Value
    Reject {Assert-RealtekIdentity ([pscustomobject]$copy)} 'DEVICE_DRIVER'
}
foreach($id in @(($fields.instance.Replace('C852','B852')),($fields.instance+"'"),($fields.instance+'\extra'))){
    $copy=$fields.Clone();$copy.instance=$id;Reject {Assert-RealtekIdentity ([pscustomobject]$copy)} 'DEVICE_ID'
}
$original=[IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $DriverFile).Path)
$expected='ed0bc40efe1fa5d069ec9c42ab5f8181de9981b986aed710c92ebb5d615c23d7'
Expect ((Get-RealtekDriverImageHash $original) -ceq $expected) 'actual upstream unsigned driver'
$signed=New-Object byte[] ($original.Length+16);[Array]::Copy($original,$signed,$original.Length)
[BitConverter]::GetBytes([uint32]123456).CopyTo($signed,352)
[BitConverter]::GetBytes([uint32]110592).CopyTo($signed,432)
[BitConverter]::GetBytes([uint32]16).CopyTo($signed,436)
Expect ((Get-RealtekDriverImageHash $signed) -ceq $expected) 'certificate appended, executable unchanged'
$tampered=[byte[]]$signed.Clone();$tampered[10000]=$tampered[10000] -bxor 1
Expect ((Get-RealtekDriverImageHash $tampered) -cne $expected) 'modified executable rejected by pin'
foreach($entry in @(@(60,265),@(264,0),@(432,110591),@(436,15),@(436,[uint32]::MaxValue))){
    $bad=[byte[]]$signed.Clone();[BitConverter]::GetBytes([uint32]$entry[1]).CopyTo($bad,[int]$entry[0])
    Reject {Get-RealtekDriverImageHash $bad} 'DRIVER_IMAGE'
}
Reject {Get-RealtekDriverImageHash (New-Object byte[] 100)} 'DRIVER_IMAGE'
$old=[pscustomobject]@{Files=@{'runtime/awdld.exe'='old'};Manifest=$mt}
$new=[pscustomobject]@{Files=@{'runtime/awdld.exe'='new'};Manifest=$rtl}
Reject {Assert-FrozenComponents $old $new} 'FROZEN_HASH'
[ordered]@{status='passed';checks=$checks;realInstallationTouched=$false;devicesTouched=$false;driverLoaded=$false}|ConvertTo-Json
