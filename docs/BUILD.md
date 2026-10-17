# 构建与打包

当前桌面程序和安装器为 **0.2.5**，两种网卡分别打包。桌面测试入口为 `app/tests/FileInteractionChecks.csproj`，安装器入口为 `app/installer/Setup.csproj`。发布包复用已验证的 0.2.5 桌面产物；原生组件分别沿用 0.2.3 MT / 0.2.4 RTL，避免把桌面更新变成驱动更新。

0.2.5 结果与限制见 [发布说明](releases/v0.2.5.md)，RTL 原生依赖见 [RTL8852CE.md](RTL8852CE.md)。下文保留 0.2.3 MT7925 的构建方法与历史记录，其中版本号与验证状态属于原版本。

## 桌面应用

需要 Windows x64、.NET SDK 10，以及运行时的 .NET Desktop Runtime 10 x64。当前项目没有额外 NuGet 包依赖。

```powershell
dotnet build .\app\XiatiaoAirDrop.csproj -c Release
```

输出为 `build/app/XiatiaoAirDrop/`，中间文件在 `build/app/obj/`。只运行这个输出不能启用无线收发：高权限助手要求固定安装目录、完整包清单和指定硬件。0.2.3 的中文 / English 字典嵌入桌面程序集，软件默认中文，可通过主窗口语言选择器切换。

## AWDL 和用户态无线进程

需要 Python 3、Visual Studio C++ 工具链、Windows SDK，以及 Clang/LLD。已验证历史基线为 MSVC 14.32.31326、Windows SDK 10.0.19041.0、Clang/LLD（原构建使用 ROCm 6.2 中的 Windows 工具链）。公开入口改为显式工具链参数，不包含作者的磁盘路径。

从配置好 Visual Studio x64 开发环境的终端运行；也可以将参数替换为实际安装目录：

```powershell
python .\awdl\build_clang.py --out .\artifacts\awdl `
  --vc-root $env:VCToolsInstallDir --sdk-root $env:WindowsSdkDir `
  --sdk-version ($env:WindowsSDKVersion.TrimEnd('\')) `
  --clang (Get-Command clang.exe).Source

python .\radio\build_mt7925.py --out .\artifacts\radio `
  --vc-root $env:VCToolsInstallDir --sdk-root $env:WindowsSdkDir `
  --sdk-version ($env:WindowsSDKVersion.TrimEnd('\'))
```

两条命令均在构建后运行自身的离线检查，不操作网卡、驱动或无线传输；输出目录必须是新的。AWDL 入口包含真实序列化器对固定合成帧的比较。`radio/CMakeLists.txt` 的其他上游后端没有随本仓库打包；若使用 CMake，必须指定 `WINAWDL_MT7925_ONLY=ON`。

## 实验内核

```powershell
python .\driver\mt7925\build_driver.py --artifact-dir .\artifacts\kernel --compiler msvc --linker msvc
```

此入口查找本机 MSVC，并使用微软 NuGet 的 `10.0.26100.1` WDK/SDK 开发文件，包含受限下载和提取，总临时空间上限 2 GiB。脚本也要求 PATH 中可以找到 `clang-cl.exe` 和 `lld-link.exe`。它只生成未签名 SYS，不生成可直接投入普通电脑的驱动发行包。安装、签名及适配需独立完成；这里没有一键关闭安全设置的脚本。

## 收发引擎

`airdrop/` 为随运行版使用的 Python 源码，当前源快照为 r3。部署版使用 Python 3.14.8 x64 embeddable；应用通过固定标准输入/输出协议启动 `engine.py`，接收和发送共享受控无线连接。`sender.py` 负责发现、请求确认和上传，`airdrop-beacon.ps1` 使用系统蓝牙接口辅助发现。不需要 pip 依赖。`receive_trial.py` 保留为早期单次传输诊断入口，桌面程序使用的是持续运行的 `engine.py`。

`airdrop/pyproject.toml` 保留底层协议包自己的元数据版本，不作为桌面版本号；当前桌面应用和安装器候选版本为 0.2.3。

**0.2.0 历史检查：**当时的个人候选完成 48 项后端测试、4 项定向反例和本机真实 HTTPS 回环；桌面状态、临时文件复制与回收、接收集成检查通过，渲染绑定错误为 0。

随后已安装的本机 0.2.0 + 专用 MT7925 向 iPhone 发送一张图片成功，用户确认收到，运行日志记录 accepted 和 sent；此前 iPhone → Windows 照片接收另有成功确认。这些实机结果只覆盖本机组合，不能扩展为所有 Apple 设备兼容性。公开整理副本没有另行构建，也没有完成跨电脑验证。

## 完整应用打包

依赖来源与摘要见 [DEPENDENCIES.json](DEPENDENCIES.json)。自行准备以下目录：

- `PublishDir`：本次桌面构建输出。
- `PythonDir`：官方便携 Python x64 解压目录，保留原 LICENSE。
- `RuntimeDir`：`awdld.exe`、`winawdl_mt7925.exe`、原样 `wintun.dll`。
- `FirmwareDir`：两份指定 MediaTek 固件及原 `LICENCE.mediatek`。

```powershell
.\app\scripts\package-xiatiao.ps1 `
  -PublishDir .\build\app\XiatiaoAirDrop `
  -PythonDir .\tools\python `
  -RuntimeDir .\artifacts\runtime `
  -FirmwareDir .\firmware `
  -OutputDir .\dist\XiatiaoAirDrop
```

打包不启动程序、网络或硬件，输出目录必须不存在。脚本保留当前受信任原生组件/固件摘要；重新构建后摘要不同会拒绝打包，需要先完成新组合的验证及摘要绑定。打包会附带完整源码树和许可证，不把仅有上游链接当作修改源码的替代。

内部程序标识和默认安装目录仍为 `XiatiaoAirDrop` / `C:\Program Files\XiatiaoAirDrop`，显示名称为「Airdrop for Windows」。

该步骤产出的是完整运行目录，还不是单 EXE 安装器。0.2.3 的安装 / 更新入口为下节的 `Airdrop-for-Windows-0.2.3-Setup-x64.exe`。早期 `scripts/install-xiatiao.ps1` 等脚本不是本轮推荐给使用者的新安装入口，也不应据旧说明先卸载再更新。

本次公开副本未执行上述构建或硬件适配。0.2.3 根据固定硬件 ID 自动识别唯一匹配的本机设备实例，不再要求手填作者的实例；匹配实验驱动与固定摘要的要求仍然保留。

## 0.2.3 单 EXE 安装器

安装器位于 `app/installer/`，使用 Windows 自带的 .NET Framework MSBuild 和 C# 编译器，目标 .NET Framework 4.8 / x64，不依赖 Inno Setup。桌面应用的 .NET Desktop Runtime 10 x64 仍是独立运行前提。

将已核验的完整 0.2.3 运行目录封装为 `payload.zip`：ZIP 根部放 `runtime-manifest.json` 及其列出的全部文件，不加外层目录和目录条目。另准备 `manifest.sha256`，保存清单原始字节的 SHA-256，编码使用 ASCII 或无 BOM 的 UTF-8。这两个文件放在同一个 payload 目录中。

从源码根目录运行，替换为实际准备好的路径：

```powershell
$payloadDir = 'C:\Build\Airdrop-for-Windows\payload'
$setupOutputDir = 'C:\Build\Airdrop-for-Windows\installer\'
& "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\MSBuild.exe" `
  .\app\installer\Setup.csproj `
  "/p:PayloadDir=$payloadDir" "/p:OutDir=$setupOutputDir"
```

输出名为 `Airdrop-for-Windows-0.2.3-Setup-x64.exe`。`--verify <新结果文件.json>` 只校验内嵌包；`--render zh-CN <图片.png>` / `--render en <图片.png>` 只生成安装界面预览。校验或渲染通过不代表安装、更新或卸载实机通过。

安装器提供中英界面，由用户点击按钮后请求管理员确认。它检查 Windows x64 build 26100 及以上、Desktop Runtime 10 x64、唯一精确匹配的专用 MT7925，以及已配置好的固定版本实验驱动。安装程序不部署或替换驱动，不改变测试签名与其他系统安全设置。

使用原安装账户从托盘正常退出应用后，再安装或更新。更新只接受固定清单对应的指定 0.2.2 或清单完全相同的本次 0.2.3。设置、历史、运行时身份、收到的文件和已有自启动偏好保留；安装成功后不会自动启动软件，从开始菜单自行打开。卸载也保留用户数据和驱动，卸载器自身因占用而暂留的状态会单独记录。用户步骤见 [INSTALL.md](INSTALL.md)，开发接口见 `app/installer/README.md`。

## 0.2.1 界面修正验证（历史）

桌面编译成功，10 项针对改动的检查通过：框选阈值、正反向、Ctrl 增减、Shift 追加、取消恢复、禁用时取消、边缘滚动、实际控件胶囊形与鼠标焦点，以及隐藏非激活窗口中的 400ms 动态采样和中途反向连续性。检查不发送桌面鼠标或键盘输入，不启动无线。

用编译后的完整桌面目录执行 `XiatiaoAirDrop.exe --xiatiao-ui-check <输出目录>` 可运行这些离线检查。界面截图是实际 WPF 控件渲染，文件与设备名称是示例。此检查不是物理鼠标验收，也不是手机传输验收。

0.2.1 沿用已验证的无线驱动和收发引擎，该轮没有重做 iPhone 收发测试；0.2.0 的实机记录保留其原有范围。公开副本已脱敏但未另行构建。

## 0.2.2 对齐修正验证（历史）

桌面编译成功，零警告。`XiatiaoAirDrop.exe --xiatiao-render <输出目录>` 在实际 WPF 布局中检查普通窗口、窄窗口滚动条、滚动到底部及恢复普通尺寸；两个开关的右边缘差均为 0 DIP。折叠设置后顶部开关保持可见，展开后恢复对齐。检查同时生成布局截图和 `switch-alignment.json`；没有发送桌面输入。无线及文件传输后端未改变，该轮未重做手机收发验收。

## 0.2.3 当前验证状态

本轮已实现界面中英切换、唯一匹配网卡自动识别和单 EXE 安装 / 更新 / 卸载入口，当前结果如下：

| 检查范围 | 0.2.3 当前结果 |
| --- | --- |
| 桌面候选编译 | 顶栏语言选择器的最终布局已重新编译，通过，0 错误、0 警告。 |
| 语言行为 | 语言偏好持久化、状态中英双向切换、用户文件名保留，共 3 项通过。 |
| 无线助手离线检查 | 49 项通过；不代表实机无线连接或驱动操作验收。 |
| 中英 WPF 离屏渲染 | 已查看两语普通、紧凑、发送和关于界面，无绑定错误；4 种展开状态下开关右边缘差均为 0 DIP，普通窗口设置区没有滚动条。 |
| 安装后端隔离样例 | 39 项通过，未执行真实系统安装 / 卸载。 |
| 安装后端定向变异 | 4 个变异全部被检查捕获。 |
| 安装器 EXE 候选构建与内嵌包校验 | 编译 0 错误、0 警告，内嵌包 199 个文件核验通过。 |
| 真实只读环境预检 | 在当前受限令牌的 `ADMIN_ACCOUNT` 检查处停止，无系统变更；不是完整环境预检通过。 |
| 实际安装 / 更新 / 卸载、新电脑、0.2.3 手机传输 | 未执行。 |

语言选择器已移至主窗口顶栏，并完成重新编译、布局复核和中英预览图更新。安装器使用「中文 / English」语言按钮，初始语言跟随系统界面语言。

原生收发组件、固件、Python 运行时和图标逐项 SHA-256 保持一致，实际接收与发送代码未改变。脱敏仅涉及离线 `receive_trial.py` 的示例名称，以及源码归档中的个人名称和构建路径，不扩大协议或硬件支持范围。

本轮只读预检没有越过管理员账户检查，不能据此声称运行时、设备、驱动及当前安装状态已全部通过现场预检。未执行实际安装 / 更新 / 卸载，现有 0.2.2 安装继续保留。

0.2.0 的 48 项后端检查、本机回环和双向图片实机记录，以及 0.2.1 / 0.2.2 的界面结果都是各自版本的历史证据，不作为 0.2.3 新增功能的验收。公开副本没有独立重建结果，新电脑安装和更广 Apple 兼容性也尚未验证。
