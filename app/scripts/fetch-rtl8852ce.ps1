# SPDX-License-Identifier: MIT
# Download and verify only. Never runs upstream installers or radio executables.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDir)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=[IO.Path]::GetFullPath($OutputDir)
if(Test-Path -LiteralPath $root){throw 'Use a new output directory; existing downloads are not overwritten.'}
[IO.Directory]::CreateDirectory($root)|Out-Null
$sources=Join-Path $root 'sources';[IO.Directory]::CreateDirectory($sources)|Out-Null
$items=@(
    @('winawdl-0.3.0-windows-x64.zip','https://github.com/raph559/winawdl/releases/download/v0.3.0/winawdl-0.3.0-windows-x64.zip','fef10e3878a59593ab1ed8f5b29b392d673fc03db49f7c74e9f00b3025dfc734',$root),
    @('winawdl-6f91199e864609f8524ace832fe4af3d94f431f4.zip','https://codeload.github.com/raph559/winawdl/zip/6f91199e864609f8524ace832fe4af3d94f431f4','2b8e3f2bc712ea23bee9f0860e4fdde60c15225d3ce22bf24167beba0a4a2216',$sources),
    @('devourer-8d936de2a3805ce126e5236349afe40e645aaf9b.zip','https://codeload.github.com/raph559/devourer/zip/8d936de2a3805ce126e5236349afe40e645aaf9b','064fd294b9bd3db3f16bef01bc5ff6d20a3d1aac74a17680e974bc5ff81bf4ea',$sources),
    @('owl-da255a70f221784c836d943dd3f243bc798f223b.zip','https://codeload.github.com/seemoo-lab/owl/zip/da255a70f221784c836d943dd3f243bc798f223b','0f57557f28e805b2e279746d3cdd5c9528d436638cfbd6efabf161d215434dac',$sources),
    @('radiotap-94984dd829a605a2d7af55241145c159607f3b30.zip','https://codeload.github.com/radiotap/radiotap-library/zip/94984dd829a605a2d7af55241145c159607f3b30','c7f94f729f2830d40856540f9752540a24f850d6ad910bbb2836c95dae78fcac',$sources)
)
foreach($item in $items){
    $path=Join-Path $item[3] $item[0]
    Invoke-WebRequest -Uri $item[1] -OutFile $path
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $item[2]){throw "Checksum mismatch: $($item[0]); nothing was executed."}
}
Expand-Archive -LiteralPath (Join-Path $root $items[0][0]) -DestinationPath (Join-Path $root 'release')
[ordered]@{status='downloaded-and-verified';runtime=(Join-Path $root 'release/winawdl-0.3.0');sources=$sources;executed=$false}|ConvertTo-Json
