[简体中文](README.md) | **English**

<div align="center">

<img src="app/assets/xiatiao-airdrop.png" width="144" alt="Xiatiao holding a pixel-art AirDrop badge">

# Airdrop for Windows

**Send and receive files with AirDrop on Windows.**

By **Dy1an-Zou** · **0.2.4 RTL8852CE community test preview**

[Features](#features) · [Installer](#installer) · [How to use](#how-to-use) · [Hardware requirements](#hardware-requirements) · [Development and builds](#development-and-builds) · [Credits and licenses](#credits-and-licenses)

</div>

> [!IMPORTANT]
> **0.2.4 adds an RTL8852CE PCIe candidate that has not been tested on hardware by this project.**
> RTL candidate hardware ID: `PCI\VEN_10EC&DEV_C852`; the upstream experimental driver must be prepared separately.
> This project's existing bidirectional transfer evidence covers only the dedicated MT7925: `PCI\VEN_14C3&DEV_7925&SUBSYS_60001A3B&REV_00`.
> [0.2.4 downloads and bilingual release notes](https://github.com/Dy1an-Zou/Airdrop-for-Windows/releases/tag/v0.2.4) · [RTL8852CE test guide](docs/RTL8852CE.md) · [Validation record](docs/RTL8852CE-VALIDATION.md). The 0.2.3 historical results below still refer to MT7925.

Move photos from your phone to your PC, or send files from your PC to your iPhone. Once enabled, the app stays in the system tray. Open its window whenever you want to accept a transfer, manage received files, or send something.

## Features

| Feature | What it does |
| --- | --- |
| Receive files | Choose your PC in the iPhone share sheet, then accept the transfer in the Windows app. |
| Send files | Select local files and a nearby device. Transfer begins after the recipient accepts; you can follow its progress or cancel. The receiving device tested so far is an iPhone. |
| Device name and save location | Set the name nearby devices see and the folder where received files are saved. |
| System tray and startup | Start hidden at sign-in and keep running in the tray after closing the window. |
| File management | Drag a selection rectangle from empty space, use Ctrl / Shift for multiple selection or Ctrl+A to select all, and scroll automatically near the edges. Right-click to open, locate, copy, remove a record, or move a file to the Recycle Bin. |
| Connection status | See the actual startup steps as they run. Switches include animated transitions. |
| Chinese and English interface | Switch languages immediately and keep your preference. Status and help text update with it; your file names stay unchanged. |

Removing a record or clearing the history only changes the list; the original files remain. Moving files to the Recycle Bin requires confirmation. If the destination does not support the Recycle Bin, the app keeps the files instead of permanently deleting them.

## Interface preview

These are off-screen renders of the actual 0.2.3 WPF interface with sample files and device names. The header language selector and both language layouts have been checked. Real-device testing is described under “What has been verified” below.

**Main window**

![Main window preview: connection settings, incoming transfers, and received files](docs/images/main-en.png)

**Send window**

![Send window preview: selected files and nearby devices](docs/images/send-en.png)

## Installer

Choose the package for your adapter. Both releases provide a Windows x64 installer, source ZIP, and SHA-256 checksums.

| Release | Hardware and verification scope |
| --- | --- |
| **[0.2.4 RTL8852CE community test preview](https://github.com/Dy1an-Zou/Airdrop-for-Windows/releases/tag/v0.2.4)** | `PCI\VEN_10EC&DEV_C852`. Builds and offline checks passed; **no hardware testing yet**. Requires the pinned experimental driver to be configured first. [Prerequisites](docs/RTL8852CE.md) · [Bilingual release notes](docs/releases/v0.2.4.md). |
| **[0.2.3 MT7925 experimental preview](https://github.com/Dy1an-Zou/Airdrop-for-Windows/releases/tag/v0.2.3)** | Only the dedicated MT7925 configuration below. Historical bidirectional image transfers were tested with 0.2.0; hardware transfers were not repeated for 0.2.3. |

**The 0.2.4 RTL installer is not an upgrade for an MT7925 installation. The installer rejects updates that change the native backend.**

### MT7925 0.2.3 installation notes

The installer, `Airdrop-for-Windows-0.2.3-Setup-x64.exe`, has been built and its embedded payload verified. It requires the dedicated MT7925 configuration and experimental driver described below.

See the [installation and removal guide](docs/INSTALL.md#english) for prerequisites, update steps, retained data, and uninstall instructions.

The installer deploys the app and its runtime files only. **It does not install or replace the Wi-Fi driver, or change Windows test-signing settings.** The matching experimental driver and required environment must already be configured. The 0.2.3 candidate automatically identifies the single configured MT7925 that matches the exact supported hardware ID, rather than relying on the author's device instance. This does not broaden hardware support.

The new installer has not been validated on a fresh PC. Successful operation on the existing configured machine does not establish successful installation on another one.

## How to use

These steps assume the dedicated adapter, driver, and complete runtime environment are already configured.

### iPhone → Windows

1. Open Airdrop for Windows, enable the connection, and choose a device name and save location.
2. Enable Wi-Fi and Bluetooth on the iPhone, select a photo or file, and choose **Share → AirDrop**.
3. Select the PC, then choose **Accept (接收)** in the Windows app. The file appears in the received-files list when the transfer finishes.

### Windows → iPhone

1. Enable Wi-Fi and Bluetooth on the iPhone, set AirDrop to **Everyone for 10 Minutes**, and keep the device unlocked.
2. In the Windows app, choose **Send files (发送文件)**, add your files, and select the discovered iPhone.
3. Choose **Send (发送)** and accept the transfer on the iPhone. The window displays the transfer status.

If the device does not appear, keep its AirDrop screen open and search again. Closing the main window hides the app in the system tray. To stop it completely, choose **Exit (退出)** from the tray menu.

## What has been verified

**0.2.4 RTL8852CE:** compilation, offline checks, and installer-content verification are complete. The author does not own an RTL8852CE; device discovery, bidirectional transfers, actual installation, shutdown/restart, and sleep/wake have not been tested. [Full validation record](docs/RTL8852CE-VALIDATION.md). The following results are MT7925 history.

**Confirmed real-device results come from version 0.2.0 on the author's PC with the dedicated MT7925:**

- **iPhone → Windows:** a photo was received, saved, and displayed in the file list. Its hash matched the original.
- **Windows → iPhone:** one image was sent successfully on October 16, 2026. The user confirmed receipt, and the sender log recorded acceptance and completion.

Version 0.2.1 corrected the switch shape and mouse-triggered focus outline, added drag selection, and passed off-screen interaction and animation checks for those changes. Version 0.2.2 aligns both switches along the same right edge; checks covered the normal window, the scrollbar in a narrow window, and collapsed settings. The wireless driver and transfer engine are unchanged from 0.2.0. Phone transfers were not repeated for these interface updates. The public source copy has not been built separately or tested on another PC.

The final 0.2.3 header layout was rebuilt with 0 errors and 0 warnings. Three checks passed for language preference persistence, status updates when switching in both directions, and preservation of user file names; 49 offline wireless-helper checks also passed. Actual off-screen renders of normal, compact, Send, and About windows were inspected in both languages, with no binding errors. The two switches aligned with a 0 DIP difference across four expanded-settings states, and the settings area needs no scrollbar at the normal window size.

The installer candidate compiled with 0 errors and 0 warnings, and its 199-file embedded payload passed verification. The backend passed 39 isolated fixture checks, and all 4 targeted mutations were detected. A real read-only environment preflight stopped at the `ADMIN_ACCOUNT` check under the current restricted token. It did not complete the remaining environment checks or change the system.

**Actual installation, updating, removal, fresh-PC setup, and 0.2.3 phone transfers have not been performed; the existing 0.2.2 installation is retained.** Individual hashes of the native transfer components, firmware, Python runtime, and icon are unchanged, as is the active transfer code. Only private identifiers in the source archive and an offline diagnostic example were sanitized. These implementation checks are recorded separately from historical real-device results.

| Scope | Current status |
| --- | --- |
| Image transfers in both directions on the configured PC | Each direction was confirmed separately; the Windows-to-iPhone result covers one image. |
| All Apple models, iOS / macOS versions, and file types | Compatibility has not been verified. |
| Concurrent transfer sessions and prolonged continuous use | Not tested. |
| Contacts Only | Not supported. |
| Verification-code pairing and 30-day temporary contacts | Not supported. |
| Out-of-the-box use on other PCs or adapters | Not supported. |

Historical 0.2.0 implementation checks include 48 backend tests, 4 targeted fault-injection checks, a local HTTPS loopback transfer, and desktop state and file-operation checks. They are recorded separately from this round's 0.2.3 checks and the real-device transfers above. See the [build and verification guide](docs/BUILD.md) (Chinese) for details.

## Hardware requirements

**0.2.4 RTL8852CE:** a dedicated PCIe `10EC:C852` adapter, Windows 11 x64 build 26100+, and the pinned upstream experimental driver and required privileges. See the [RTL8852CE guide](docs/RTL8852CE.md) for all prerequisites. This release does not add support for RTL8852AE, RTL8852BE, Realtek USB, Intel, or Broadcom adapters.

**0.2.3 MT7925:**

- **Windows x64**; the current driver INF requires **build 26100 or later**.
- A **dedicated MediaTek MT7925 PCIe adapter** matching `PCI\VEN_14C3&DEV_7925&SUBSYS_60001A3B&REV_00`.
- The experimental driver, matching wireless backend and Wintun virtual interface, and a driver test-signing environment configured by the developer.

While running the experimental driver, this dedicated adapter does not provide a normal Wi-Fi internet connection. The verified configuration uses 2.4 GHz channel 6. Support has not been established for other hardware, channels, or system configurations.

The source repository does not contain signed drivers, signing certificates, private keys, firmware, or a Python runtime. Building the desktop window does not configure the wireless environment automatically. See [hardware and runtime requirements](docs/HARDWARE.md) (Chinese).

## Development and builds

On Windows x64 with .NET SDK 10, build the desktop component with:

```powershell
dotnet build .\app\XiatiaoAirDrop.csproj -c Release
```

A working installation also needs the protocol engine, wireless backend, firmware, and matching driver. See [BUILD.md](docs/BUILD.md) for build instructions, and [DEPENDENCIES.json](docs/DEPENDENCIES.json) and [PROVENANCE.json](docs/PROVENANCE.json) for component versions and origins.

| Directory | Contents |
| --- | --- |
| `app/` | Windows interface, system tray, wireless helper, packaging scripts, and UI assets. |
| `airdrop/` | Python device discovery, file reception, and sending engine. |
| `awdl/`, `radio/`, `driver/mt7925/` | AWDL protocol support, the userspace radio process, and the experimental MT7925 driver. |
| `upstream/owl/` | The OWL and Radiotap source used by the project. |
| `artwork/` | Editable, layered source for the approved icon. |
| `docs/` | Build, hardware, provenance, and dependency documentation. |

Settings, transfer history, and runtime identity files are stored in `%LOCALAPPDATA%\XiatiaoAirDrop` and are not part of the source repository. Received files are saved to the folder selected by the user.

## Credits and licenses

This project builds on the work of these projects and authors:

| Project | Contribution |
| --- | --- |
| [winawdl · Raphaël Richaud](https://github.com/raph559/winawdl) | Windows AirDrop foundations and sending code. |
| [OWL · SEEMOO / TU Darmstadt](https://github.com/seemoo-lab/owl) | AWDL protocol implementation. |
| [omdrop · Brent Kearney](https://github.com/brentkearney/omdrop-plugin) | AirDrop reception engine. |
| [mt76 · OpenWrt / MediaTek](https://github.com/openwrt/mt76) | Reference implementation for the MT7925 port. |
| [Uiverse / Galaxy](https://github.com/uiverse-io/galaxy) | UI components. Original materials, author credits, and licenses are retained in `app/design-sources/` and `app/licenses/`. |

Desktop integration and this project's modifications are by **Dy1an-Zou**. Components are separately licensed under MIT, GPL-2.0-only, GPL-3.0-or-later, BSD-3-Clause-Clear, ISC, and other applicable licenses. The desktop code's MIT license does not cover the entire repository. See [LICENSE](LICENSE) and the [third-party notices](app/licenses/THIRD-PARTY-NOTICES.md) for the license map and original author credits.

AirDrop, Apple, iPhone, and Mac are trademarks of Apple Inc. This is an independent third-party project, with no affiliation, sponsorship, or endorsement from Apple Inc. Artwork and trademarks are not covered by the code's MIT license. See [ARTWORK.md](app/design-sources/ARTWORK.md) for asset provenance.
