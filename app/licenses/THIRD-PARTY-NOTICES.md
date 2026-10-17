# 第三方软件与素材说明

应用作者及桌面整合：**Dy1an-Zou**。上游项目和组件的版权仍归原作者，各文件内的许可证与署名保留。

| 组件 | 作者 / 来源 | 许可及随附材料 |
| --- | --- | --- |
| winawdl 应用基础 | Raphaël Richaud · [winawdl](https://github.com/raph559/winawdl) | MIT · `winawdl-MIT.txt` |
| AWDL / OWL | Milan Stute、The Open Wireless Link Project、SEEMOO / TU Darmstadt · [OWL](https://github.com/seemoo-lab/owl) | GPL-3.0-or-later，保留文件级例外 · `AWDL-GPL-3.0.txt`、`awdl/`、`upstream/owl/` |
| MT7925 用户态无线进程 | winawdl 原生移植和本项目修改 | GPL-2.0-only，部分头文件单独标明 MIT/BSD · `Radio-GPL-2.0.txt`、`radio/` |
| MT7925 内核移植 | Felix Fietkau、MediaTek Inc. 的参考实现及本项目移植 · [mt76](https://github.com/openwrt/mt76/tree/467ae2e3f190baf737e15e318906da0dc7d0ab91) | BSD-3-Clause-Clear · `MT7925-Clear-BSD.txt`、`driver/mt7925/` |
| AirDrop 接收引擎 | Brent Kearney · [omdrop](https://github.com/brentkearney/omdrop-plugin) | MIT · `omdrop-MIT.txt`、`airdrop/`；保留 receiver.py / transfer.py 注记 |
| AirDrop 发送代码 | Raphaël Richaud · [winawdl](https://github.com/raph559/winawdl) 与本项目修改 | MIT · `winawdl-MIT.txt`、`airdrop/sender.py`；保留文件内署名 |
| Radiotap | Andy Green、Johannes Berg | ISC · `Radiotap-ISC.txt`、`upstream/owl/radiotap/` |
| OWL log.c | rxi | MIT · `rxi-log-MIT.txt` |
| SipHash | Frank Denis / libsodium 1.0.16 | ISC · `libsodium-1.0.16-ISC.txt` |
| UI 组件 | AhSiber、alexruix、Sujitkavaiya、mrhyddenn、gharsh11032000 (Harsh Gupta) · [Uiverse / Galaxy](https://github.com/uiverse-io/galaxy) | MIT · `Galaxy-MIT.txt`、`Uiverse-MIT.txt`；原始 HTML 和来源在 `app/design-sources/` |
| Wintun 0.14.1 | WireGuard LLC · [Wintun](https://www.wintun.net/) | 独立预编译分发许可 · `Wintun-0.14.1-Prebuilt.txt`；源码仓库不含 DLL |
| MT7925 固件 | MediaTek Inc. / linux-firmware | `MediaTek-firmware.txt`，固件用于 MediaTek 芯片并保留独立许可；源码仓库不含固件二进制 |
| Python 3.14.8 | Python Software Foundation 及各依赖作者 · [Python](https://www.python.org/downloads/release/python-3148/) | PSF-2.0 与依赖许可 · Python-* 原文及官方 SBOM；源码仓库不含 Python 运行时 |

OWL 的 hashmap 文件保留 Elliot C Back、Pete Warden、Milan Stute 的原始注记。桌面代码自己的 MIT 授权不覆盖这些独立组件、固件、品牌图形或商标。

## 对应源码

本仓库包含实际使用的 AWDL、用户态无线进程、驱动、Python 引擎和桌面应用源文件。仓库根的 `docs/PROVENANCE.json` 记录原冻结源包与公开整理副本之间的逐文件摘要和变动；`docs/DEPENDENCIES.json` 记录本机已验证原生二进制的摘要。

公开副本的修改包括个人标识中性化、工具链路径参数化、公共文档和定稿图标。协议、驱动实现及其安全校验未为公开打包而重写。公开副本未重新编译或进行另一台设备的验证。重新分发自行修改后的 GPL 组件时，应同时提供与该发行版相应的实际源码、构建入口和许可证，不能只提供上游项目链接。

运行包的打包入口将源码保存在包内 `source/`，此时上述目录位于 `source/` 下。Wintun、固件和 Python 应按各自条款原样保留通知，不改署名或重新声明为本项目 MIT。

## 图标、名称与商标

虾条原画与徽章素材由项目作者提供，定稿通过 Aseprite 分层合成，编辑原稿在 `artwork/`。图形来源见 `app/design-sources/ARTWORK.md`。

AirDrop、Apple、iPhone 和 Mac 是 Apple Inc. 的商标。AirDrop 标识归 Apple Inc. 所有，不声明为本项目原创，不包含在代码的 MIT 授权中。本软件为独立第三方项目，与 Apple Inc. 无隶属、赞助或认可关系；名称用于说明兼容功能。
