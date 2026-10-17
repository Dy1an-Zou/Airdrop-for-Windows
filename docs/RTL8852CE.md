# RTL8852CE community test candidate / 社区实测候选

This preview adds a **Realtek RTL8852CE PCIe** backend to Airdrop for Windows 0.2.4.
It has not been tested on an RTL8852CE by this project. The existing MT7925 0.2.3
release remains available for MT7925 users. RTL8852AE, RTL8852BE and USB Realtek
adapters are not included.

本预览版增加 **Realtek RTL8852CE PCIe** 的接入代码和独立测试包，硬件标识为
`PCI\VEN_10EC&DEV_C852`。**本项目没有这张卡，不能把离线检查或上游的实测结果
当作本软件的实机通过。** 需要社区测试者验证发现、接收、发送和关闭后再开启。

## What the package changes / 接入内容

- A package chooses one backend through its protected `runtime-manifest.json`.
  Missing `radioBackend` retains the historical MT7925 behavior. An unknown or
  mismatched backend is refused before starting the radio.
- The RTL package uses the unmodified, matching `winawdl_radio.exe` and `awdld.exe`
  from [raph559/winawdl v0.3.0](https://github.com/raph559/winawdl/releases/tag/v0.3.0).
  It uses channel 149, 80 MHz, as in that upstream configuration. Regional channel
  availability and OEM variations remain unverified.
- The application keeps its existing sending, receiving, tray, history, and
  Chinese / English interface. This is a hardware backend candidate, not a UI fork.
- The installer and privileged helper check one present C852 adapter, its UMDF
  driver identity, and the exact pinned driver code. Local Authenticode signing
  is allowed to change only the PE checksum, certificate-directory fields and
  appended certificate. Modified executable content is rejected.
- RTL stop uses upstream's normal shutdown, a zero exit code, an initialized
  radio session, unchanged driver identity, and interface / firewall cleanup.
  It does **not** expose the MT7925 STOP IOCTL or an independent DMA-state readback.
  A crash or forced stop is a fault; automatic restart is refused in that session.

## Before a hardware test / 实机前提

1. A **dedicated RTL8852CE** with working antennas, and another internet connection.
   Its ordinary Windows Wi-Fi function is unavailable while bound to the experimental
   driver. Do not install the RTL candidate over an existing MT7925 installation;
   the installer rejects changing native backends during an update.
2. Windows 11 x64 build 26100 or newer, .NET Desktop Runtime 10 x64, and Microsoft
   Visual C++ 2015–2022 Redistributable x64.
3. The upstream v0.3.0 `winawdl_regproxy` experimental driver must already be prepared,
   locally signed, and bound to that adapter. Follow the instructions in the
   **pinned upstream release**, not an arbitrary newer driver. This candidate
   installer does not install drivers, change signing policy, or reboot Windows.
4. The account running the radio helper needs **Lock pages in memory**
   (`SeLockMemoryPrivilege`). After assigning it in Local Security Policy →
   User Rights Assignment, sign out and sign back in. The installer checks it;
   it does not grant privileges itself.
5. The Apple device needs Wi-Fi and Bluetooth enabled. For initial testing use
   AirDrop **Everyone for 10 Minutes**. Contacts Only and remembered-code pairing
   are not implemented here.

这是面向已经准备好实验驱动的测试者的包，不是原厂驱动下直接双击即用的通用安装包。
驱动、DMA、睡眠恢复和地区信道都还需要实机验证；先使用普通测试图片，保留原厂驱动的恢复方式。

## Suggested acceptance / 建议反馈

Record Windows version, adapter hardware ID including SUBSYS, driver version,
Apple model / OS, and the first failing stage. Test in this order:

1. Install the matching candidate and turn AirDrop on. Record whether initialization
   reaches the enabled state, and whether an Apple device discovers the PC.
2. Send one ordinary image Apple → Windows, accept it, then verify that it opens.
3. Send one ordinary image Windows → Apple and verify receipt.
4. Turn the radio off, check the recorded stop result, then turn it on and repeat
   discovery. Check normal exit before testing sleep / wake separately.

Local helper logs are in `%ProgramData%\XiatiaoAirDrop\radio`. The `stop.json`
records the backend and explicitly marks `hardwareQuiescenceReadback: false` for
RTL. Logs can contain hardware and network identifiers; remove those before
posting an issue. Do not upload private photos, certificates, keys, or full user data.

## Reproduce the package / 复现

Use `app/scripts/fetch-rtl8852ce.ps1 -OutputDir <new-directory>` to download the
pinned binary release and corresponding source archives. It verifies SHA-256
and does not run the downloaded code. Build the desktop app as in [BUILD.md](BUILD.md),
then package it using:

```powershell
.\app\scripts\package-xiatiao.ps1 `
  -Backend rtl8852ce `
  -PublishDir .\build\app\XiatiaoAirDrop `
  -PythonDir <prepared-portable-python> `
  -RuntimeDir <download-directory> `
  -RealtekPackageDir <download-directory>\release\winawdl-0.3.0 `
  -RealtekSourceDir <download-directory>\sources `
  -OutputDir <new-payload-directory>
```

Use the existing installer build procedure with this 0.2.4 payload. Name it
`Airdrop-for-Windows-0.2.4-RTL8852CE-Preview-Setup-x64.exe` so it cannot be mistaken
for the MT7925 release. The historical `install-xiatiao.ps1` rejects RTL packages.

Offline checks after building:

```powershell
dotnet .\build\app\XiatiaoAirDrop\XiatiaoAirDrop.dll --xiatiao-host-check
dotnet .\build\app\XiatiaoAirDrop\XiatiaoAirDrop.dll --xiatiao-backend-check <upstream-driver.dll>
pwsh -File .\app\installer\check-backend.ps1
pwsh -File .\app\installer\check-radio-profiles.ps1 -DriverFile <upstream-driver.dll>
```

These commands neither load the driver nor exercise RF transmission. See
[RTL8852CE-VALIDATION.md](RTL8852CE-VALIDATION.md) for this candidate's actual results.

## Provenance and licenses / 来源与许可

- `winawdl` v0.3.0 source: `6f91199e864609f8524ace832fe4af3d94f431f4`.
- `devourer`: `8d936de2a3805ce126e5236349afe40e645aaf9b`.
- `owl`: `da255a70f221784c836d943dd3f243bc798f223b`.
- OWL's `radiotap` submodule: `94984dd829a605a2d7af55241145c159607f3b30`.
  Unpack the source archives into `upstream/devourer`, `upstream/owl`, and
  `upstream/owl/radiotap` inside the winawdl source tree before building.
- Exact release ZIP SHA-256:
  `fef10e3878a59593ab1ed8f5b29b392d673fc03db49f7c74e9f00b3025dfc734`.
- Native code and license notices are retained from upstream. The package ships
  the exact source archives under `source/rtl8852ce-upstream` and the supplied
  notices under `licenses/rtl8852ce-upstream`, including the GPL-covered components.
  The desktop integration and packaging additions remain under this project's MIT
  license; that does not relicense the native components.

Author: **Dy1an-Zou**. This is an independent project, not affiliated with Apple,
Realtek, or the upstream authors. AirDrop is an Apple trademark.
