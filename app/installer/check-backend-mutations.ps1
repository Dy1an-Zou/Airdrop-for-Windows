# SPDX-License-Identifier: MIT
# Four directed offline mutants. Never executes the backend entrypoint.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$source=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'setup-backend.ps1') -Raw
$root=Join-Path ([IO.Path]::GetFullPath($env:TEMP)) ('airdrop-setup-mutants-'+[guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root)|Out-Null
$mutations=@(
    @{name='manifest-digest';from='if ((Hash-Bytes $bytes) -ine $ManifestSha)';to='if ($false)'},
    @{name='unknown-file';from="elseif (`$relative -ne 'runtime-manifest.json' -and";to="elseif (`$false -and `$relative -ne 'runtime-manifest.json' -and"},
    @{name='frozen-content';from='if($Old.Files[$name] -ine $New.Files[$name])';to='if($false)'},
    @{name='rollback-restore';from='Move-Item -LiteralPath $backup -Destination $installRoot;Read-VerifiedTree';to='Read-VerifiedTree'}
)
$results=New-Object 'System.Collections.Generic.List[object]'
$powershell=Join-Path ([Environment]::GetFolderPath('Windows')) 'System32\WindowsPowerShell\v1.0\powershell.exe'
foreach($mutation in $mutations){
    $matches=([regex]::Matches($source,[regex]::Escape($mutation.from))).Count
    if($matches -ne 1){throw "Mutation target must occur once: $($mutation.name) ($matches)"}
    $path=Join-Path $root ($mutation.name+'.ps1')
    [IO.File]::WriteAllText($path,$source.Replace($mutation.from,$mutation.to),[Text.UTF8Encoding]::new($true))
    $previousAction=$ErrorActionPreference
    $ErrorActionPreference='Continue'
    try{$output=& $powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'check-backend.ps1') -BackendPath $path 2>&1;$exit=$LASTEXITCODE}
    finally{$ErrorActionPreference=$previousAction}
    $log=Join-Path $root ($mutation.name+'.log');[IO.File]::WriteAllText($log,($output|Out-String),[Text.UTF8Encoding]::new($false))
    $text=$output|Out-String
    $outcome=if($exit -ne 0 -and $text -match 'CHECK_FAILED:'){'caught'}elseif($exit -eq 0){'missed'}else{'unviable'}
    $results.Add([ordered]@{name=$mutation.name;outcome=$outcome;exitCode=$exit;log=$log})
}
[ordered]@{results=@($results.ToArray());mutantRoot=$root;boundary='directed implementation checks; no live install, device or process operations'}|ConvertTo-Json -Depth 5
if(@($results|Where-Object{$_.outcome -ne 'caught'}).Count){exit 1}
