# 最低延迟版 EP2 数据面（门铃 + 状态轮询 + 预分配 DMA）

## 设计要点（相对 pci_epf_test）

| 项目 | pci_epf_test | 本方案 |
| --- | --- | --- |
| 命令发现 | 1ms `delayed_work` 轮询；门铃 handler 空转 | 门铃 IRQ → 立刻 `queue_work` |
| 完成通知 | MSI/MSI-X（你们链路上会随机失效） | **RC 轮询 status 寄存器**（关键路径不用中断） |
| 缓冲区 | 每次传输分配 + CRC + 随机数 | 启动预分配 4MB，无 CRC |
| 目标 | 功能验证 | 4KB 端到端目标 **20~40µs**（相对 dmalat 的 2~5ms） |

协议见 `infer_proto.h`。设备 ID：`1ef1:0301`。

完整工作记录（设计动机、平台坑、编译部署、板测步骤、实测数据）见 **`BRINGUP.md`**。

## BST BAR 布局（必读）

| BAR | 内容 | 主机可见 |
| --- | --- | --- |
| **BAR0** | MSI-X 表 + 门铃（硬件，通常 `+0xe00`） | **不是** DDR；读 offset 0 得到 0 / 垃圾 |
| **BAR1** | 协议寄存器（`infer_regs`，含 magic） | inbound-ATU → EP DDR；**magic 必须在这里** |

旧版把 magic 写在 BAR0+0，EP 本地能看到，主机永远读不到 → `infer_rc` probe 失败 `-22`。

## 文件

- `pci_epf_infer.c` — EP 侧 function 驱动
- `infer_rc.c` — RC 侧主机驱动
- `inferlat.c` — 用户态延迟扫表（4KB→2MB）
- `infer_proto.h` — 共享协议
- `bst_doorbell.h` — 门铃 API 前向声明

## 编译

见 `BUILD.md`（推荐 `./install-into-kernel.sh`）。

## 部署与枚举（两板成对重启后）

**每板先做本板 EP（两端都 ready 后再各自做 RC 枚举）：**

```bash
insmod /userdata/ep_test/pci_epf_infer.ko
ls /dev/pci_epf_infer_ctl   # 必须存在

cd /sys/kernel/config/pci_ep/
mkdir -p functions/pci_epf_infer/func1
echo 0x1ef1 > functions/pci_epf_infer/func1/vendorid
echo 0x0301 > functions/pci_epf_infer/func1/deviceid
ln -s functions/pci_epf_infer/func1 controllers/73000000.pcie2_ep/
echo 1 > controllers/73000000.pcie2_ep/start
sleep 1
echo 1 > /dev/pci_epf_infer_ctl    # 必须：start 会清掉 bind 时的 BAR/ATU
dmesg | grep reprogram | tail -3
# 必须: reprogram done magic=0x494e4652
# 必须: ctrl BAR1 programmed ...
```

**再在本板做对端 EP 的 RC 枚举：**

```bash
rmmod infer_rc 2>/dev/null
echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove 2>/dev/null
sleep 1
echo 1 > /sys/bus/pci/rescan
setpci -s 0000:00:00.0 CAP_EXP+10.w=0020
sleep 1

lspci -n -s 01:00.0
# 期望: 1ef1:0301
lspci -vv -s 01:00.0 | grep Region

# 用 BAR1 的 CPU 地址验 magic（resource 第 2 行，不是第 1 行）
sed -n '2p' /sys/bus/pci/devices/0000:01:00.0/resource
# 例: 0x0000000900b00000 ...  →  busybox devmem 0x900b00000 32
# 期望: 0x494E4652
# BAR0（第 1 行）读到 0 / 0xFFFFFFFF 是正常的

insmod /userdata/ep_test/infer_rc.ko
dmesg | tail -10
# 期望: infer regs on BAR1 ... /dev/infer_rc0
ls /dev/infer_rc0
```

**测延迟：**

```bash
./inferlat /dev/infer_rc0 w 100
./inferlat /dev/infer_rc0 r 100
```

## 关键路径时序

```
RC: status=IDLE → size/pci_addr → command → wmb → writel(BAR0+db_off)
EP: doorbell IRQ → queue_work → DMA → status=OK（写在 BAR1）
RC: poll BAR1 status until OK  → 返回耗时(ns)
```

## 已知限制

1. 成对重启纪律不变（BST EP 软复位恢复未修好前）。
2. EP 侧 DMA 目前搬进 EP 本地 4MB；真正推理要把缓冲映射到 NPU 可见内存。
3. 双链路 EP2：每板各装 EP 模块 + 对面板装 RC 模块。
