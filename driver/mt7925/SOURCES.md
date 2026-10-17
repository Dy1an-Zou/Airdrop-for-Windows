# MT7925 PCIe 源码与参考来源

这是当前实机接收组合中的实验性 Windows PCIe 移植，采用独立的 **BSD-3-Clause-Clear** 许可。目录内 `LICENSE` 和文件级版权注记保持原样；本项目的其他许可不替代这里的声明。

上游参考为 [openwrt/mt76](https://github.com/openwrt/mt76)，固定版本：

`467ae2e3f190baf737e15e318906da0dc7d0ab91`

主要参考作者包括 Felix Fietkau、MediaTek Inc.；没有复制 GPL wifit3 的实现。

| 本地模块 | 上游参考及用途 |
| --- | --- |
| `core.*`、`pcie_mcu.*` | `mt7925/mcu.c`、`mt76_connac_mcu.h`：PCIe MCU 封装、序号、统一和旧式命令布局 |
| `firmware.*` | `mt76_connac_mcu.c`、`mt792x_core.c`：patch/RAM 布局、下载模式、patch semaphore、固件启动 |
| `chip_control.*`、`tx_init.*` | `mt792x_core.c`、`mt792x_dma.c`、`mt7925/pci.c`：所有权、复位、DMA、队列与固定映射 |
| `rx_ring.*`、`raw_tx.*` | `dma.h`、`dma.c`：DMA 描述符、完成状态及环形队列约束 |
| `raw_rx.*` | `mt7925/mac.c`、`mt76_connac3_mac.h`、`mt792x.h`：RXD 字段、组顺序、频道、A-MSDU 填充；`mt7925/mac.c` blob 为 `7af996abe2940c3724526a0edc10fe4fefde987b` |
| `nic_caps.*`、`tx_power.*`、`clc_power.*`、`regulatory.*`、`efuse_type.*` | mt76 固件能力、功率、监管与芯片信息相关定义，具体来源保留在源文件内 |
| `device.*`、`platform_acpi.*`、`radio_device.*` | Windows KMDF 资源、DMA、权限和设备接口绑定 |

Windows 绑定使用微软 WDF 接口：

- [Common Buffers](https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/using-common-buffers)
- [WDF DMA Profile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdfdmaenabler/ne-wdfdmaenabler-_wdf_dma_profile)
- [BUS_INTERFACE_STANDARD](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_bus_interface_standard)

实现使用显式小端序列化、32 位 DMA 地址范围检查、固定队列和同步约束；用户态不得提供任意内核地址或寄存器写入命令。原生用户态无线进程、驱动和固件必须保持匹配。

本源码从已验证 r36 对应源包提取，内核 `.c/.h` 和 INF 未经本次公开整理修改。原始文件摘要和发行来源见 [PROVENANCE.json](../../docs/PROVENANCE.json)。硬件、系统和测试签名边界见 [HARDWARE.md](../../docs/HARDWARE.md)。源码存在不代表可直接装到其他设备。
