# openclaw-work

## PCIe EP2 数据面（A2000 双板）

| 文档 / 目录 | 内容 |
| --- | --- |
| [`pcie-ep-rc-pci-epf-test-communication.md`](pcie-ep-rc-pci-epf-test-communication.md) | 前期用 `pci_epf_test` / `pcitest` 验证链路 |
| [`pcie-epf-infer-minlat/`](pcie-epf-infer-minlat/) | **自研最低延迟栈**（门铃 IRQ→eDMA，4KB ~17µs） |
| → [`README.md`](pcie-epf-infer-minlat/README.md) | 总览与快速部署 |
| → [`BRINGUP.md`](pcie-epf-infer-minlat/BRINGUP.md) | 板测全记录与实测表 |
| → [`BUILD.md`](pcie-epf-infer-minlat/BUILD.md) | 编译 / 拷板 |
| → [`ZEROCOPY.md`](pcie-epf-infer-minlat/ZEROCOPY.md) | v2 零拷贝协议（已接线+板测） |

其它笔记：`deepseek-v4-flash-single-layer-int4-npu.md`、`openclaw-qmd-local-kb-practice.md`。
