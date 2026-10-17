# 0.2.3 安装器开发说明

> 本分支安装器源码已更新为 **0.2.4**。RTL 专用候选的依赖、命令和版本边界见 [RTL8852CE 说明](../../docs/RTL8852CE.md)。下文保留 0.2.3 MT7925 的设计与历史验证记录，不能用来宣称 RTL 实机通过。

本目录是 **Airdrop for Windows 0.2.3** 的 Windows x64 单文件安装器源码，作者 **Dy1an-Zou**。它由 .NET Framework 4.8 Windows Forms 界面和 PowerShell 后端组成，使用 Windows 自带的 .NET Framework MSBuild / C# 编译器构建，不依赖 Inno Setup。

使用者的安装步骤见公开交付中的 `docs/INSTALL.md`。本说明面向准备完整发行包的开发者；源码构建不等于驱动配置或新电脑安装验收。

## 组成与职责

| 文件 | 用途 |
| --- | --- |
| `Setup.cs` | 中英安装界面、用户点击后的 UAC 提升、内嵌包校验与解压、结果显示。 |
| `setup-backend.ps1` | 安装前检查、应用文件部署、受限无线助手注册、更新与卸载事务。 |
| `Setup.csproj` | .NET Framework 编译入口，将已核验的 payload、清单摘要和后端脚本嵌入 EXE。 |
| `setup.manifest` | 安装器进程清单；打开窗口本身不执行安装。 |

输出名为 `Airdrop-for-Windows-0.2.3-Setup-x64.exe`。安装器语言可在窗口中切换；仅在点击安装 / 更新或卸载后请求管理员权限。当前 Windows 账户须属于管理员组，提升时保持同一账户；不支持用另一个账户的凭据代办。

## 准备已核验的 payload

先使用应用打包入口生成完整的 **0.2.3** 运行目录，再准备单独的 payload 目录，包含：

```text
payload/
  payload.zip
  manifest.sha256
```

- `payload.zip` 的根部直接放 `runtime-manifest.json` 和它登记的全部文件，不额外套一层目录。
- ZIP 只包含文件条目；清单以外的文件、目录条目、重复路径、越界路径均会被拒绝。
- `manifest.sha256` 保存 ZIP 内 `runtime-manifest.json` 原始字节的 SHA-256 十六进制文本，使用 ASCII 或无 BOM 的 UTF-8。
- 内嵌清单和每个文件都要核验；单文件上限 128 MiB，解压总量上限 512 MiB。
- 安装后端要求桌面程序集版本 `0.2.3.0`，并固定核验无线组件、固件和实验驱动摘要。重新构建这些组件后不能直接沿用旧摘要，需要先验证新组合。

完整 payload 与公开源码压缩包用途不同。源码不附带作者的驱动签名、证书、私钥或已配置的无线环境；不能把源码 ZIP 作为运行 payload。

## 构建

从包含 `app/` 的源码根目录运行，替换为自己的已核验 payload 和输出目录：

```powershell
$payloadDir = 'C:\Build\Airdrop-for-Windows\payload'
$setupOutputDir = 'C:\Build\Airdrop-for-Windows\installer\'
& "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\MSBuild.exe" `
  .\app\installer\Setup.csproj `
  "/p:PayloadDir=$payloadDir" "/p:OutDir=$setupOutputDir"
```

构建安装器需要本机 .NET Framework 4.8 工具与引用程序集；运行桌面应用另需 **.NET Desktop Runtime 10 x64**。这两者不是同一个运行时。构建不运行安装后端、不启动应用、不修改网卡。

## 接口与验证边界

| 入口 | 行为 |
| --- | --- |
| `Setup.exe --verify <新结果文件.json>` | 只检查内嵌 ZIP、清单摘要及文件完整性，写出校验结果；不安装、不请求 UAC。 |
| `Setup.exe --render zh-CN <图片.png>` / `--render en <图片.png>` | 输出安装器界面预览；不执行安装。 |
| `Setup.exe` | 打开安装 / 更新窗口，等待用户点击。 |
| `Setup.exe --uninstall` | 打开卸载窗口，等待用户点击。 |

后端的 `verify-payload` 只核验解压目录；`preflight` 和 `uninstall-preflight` 读取环境及当前安装状态。`install`、`uninstall` 才执行相应变更。`--worker` 是界面内部的提升入口，不是面向用户的通用脚本执行接口。

不要把清单校验通过或界面渲染成功写成安装 / 卸载实机通过。0.2.3 安装器的构建、内嵌包验证、环境预检和真实安装 / 卸载结果需分别记录；本说明编写时，本轮最终验证尚待补充。

## 安装和更新约束

安装前检查：Windows x64 build 26100 及以上、.NET Desktop Runtime 10 x64、唯一一块精确匹配 `PCI\VEN_14C3&DEV_7925&SUBSYS_60001A3B&REV_00` 的专用 MT7925，以及已就绪且摘要匹配的 `mt7925_bringup` 实验驱动。设备实例由当前机器自动识别；不会改选其他网卡。

当前固定实验驱动 SHA-256：

```text
938148d69b14c911c18c4c7a9ef00703ad5625e46d876143b8f887df6762aafb
```

安装器部署到 `%ProgramFiles%\XiatiaoAirDrop`，注册当前用户的受限无线助手、开始菜单快捷方式和卸载入口。初次安装可选择登录后隐藏启动，更新保留原自启动设置。成功后不会自动启动应用，也不会自动重启。

更新只接受后端固定清单对应的既有 0.2.2，或清单完全一致的本次 0.2.3。对来源不明、其他版本或已有内容被修改的安装，不会用强制覆盖代替验证。更新前必须从托盘正常退出，等待无线助手停止；安装器不强制结束进程。

驱动安装 / 替换、Windows 测试签名与其他安全设置均不在此安装器范围内。驱动开发及配置仍由开发者单独完成。应用设置、接收历史、运行时身份及已收到的文件保持在原位置。

## 卸载、结果和清理

卸载删除经过核验的应用文件、无线助手任务、自启动与快捷方式及卸载注册，保留用户数据、收到的文件、诊断数据和网卡驱动。

从已安装的卸载入口运行时，`%CommonProgramFiles%\AirdropForWindowsSetup\Setup.exe` 可能因当前进程占用而暂时保留。后端通过 `setupCleanupPending`、`setupFolder` 和 `setupCleanupWarning` 明确记录此状态；不要把该残留解释为用户文件被删除或驱动被恢复。更新清理异常也会记录 `cleanupWarning` 及保留的备份路径，不删除未核验内容。

每次提升作业的 `result.json` 和 `backend.log` 位于 `%ProgramData%\AirdropForWindowsSetup\<作业编号>\`。只删除该作业的已核验临时解压内容；结果和日志保留用于定位失败。安装器自身目前未作 Authenticode 签名。

## English developer notes

This is the source for the **Airdrop for Windows 0.2.3** x64 single-EXE installer by **Dy1an-Zou**. The UI uses .NET Framework 4.8 Windows Forms; deployment is handled by `setup-backend.ps1`. Build it with the Windows .NET Framework MSBuild command above. Inno Setup is not required.

`PayloadDir` must contain an already verified `payload.zip` and `manifest.sha256`. The ZIP must contain `runtime-manifest.json` at its root and exactly the files listed in that manifest, with no directory entries or extra top-level folder. The hash file is the SHA-256 of the original manifest bytes, encoded as ASCII or UTF-8 without a BOM. The public source archive is not a runtime payload.

`--verify` checks the embedded payload without installing. `--render zh-CN` and `--render en` produce UI previews without installing. Normal execution and `--uninstall` open their respective windows; elevation occurs only after the user selects the action. The 0.2.3 installer compiled without warnings or errors; its payload verification, 39 isolated backend fixtures, 4 directed fault checks, bilingual 200% DPI previews, and 4 in-memory language-switch checks passed. These checks do not establish successful installation or removal. No live install, update, uninstall or fresh-PC acceptance has been performed.

Setup requires Windows x64 build 26100 or later, .NET Desktop Runtime 10 x64, and exactly one dedicated MT7925 with the exact hardware ID and preconfigured driver hash listed above. It discovers the local device instance automatically. It does not install or replace drivers, change test-signing settings, start the app after installation, or restart Windows.

The current Windows account must be an administrator, and elevation must use that same account. Updates accept only the specifically pinned 0.2.2 installation or this exact 0.2.3 manifest. Exit the app from the tray before updating or uninstalling; Setup does not force-stop it. Updates preserve startup preferences, application settings, history, runtime identity, and received files.

Uninstall preserves user files, preferences, diagnostic data, and drivers. If the installed uninstaller is still in use, its copy in `%CommonProgramFiles%\AirdropForWindowsSetup` may remain; `setupCleanupPending` and related result fields identify this condition. Results and logs are retained in `%ProgramData%\AirdropForWindowsSetup\<job-id>\`. The installer is currently unsigned.
