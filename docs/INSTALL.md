# 安装与卸载 · Installation and removal

**Airdrop for Windows 0.2.3 · Dy1an-Zou · 实验预览版 / Experimental preview**

[返回中文介绍](../README.md) · [English overview](../README.en.md) · [English instructions](#english)

## 安装前需要什么

本安装包适用于已经配置好专用无线环境的电脑。它还不是任意 Windows 电脑安装后就能使用的通用 AirDrop 方案。

| 条件 | 当前要求 |
| --- | --- |
| 系统 | Windows x64，系统构建 26100 及以上。 |
| 桌面运行时 | .NET Desktop Runtime **10 x64**。安装器可打开微软下载页，不会替你自动下载安装。 |
| 网卡 | 唯一一块专用 MediaTek MT7925 PCIe，硬件 ID 必须精确匹配下方值。 |
| 驱动 | 已配置并正常运行的对应版本 `mt7925_bringup` 实验驱动，安装器会核对版本摘要。原厂驱动不满足这个条件。 |
| Windows 账户 | 当前账户属于管理员组；管理员确认继续使用同一账户。更新与卸载使用原安装账户。 |

```text
PCI\VEN_14C3&DEV_7925&SUBSYS_60001A3B&REV_00
```

0.2.3 会自动识别这块匹配网卡的本机实例，不需要填写作者电脑的设备路径。芯片型号相同但硬件 ID 不同，或装有多块匹配网卡，都不在当前自动配置范围内。这块专用网卡不同时承担普通 Wi-Fi 联网。

**安装器不安装或替换网卡驱动，不启用 / 关闭测试签名，也不改变其他系统安全设置。** 公开源码中的实验驱动仍供开发者自行构建、签名与配置；现有安装包要求匹配的实验驱动已经就绪。

## 安装或更新

1. 如果已有程序在运行，先从系统托盘菜单选择「退出」，等待软件与无线助手正常停止。只关闭主窗口会留在托盘，不算退出。
2. 打开 `Airdrop-for-Windows-0.2.3-Setup-x64.exe`。安装器初始语言跟随系统界面语言，也可点击右上角「中文 / English」按钮切换。
3. 初次安装时选择是否登录后隐藏启动。更新会保留原来的自启动设置。
4. 点击「安装 / 更新」，完成 Windows 管理员确认。安装器检查系统、运行时、专用网卡、驱动和包完整性后才部署。
5. 显示安装完成后，关闭安装器，从开始菜单打开 **Airdrop for Windows**。安装器不会自动启动软件，也不会自动重启电脑。

应用默认中文，通过主窗口语言选择器切换到 English 即可，语言偏好会保存。应用和配套运行文件安装到 `%ProgramFiles%\XiatiaoAirDrop`，开始菜单及窗口显示名称为 **Airdrop for Windows**。

更新目前只接受已核验的指定 0.2.2 安装，或清单完全相同的本次 0.2.3。其他版本、其他用户的安装或被修改的安装内容会停止处理，显示具体原因。

安装器目前未作 Authenticode 签名。

## 文件和设置会怎样

- 已收到的文件留在用户原先选择的文件夹。
- 设置、接收历史和运行时身份保留在 `%LOCALAPPDATA%\XiatiaoAirDrop`。
- 已有自启动偏好在更新时保留；之后仍可在软件里更改。
- 软件运行所需的受限无线助手、当前用户快捷方式及卸载入口由安装器注册。

安装包、源码与校验文件见 [GitHub Release v0.2.3](https://github.com/Dy1an-Zou/Airdrop-for-Windows/releases/tag/v0.2.3)。

## 卸载

先从托盘正常退出软件，然后在 Windows「设置 → 应用 → 已安装的应用」中找到 **Airdrop for Windows**，选择卸载，并完成管理员确认。

卸载移除应用程序和对应的启动 / 助手注册；**收到的文件、个人设置、接收记录、诊断数据和网卡驱动会保留**。它不会恢复原厂驱动或更改测试签名设置。

如果卸载器自身仍被占用，`%CommonProgramFiles%\AirdropForWindowsSetup` 中的 `Setup.exe` 可能暂时保留。安装结果会记录待清理状态和具体路径；关闭卸载窗口后，再按它提供的清理提示处理。不要为清理此文件而删除个人保存目录或其他公共目录。

## 安装中止时

| 提示 | 怎么处理 |
| --- | --- |
| 软件或无线助手仍在运行 | 从托盘退出并等待正常停止，再重试。安装器不会强制结束进程。 |
| 缺少 Desktop Runtime 10 x64 | 通过安装窗口中的微软下载入口安装对应的桌面运行时，然后重试。 |
| 没有唯一的匹配网卡，或驱动未就绪 | 先核对上面的硬件 ID 和实验驱动环境；反复运行安装器不会替你配置驱动。 |
| 版本、文件摘要或安装账户不匹配 | 保留当前文件，按具体提示核对安装来源及账户。 |

每次操作的结果与日志保存在 `%ProgramData%\AirdropForWindowsSetup\<作业编号>\result.json` 和 `backend.log`。更新或卸载不能安全清理的内容会保留，并在结果中说明。

## 验证范围

0.2.3 顶栏语言布局已再次编译通过（0 错误、0 警告），语言持久化、中英双向状态更新、用户文件名保留 3 项检查及无线助手 49 项离线检查通过。已查看中英普通、紧凑、发送和关于界面的离屏渲染，没有绑定错误；4 种展开状态下开关对齐差为 0 DIP，普通窗口设置区没有滚动条。

安装器候选编译通过（0 错误、0 警告），**内嵌包 199 个文件核验通过，安装后端 39 项隔离样例检查通过，4 个定向变异全部捕获**。真实只读环境预检在当前受限令牌的 `ADMIN_ACCOUNT` 检查处拒绝，未完成后续环境检查，无系统变更。

**真实安装 / 更新 / 卸载、新机安装与 0.2.3 手机传输均未执行；现有 0.2.2 安装保留。** 本次以实验预览版发布，构建和离线检查不等于安装实机验收。原生收发组件、固件、Python 和图标摘要保持一致，实际收发代码未变；离线示例与源码归档仅做隐私脱敏。

此前已确认的是：本机 0.2.0 与专用 MT7925 的组合完成过 iPhone → Windows 照片接收，以及 Windows → iPhone 单张图片发送。当前没有新电脑的安装验收，也没有所有 Apple 机型和系统版本的兼容性结论。当前不支持「仅限联系人」、验证码配对或 30 天临时联系人。

## English

### Prerequisites

This installer is intended for a PC with the dedicated wireless environment already configured. It is not a universal AirDrop installer for any Windows PC.

- **Windows x64, build 26100 or later.**
- **.NET Desktop Runtime 10 x64.** Setup links to Microsoft's download page; it does not download or install the runtime automatically.
- Exactly one dedicated **MediaTek MT7925 PCIe** matching `PCI\VEN_14C3&DEV_7925&SUBSYS_60001A3B&REV_00`.
- The matching **`mt7925_bringup` experimental driver**, already installed and working. Setup verifies its hash; the stock Wi-Fi driver does not meet this requirement.
- The current Windows account must belong to Administrators. Elevation must use the same account; updates and removal must use the original installing account.

Version 0.2.3 detects the matching adapter's local instance automatically. Different hardware IDs or multiple matching adapters are not supported by this setup path. The dedicated adapter does not simultaneously provide a regular Wi-Fi internet connection.

**Setup does not install or replace the adapter driver, change test-signing settings, or change other Windows security settings.** The public experimental driver source remains a developer component that requires separate building, signing, and configuration.

### Install or update

1. If the app is running, choose **Quit** from its system tray menu and wait for the app and wireless helper to stop normally. Closing the main window only hides it.
2. Open `Airdrop-for-Windows-0.2.3-Setup-x64.exe`. Setup initially follows the system UI language. Click the **中文 / English** button in the top-right corner to switch.
3. On first installation, choose whether to start hidden at sign-in. Updates preserve the existing startup preference.
4. Select **Install / Update** and approve the Windows administrator prompt. Setup checks the prerequisites and package integrity before deployment.
5. Once installation finishes, close Setup and open **Airdrop for Windows** from the Start menu. Setup does not launch the app automatically or restart Windows.

The app defaults to Chinese. Use the main window's language selector to choose **English**; the app saves your preference. Program files are installed in `%ProgramFiles%\XiatiaoAirDrop`; the displayed product name is **Airdrop for Windows**.

Updates currently accept only the specifically verified 0.2.2 installation or this exact 0.2.3 manifest. Other versions, another user's installation, or modified installation files cause Setup to stop and explain why. The installer is currently unsigned.

Settings, received-file history, and runtime identity remain in `%LOCALAPPDATA%\XiatiaoAirDrop`. Received files stay in the save folder chosen by the user. Setup registers the restricted wireless helper, the user's shortcut and startup preference, and the uninstall entry.

The local review installer filename does not imply a published GitHub Release or a public download.

### Uninstall

Quit the app from the system tray. In Windows **Settings → Apps → Installed apps**, select **Airdrop for Windows**, choose **Uninstall**, and approve the administrator prompt.

Removal deletes verified application files and associated startup / helper registrations. **Received files, preferences, history, diagnostic data, and the adapter driver are kept.** It does not restore the stock driver or change test-signing settings.

The running uninstaller may leave its own `Setup.exe` in `%CommonProgramFiles%\AirdropForWindowsSetup`. The result records whether cleanup is pending and the affected path. Close the uninstall window before following any cleanup instructions. Do not remove personal save folders or unrelated shared directories.

If Setup stops, resolve the specific message before retrying: close the app normally, install the required Desktop Runtime, check the exact adapter and configured driver, or check the installation's version and account. Results and logs are retained at `%ProgramData%\AirdropForWindowsSetup\<job-id>\result.json` and `backend.log`.

### Verification status

The final 0.2.3 header layout was rebuilt with 0 errors and 0 warnings. Three checks passed for language preference persistence, status updates in both language directions, and preservation of user file names, along with 49 offline wireless-helper checks. Actual off-screen renders of normal, compact, Send, and About windows were inspected in both languages, with no binding errors. Switch alignment differed by 0 DIP across four expanded-settings states; the settings area needs no scrollbar at normal window size.

The installer candidate compiled with 0 errors and 0 warnings. **Its 199-file embedded payload passed verification, the backend passed 39 isolated fixture checks, and all 4 targeted mutations were detected.** A real read-only environment preflight was rejected at the `ADMIN_ACCOUNT` check under the current restricted token. It did not complete the remaining environment checks or change the system.

**Actual install / update / uninstall operations, fresh-PC installation, and 0.2.3 phone transfers have not been performed; the existing 0.2.2 installation is retained.** This release is an experimental preview. Compilation and offline checks do not establish successful installation. The native transfer components, firmware, Python runtime, and icon retain the same hashes, and the active transfer code is unchanged. Only private identifiers in the offline example and source archive were sanitized.

Earlier real-device confirmation covers version 0.2.0 on the configured PC with its dedicated MT7925: an iPhone-to-Windows photo reception and a separate Windows-to-iPhone single-image transfer. Installation on a fresh PC and compatibility with all Apple models or OS versions have not been verified. **Contacts Only, verification-code pairing, and 30-day temporary contacts are not supported.**
