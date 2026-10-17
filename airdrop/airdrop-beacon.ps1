# Sends the Bluetooth advertisement an Apple device sends before an AirDrop,
# so that devices nearby turn AWDL on and announce themselves. It carries no
# identity: only receivers visible to everyone react. Runs for a bounded time;
# airdrop\sender.py starts and stops it. -DryRun builds it and sends nothing.
# Windows PowerShell 5.1 only: PowerShell 7 cannot load the Bluetooth API so.
param(
  [ValidateRange(1, 2147483647)][int]$ParentId,
  [ValidateRange(1, 600)][int]$LifetimeSeconds = 600,
  [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$AppleCompanyId = 0x004C
$StartWaitSeconds = 5
$publisher = $null
$parentStarted = if ($ParentId) { (Get-Process -Id $ParentId -ErrorAction Stop).StartTime } else { $null }

function Say($line) {
  [Console]::Out.WriteLine($line)
  [Console]::Out.Flush()
}

try {
  $namespace = 'Windows.Devices.Bluetooth.Advertisement'
  [void][Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementPublisher, Windows.Devices.Bluetooth.Advertisement, ContentType = WindowsRuntime]
  [void][Windows.Devices.Bluetooth.Advertisement.BluetoothLEManufacturerData, Windows.Devices.Bluetooth.Advertisement, ContentType = WindowsRuntime]
  Add-Type -AssemblyName System.Runtime.WindowsRuntime

  # PowerShell cannot call into the list an advertisement keeps its sections
  # in, so that one call is compiled.
  $metadata = "$env:windir\System32\WinMetadata"
  $compiler = New-Object System.CodeDom.Compiler.CompilerParameters
  $compiler.GenerateInMemory = $true
  $compiler.ReferencedAssemblies.AddRange([string[]]@(
    [Reflection.Assembly]::Load('System.Runtime, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b03f5f7f11d50a3a').Location,
    "$metadata\Windows.Devices.winmd", "$metadata\Windows.Foundation.winmd"))
  Add-Type -CompilerParameters $compiler -TypeDefinition @'
using Windows.Devices.Bluetooth.Advertisement;
public static class AirDropBeacon {
  public static void Add(BluetoothLEAdvertisementPublisher publisher, BluetoothLEManufacturerData data) {
    publisher.Advertisement.ManufacturerData.Add(data);
  }
}
'@

  # Type 5 (AirDrop), 18 bytes: eight zeros, version 1, four two-byte hashes
  # of the sender's contact details, a zero. The hashes are random here.
  $hashes = New-Object byte[] 8
  (New-Object Random).NextBytes($hashes)
  [byte[]]$payload = @(0x05, 0x12) + (,0 * 8) + @(0x01) + $hashes + @(0x00)

  $data = New-Object "$namespace.BluetoothLEManufacturerData"
  $data.CompanyId = $AppleCompanyId
  $data.Data = [Runtime.InteropServices.WindowsRuntime.WindowsRuntimeBufferExtensions]::AsBuffer($payload)
  $publisher = New-Object "$namespace.BluetoothLEAdvertisementPublisher"
  [AirDropBeacon]::Add($publisher, $data)
  if ($DryRun) { Say "would advertise $($payload.Length) bytes"; return }
  $publisher.Start()

  $deadline = (Get-Date).AddSeconds($StartWaitSeconds)
  while ("$($publisher.Status)" -notin 'Started', 'Aborted' -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 100
  }
  if ("$($publisher.Status)" -ne 'Started') {
    Say "Bluetooth did not start advertising, status $($publisher.Status)"
    return
  }
  Say 'advertising'
  $expires = [DateTime]::UtcNow.AddSeconds($LifetimeSeconds)
  while ([DateTime]::UtcNow -lt $expires -and "$($publisher.Status)" -eq 'Started') {
    if ($ParentId) {
      $owner = Get-Process -Id $ParentId -ErrorAction SilentlyContinue
      if (-not $owner -or $owner.StartTime -ne $parentStarted) { break }
    }
    Start-Sleep -Milliseconds 500
  }
} catch {
  Say "Bluetooth advertising failed: $($_.Exception.Message)"
} finally {
  if ($publisher) { $publisher.Stop() }
}
