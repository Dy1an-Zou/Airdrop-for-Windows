# Uiverse 组件选择与改编映射

固定仓库版本：`adbd2adde0a299a3956ea288fb444ec01891ca41`。原始组件文件保持未修改；采用项与来源摘要见 component-sources.json。布局依据用户图二。2026-10-16 按用户最终指定，将两个开关更换为 gharsh11032000 的 green-liger-89。

| 用途 | 作者 | 原始文件 | 页面 | 改编说明 |
|---|---|---|---|---|
| button | AhSiber | [galaxy/Buttons/AhSiber_great-gecko-98.html](galaxy/Buttons/AhSiber_great-gecko-98.html) | [Uiverse](https://uiverse.io/AhSiber/great-gecko-98) | 保存、选择文件夹、打开文件、关于、确认与拒绝；保留描边和悬停填色，取消斜体 |
| input | alexruix | [galaxy/Inputs/alexruix_slippery-frog-10.html](galaxy/Inputs/alexruix_slippery-frog-10.html) | [Uiverse](https://uiverse.io/alexruix/slippery-frog-10) | 名称/保存路径/历史搜索；灰底8px圆角，琥珀焦点，名称与路径不放搜索图标 |
| toggle | gharsh11032000 (Harsh Gupta) | [galaxy/Toggle-switches/gharsh11032000_green-liger-89.html](galaxy/Toggle-switches/gharsh11032000_green-liger-89.html) | [Uiverse](https://uiverse.io/gharsh11032000/green-liger-89) | AirDrop总开关、登录启动；忠实采用59.5×34比例、白色34圆点、关闭#9fccfa/开启#0974f1、27.2位移、向下10/模糊20/40%黑影；400ms cubic-bezier(0.175,0.885,0.32,1.275)，系统减少动画时即时切换 |
| card | Sujitkavaiya | [galaxy/Cards/Sujitkavaiya_mighty-pig-81.html](galaxy/Cards/Sujitkavaiya_mighty-pig-81.html) | [Uiverse](https://uiverse.io/Sujitkavaiya/mighty-pig-81) | 设置区容器、传输列表容器、文件缩略图、对话框外壳；暖白底浅描边，悬停琥珀描边 |
| about | alexruix | [galaxy/Cards/alexruix_tough-shrimp-75.html](galaxy/Cards/alexruix_tough-shrimp-75.html) | [Uiverse](https://uiverse.io/alexruix/tough-shrimp-75) | 关于页大logo上方与居中作者文字结构；去掉彩虹渐变/悬浮位移，使用虾条logo |
| notification | alexruix | [galaxy/Notifications/alexruix_gentle-octopus-87.html](galaxy/Notifications/alexruix_gentle-octopus-87.html) | [Uiverse](https://uiverse.io/alexruix/gentle-octopus-87) | 接收请求和状态条：头像+文字结构；取消登场/循环动画，改为常驻可读反馈 |

统一视觉：暖白画布、炭灰正文、虾条琥珀强调；开关按用户最终选定组件采用蓝色与原版圆点阴影。按钮/输入/开关/卡片/状态/关于页均从以上来源改编；原生 Windows 标题栏、系统托盘菜单和文件选择器属于系统控件，不冒充 Galaxy HTML。文件图像是用户收到的内容；品牌插画是用户提供素材的改编。

许可：Galaxy 仓库 LICENSE 原文在 GALAXY-LICENSE.txt；每项作者的 MIT 许可另存于 ../licenses/Uiverse-MIT.txt。保留原作者署名不表示原作者为本项目背书。

进度条补充：`galaxy/loaders/mrhyddenn_tasty-crab-98.html`，mrhyddenn，https://uiverse.io/mrhyddenn/tasty-crab-98 。细圆角轨道+填充，宽度绑定实际进度。`doniaskima_yellow-deer-12.html` 仅候选备用，默认不采用。
