# 最低延迟版 EP2 数据面（门铃 + 状态轮询 + 预分配 DMA）

## 设计要点（相对 pci_epf_test）

| 项目 | pci_epf_test | 本方案 |
| --- | --- | --- |
| 命令发现 | 1ms `delayed_work` 轮询；门铃 handler 空转 | 门铃 IRQ → 立刻 `queue_work` |
| 完成通知 | MSI/MSI-X（你们链路上会随机失效） | **RC 轮询 status 寄存器**（关键路径不用中断） |
| 缓冲区 | 每次传输分配 + CRC + 随机数 | 启动预分配 4MB，无 CRC |
| 目标 | 功能验证 | 4KB 端到端目标 **20~40µs**（相对 dmalat 的 2~5ms） |

协议见 `infer_proto.h`。设备 ID：`1ef1:0301`。

## 文件

- `pci_epf_infer.c` — EP 侧 function 驱动（装在 EP 板）
- `infer_rc.c` — RC 侧主机驱动（装在 RC 板）
- `inferlat.c` — 用户态延迟扫表（4KB→2MB）
- `infer_proto.h` — 共享协议
- `bst_doorbell.h` — 门铃 API 前向声明（编译时优先用内核树里的 `pcie-bst.h`）

## 编译

```bash
# 内核树需为 6.6.64-rt47，且 make modules_prepare 已完成
export KDIR=/path/to/linux-6.6
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-

make -C $KDIR M=$PWD modules
make inferlat
```

若编译报找不到 `bst_pcie_ep_db_*`，确认 Makefile 的 `-I$(KDIR)/drivers/pci/controller/bst`，并把 `pci_epf_infer.c` 顶部改成直接 `#include <pcie-bst.h>`（与 `pci-epf-test.c` 相同方式）。

## 部署与枚举（两板成对重启后）

**EP 板：**

```bash
insmod pci_epf_infer.ko
cd /sys/kernel/config/pci_ep/
mkdir -p functions/pci_epf_infer/func1
echo 0x1ef1 > functions/pci_epf_infer/func1/vendorid
echo 0x0301 > functions/pci_epf_infer/func1/deviceid
ln -s functions/pci_epf_infer/func1 controllers/73000000.pcie2_ep/
echo 1 > controllers/73000000.pcie2_ep/start
dmesg | tail -20
# 期望: doorbell bar=... off=0xe00 ... 以及 DMA ready
```

**RC 板：**

```bash
echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove 2>/dev/null
echo 1 > /sys/bus/pci/rescan
setpci -s 0000:00:00.0 CAP_EXP+10.w=0020
lspci -n -s 01:00.0    # 期望 1ef1:0301
insmod infer_rc.ko
ls /dev/infer_rc0
```

**测延迟：**

```bash
./inferlat /dev/infer_rc0 w 100
./inferlat /dev/infer_rc0 r 100
```

## 关键路径时序

```
RC: status=IDLE → size/pci_addr → command → wmb → writel(doorbell)
EP: doorbell IRQ → queue_work → DMA → status=OK
RC: poll status until OK  → 返回耗时(ns)
```

## 已知限制 / 下一步

1. DMA channel 申请目前用通用 `DMA_MEMCPY` filter；若板上拿不到通道，需按 `pci-epf-test.c` 的 `epf_dma_filter_fn` / `DMA_SLAVE|DMA_PRIVATE` 方式对齐 dw-edma。
2. EP 侧 DMA 目前把数据搬进 EP 本地 4MB 缓冲；真正推理要把 `pci_addr` 映射到 NPU 可见内存（第二版）。
3. 双链路 EP2：每板各装 EP 模块 + 对面板装 RC 模块。
4. 成对重启纪律不变（BST EP 复位恢复未修好前）。
