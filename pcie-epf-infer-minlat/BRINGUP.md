# PCIe 最低延迟 EP2 数据面：从内核模块到板测全记录

> 平台：黑芝麻 A2000（双板互连，PCIe Gen4 x4）  
> 内核：`6.6.64-rt47`  
> 设备 ID：`1ef1:0301`  
> 代码目录：`pcie-epf-infer-minlat/`  
> 实测结果（A 板 RC→对端 EP）：4KB 中位约 **24µs**，2MB 带宽约 **6.2 GB/s**

本文记录从「自研内核模块」开始到「板子上跑通 `inferlat`」的全部工作：设计动机、平台坑、编译部署、板测步骤、验收标准与实测数据。

---

## 1. 背景与目标

两块 A2000 通过 PCIe Gen4 x4 双线缆互连，每板同时是一端的 EP、另一端的 RC（对称拓扑）。上层要做 EP2 风格推理数据面，需要：

1. 可靠的 RC↔EP 内存搬运（DMA）
2. **端到端延迟尽量低**（尤其是 4KB 小包）

前期用 BSP 自带的 `pci_epf_test` + `pcitest` 验证了链路与 DMA 吞吐（约 6.5 GB/s），但用其做延迟测量（`dmalat`）得到 **2～5 ms**，远高于链路能力。根因是框架开销，不是物理链路：

| 开销来源 | 说明 |
| --- | --- |
| EP 侧 `delayed_work` | 门铃 IRQ 只打印，真正处理命令仍按 **1ms** 轮询 |
| 主机侧重试 / CRC | `pci_endpoint_test` 路径带校验与多次往返 |
| MSI 不稳定 | 本平台 MSI 偶发失效，不能当关键路径完成通知 |

因此自研一套 **门铃触发 + 状态寄存器轮询 + 预分配 DMA** 的最小延迟栈，目标：

- 4KB 端到端：**20～40µs**
- 大包带宽：接近 `pcitest` DMA 峰值（~6 GB/s）
- 关键路径 **不依赖 MSI**

---

## 2. 软件架构

### 2.1 组件

| 文件 | 角色 | 装在哪 |
| --- | --- | --- |
| `pci_epf_infer.c` | EP function 驱动 | 每板的 EP 口（`73000000.pcie2_ep`） |
| `infer_rc.c` | RC 主机驱动 | 每板的 RC 口（枚举到的 `0000:01:00.0`） |
| `infer_proto.h` | 共享协议（magic / 寄存器布局 / ioctl） | 两边模块 + 用户态共用 |
| `inferlat.c` | 用户态延迟扫表（4KB→2MB） | RC 侧用户态 |
| `bst_doorbell.h` | 门铃 API 声明（进树编时优先用内核 `pcie-bst.h`） | 编译辅助 |
| `install-into-kernel.sh` | 拷进内核树并编 `.ko` | 开发机 |

设备 ID 故意用 **`1ef1:0301`**（不用 `0300`），避免和 `pci_epf_test` / `pci-endpoint-test` 冲突。

### 2.2 关键路径时序

```
RC (infer_rc / inferlat)
  1. 写 BAR1: status=IDLE, size, pci_addr, command
  2. wmb
  3. writel(msg) → BAR0 + 0xe00   ← 门铃
  4. 轮询 BAR1 status，直到 OK / FAIL

EP (pci_epf_infer)
  1. 门铃 IRQ → 立刻 queue_work(system_highpri_wq)   ← 无 1ms delayed_work
  2. 读 command/size/pci_addr，清 command，置 BUSY
  3. eDMA slave：RC↔EP 本地 4MB 预分配缓冲
  4. 写 status=OK/FAIL
```

协议寄存器（`struct infer_regs`）布局见 `infer_proto.h`：`magic / command / status / size / pci_addr / db_* / seq`。

### 2.3 BAR 布局（BST 平台必读）

| BAR | 内容 | 主机可见性 |
| --- | --- | --- |
| **BAR0** | MSI-X 表（offset 0）+ **门铃**（通常 `+0xe00`） | **硬件窗口，不是 DDR**；读 offset 0 得到 0 / `0xFFFFFFFF` 是正常的 |
| **BAR1** | `infer_regs`（含 magic=`0x494e4652`） | inbound-ATU → EP DDR；**控制面必须在这里** |
| BAR2 / BAR4 | 平台默认窗口 | 本驱动不使用 |

早期把 magic 写在 BAR0+0：EP 本地 `reprogram done` 正常，主机永远读不到 → `infer_rc` probe 失败 `-22`。修复后控制块在 BAR1，门铃仍走 BAR0。

---

## 3. 开发过程中踩过的坑（按时间线）

### 3.1 必须进树编译

`bst_pcie_ep_db_*` 门铃 API **没有 `EXPORT_SYMBOL`**。`pci_epf_test` 是内置（`=y`）所以能调；纯 out-of-tree `.ko` 会 `Unknown symbol`。

做法：用 `install-into-kernel.sh` 把源码拷进

- `drivers/pci/endpoint/functions/`（EP）
- `drivers/misc/`（RC）

再 `make M=...` 编模块。

### 3.2 `kernelrelease` 必须精确匹配

板子是 `6.6.64-rt47`。开发机若带 `+` 后缀或 `setlocalversion` 污染，模块会 `disagrees about version of symbol`。需要：

- `make -s ARCH=arm64 kernelrelease` → 正好 `6.6.64-rt47`
- 必要时 stub `setlocalversion`
- 从板子拷 `Module.symvers` 到内核源码树根（否则 `__pci_register_driver` 等未定义）

### 3.3 `echo 1 > start` 会清掉 bind 时的 BAR/ATU

BST 控制器在 `start` 时重新初始化，**bind 阶段 `pci_epc_set_bar` 的配置会被抹掉**。主机枚举后看不到正确的 Region / magic。

做法：

1. bind **不**编程 BAR
2. `start` 之后写 `echo 1 > /dev/pci_epf_infer_ctl` 强制 **reprogram**（header + BAR1 + doorbell + eDMA）
3. dmesg 必须出现：`reprogram done magic=0x494e4652`

### 3.4 控制寄存器不能放 BAR0

见 §2.3。症状：EP `reprogram done`，主机 `BAR0 magic=0x0` / `devmem` 得 `0xFFFFFFFF`。  
修复：`INFER_CTRL_BARNO = 1`；RC 分开 map 控制 BAR 与门铃 BAR。

### 3.5 DMA 必须用 PCIe eDMA slave，不能用系统 MEMCPY

系统 DMA 会把 RC 的 PCI 总线地址当成本地物理地址。应对齐 `pci_epf_test`：

- `dma_request_channel(DMA_SLAVE, filter)`，filter 要求 `chan->device->dev == epc->dev.parent`
- 实测通道：`tx=dma1chan0`，`rx=dma1chan8`
- `dma_slave_config` 里填远端 PCI 地址；本地用 `prep_slave_single` 的预分配缓冲

### 3.6 DMA 回调与超时

- `callback_param` 不能指向栈上临时对象（UAF）
- 超时必须 `dmaengine_terminate_sync`，避免迟到回调

### 3.7 成对重启纪律

软 `remove` + `rescan` **不能**可靠恢复对端 EP 被 RC 复位后的 BAR 尺寸掩码（曾出现 BAR1 变成 8MB 等异常）。  
**两板一起 reboot**，再按「先两边 EP ready，再各自 RC 枚举」的顺序操作。

### 3.8 其它操作注意

- 不要跑 `pcitest -c`（会挂）
- 不要对 BAR0 做普通 `-b 0` 功能测试（门铃/MSI-X 表区域）
- MSI 在本链路偶发失效；本方案关键路径用 **status 轮询**，不依赖 MSI
- `busybox devmem` 在设备 **未 enable Memory Space** 前读 BAR 会得到 `0xFFFFFFFF`；以 `insmod infer_rc` 之后的 dmesg 为准

---

## 4. 开发机编译

### 4.1 前置条件

```bash
# 内核树已 modules_prepare，且：
make -s -C ~/workspace/linux-6.6 ARCH=arm64 kernelrelease
# → 6.6.64-rt47

# 若缺 Module.symvers：
scp root@<板IP>:/lib/modules/6.6.64-rt47/build/Module.symvers \
    ~/workspace/linux-6.6/
```

### 4.2 一键安装并编译

```bash
cd /path/to/openclaw-work/pcie-epf-infer-minlat
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
./install-into-kernel.sh ~/workspace/linux-6.6
```

产物：

```
~/workspace/linux-6.6/drivers/pci/endpoint/functions/pci_epf_infer.ko
~/workspace/linux-6.6/drivers/misc/infer_rc.ko
./inferlat
```

### 4.3 拷到两块板（拓扑对称，每板都要 EP+RC）

```bash
BOARD_A=10.28.10.201
BOARD_B=10.28.10.130
KDIR=~/workspace/linux-6.6
SRC=/path/to/pcie-epf-infer-minlat

for IP in $BOARD_A $BOARD_B; do
  ssh root@$IP 'mkdir -p /userdata/ep_test'
  scp $KDIR/drivers/pci/endpoint/functions/pci_epf_infer.ko \
      $KDIR/drivers/misc/infer_rc.ko \
      $SRC/inferlat \
      root@$IP:/userdata/ep_test/
done

# 建议核对 md5，避免板子上还是旧 ko
md5sum $KDIR/drivers/pci/endpoint/functions/pci_epf_infer.ko \
       $KDIR/drivers/misc/infer_rc.ko
ssh root@$BOARD_A 'md5sum /userdata/ep_test/*.ko'
ssh root@$BOARD_B 'md5sum /userdata/ep_test/*.ko'
```

---

## 5. 板子上测试（完整流程）

### 5.0 成对重启

两板同时 `reboot`，等 SSH 恢复。不要只重启一边。

确认内核：

```bash
uname -r    # 6.6.64-rt47
```

### 5.1 两板同时：本板 EP 初始化

在 **A 板和 B 板各自**执行（可并行）：

```bash
cd /userdata/ep_test
insmod ./pci_epf_infer.ko
ls /dev/pci_epf_infer_ctl          # 必须存在

cd /sys/kernel/config/pci_ep/
mkdir -p functions/pci_epf_infer/func1
echo 0x1ef1 > functions/pci_epf_infer/func1/vendorid
echo 0x0301 > functions/pci_epf_infer/func1/deviceid
ln -s functions/pci_epf_infer/func1 controllers/73000000.pcie2_ep/
echo 1 > controllers/73000000.pcie2_ep/start
sleep 1
echo 1 > /dev/pci_epf_infer_ctl    # 必须：post-start reprogram
```

验收（dmesg）：

```bash
dmesg | grep -E 'ctrl BAR|reprogram done|eDMA' | tail -5
```

必须看到类似：

```
ctrl BAR1 programmed size=...
eDMA channels: tx=dma1chan0 rx=dma1chan8
reprogram done magic=0x494e4652 db=0:0xe00
```

**两边都 ready 之后**，再做下面的 RC 枚举。

### 5.2 两板各自：枚举对端 EP 并加载 RC 驱动

```bash
rmmod infer_rc 2>/dev/null

# 若设备节点已存在才 remove；首次可能没有，忽略报错即可
echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove 2>/dev/null
sleep 1
echo 1 > /sys/bus/pci/rescan
setpci -s 0000:00:00.0 CAP_EXP+10.w=0020
sleep 1

lspci -n -s 01:00.0
# 期望: 1ef1:0301

lspci -vv -s 01:00.0 | grep -iE 'Region|LnkSta'
# Region 0/1/2/4 各 1M，有具体 CPU 地址
# LnkSta: Speed 16GT/s, Width x4（或至少 Gen4 协商完成）
```

可选：在 `insmod infer_rc` **之前**用 `devmem` 读 BAR1 可能得到 `0xFFFFFFFF`（设备尚未 enable Memory）。可跳过，直接：

```bash
cd /userdata/ep_test
insmod ./infer_rc.ko
ls /dev/infer_rc0
dmesg | tail -10
```

验收（dmesg）：

```
infer_rc 0000:01:00.0: enabling device (0000 -> 0002)
infer_rc 0000:01:00.0: infer regs on BAR1 len=0x0000000000100000
infer_rc 0000:01:00.0: doorbell mapped BAR0+0xe00
infer_rc 0000:01:00.0: infer_rc ready ctrl=BAR1 db=0:0xe00 msg=0x1 ... /dev/infer_rc0
```

若仍报 `no BAR with magic`：

1. 确认对端已 `echo 1 > /dev/pci_epf_infer_ctl`
2. 确认 ko 是 BAR1 版本（md5 与开发机一致）
3. 成对重启后重做 §5.1→§5.2

### 5.3 跑延迟扫表

```bash
cd /userdata/ep_test
chmod +x ./inferlat

# RC → EP（对端 EP DMA 读本机缓冲）
./inferlat /dev/infer_rc0 w 100

# EP → RC（对端 EP DMA 写本机缓冲）
./inferlat /dev/infer_rc0 r 100
```

参数：`inferlat <设备> <w|r> <每档次数>`。扫 4KB、8KB、…、2MB。

### 5.4 验收标准

| 项 | 期望 |
| --- | --- |
| `/dev/pci_epf_infer_ctl` | EP `insmod` 后存在 |
| reprogram | `magic=0x494e4652`，`ctrl BAR1`，eDMA `dma1chan0/8` |
| `lspci -n` | `1ef1:0301` |
| `/dev/infer_rc0` | `insmod infer_rc` 后存在 |
| 4KB 中位延迟 | 约 **20～40µs**（实测 ~24µs） |
| 2MB 带宽 | 约 **6 GB/s** 量级 |
| w / r | 延迟大致对称 |

---

## 6. 实测数据（A 板，2026-07）

环境：A 板作 RC 测对端（B）EP；`inferlat` 每档 100 次。

### WRITE（RC→EP）

| 块大小 | 最小(µs) | 中位(µs) | 平均(µs) | 99%(µs) | 最大(µs) | 带宽(MB/s) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 4KB | 24.3 | 24.6 | 25.7 | 65.1 | 65.1 | 159 |
| 8KB | 24.7 | 25.5 | 26.5 | 64.8 | 64.8 | 306 |
| 16KB | 26.4 | 27.5 | 27.9 | 65.1 | 65.1 | 569 |
| 32KB | 28.5 | 28.8 | 29.5 | 62.2 | 62.2 | 1086 |
| 64KB | 32.8 | 33.1 | 33.7 | 42.3 | 42.3 | 1888 |
| 128KB | 42.2 | 42.5 | 43.1 | 56.0 | 56.0 | 2940 |
| 256KB | 60.3 | 61.0 | 61.2 | 67.3 | 67.3 | 4097 |
| 512KB | 97.2 | 97.7 | 98.0 | 115.1 | 115.1 | 5120 |
| 1MB | 171.2 | 171.9 | 172.3 | 204.8 | 204.8 | 5819 |
| 2MB | 319.2 | 319.9 | 320.1 | 327.6 | 327.6 | 6252 |

### READ（EP→RC）

| 块大小 | 最小(µs) | 中位(µs) | 平均(µs) | 99%(µs) | 最大(µs) | 带宽(MB/s) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 4KB | 23.4 | 23.9 | 25.0 | 68.7 | 68.7 | 163 |
| 8KB | 24.2 | 25.2 | 25.7 | 63.4 | 63.4 | 311 |
| 16KB | 25.7 | 26.7 | 27.3 | 80.6 | 80.6 | 585 |
| 32KB | 27.6 | 28.9 | 29.4 | 56.6 | 56.6 | 1080 |
| 64KB | 32.8 | 33.4 | 33.7 | 38.8 | 38.8 | 1872 |
| 128KB | 42.4 | 42.6 | 42.9 | 48.6 | 48.6 | 2934 |
| 256KB | 60.2 | 61.4 | 61.9 | 95.8 | 95.8 | 4075 |
| 512KB | 97.5 | 98.6 | 98.7 | 104.5 | 104.5 | 5073 |
| 1MB | 172.8 | 173.6 | 173.6 | 177.1 | 177.1 | 5759 |
| 2MB | 321.8 | 323.5 | 323.9 | 354.2 | 354.2 | 6183 |

相对 `pci_epf_test` 路径的 2～5ms，4KB 延迟约改善 **100×**。小包地板约 23～25µs（门铃 + workqueue + 状态轮询固定开销）；大包受链路吞吐限制，接近此前 DMA 峰值。

---

## 7. 故障排查速查

| 现象 | 原因 / 处理 |
| --- | --- |
| `Unknown symbol bst_pcie_ep_db_*` | 未进树编；用 `install-into-kernel.sh` |
| `disagrees about version of symbol` | `kernelrelease` ≠ `6.6.64-rt47` |
| 无 `/dev/pci_epf_infer_ctl` | `pci_epf_infer.ko` 未加载成功 |
| `reprogram` 后主机仍无 magic | 未 `echo 1 > ctl`；或仍是 BAR0 旧模块 |
| `BAR0 magic=0x0`，BAR1 也没有 | 对端 EP 未 reprogram；或成对重启后再枚举 |
| `devmem` BAR1 = `0xFFFFFFFF`，但 `infer_rc` 成功 | enable 前读无效；以 dmesg 为准 |
| `remove: No such file or directory` | 设备尚未枚举，可忽略，直接 `rescan` |
| Region 尺寸异常 / 枚举怪异 | 软复位不够 → **两板一起 reboot** |
| `inferlat` open 失败 | 无 `/dev/infer_rc0`，先看 `infer_rc` probe |
| 传输超时 | 对端 EP 未加载 / doorbell 未 setup / eDMA 失败；看两边 dmesg |
| eth2/eth3 `phy_poll_reset failed` | 与 PCIe 无关，可忽略 |

---

## 8. 推荐日常回归脚本（单板视角）

把下面存成 `/userdata/ep_test/bringup_ep.sh` / `bringup_rc.sh` 便于重复测。

**EP（每板先跑）：**

```bash
#!/bin/bash
set -e
cd /userdata/ep_test
insmod ./pci_epf_infer.ko 2>/dev/null || true
test -e /dev/pci_epf_infer_ctl
cd /sys/kernel/config/pci_ep/
mkdir -p functions/pci_epf_infer/func1
echo 0x1ef1 > functions/pci_epf_infer/func1/vendorid
echo 0x0301 > functions/pci_epf_infer/func1/deviceid
ln -sfn functions/pci_epf_infer/func1 controllers/73000000.pcie2_ep/
echo 1 > controllers/73000000.pcie2_ep/start
sleep 1
echo 1 > /dev/pci_epf_infer_ctl
dmesg | grep -E 'ctrl BAR|reprogram done|eDMA' | tail -5
```

**RC（两边 EP ready 后跑）：**

```bash
#!/bin/bash
set -e
rmmod infer_rc 2>/dev/null || true
echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove 2>/dev/null || true
sleep 1
echo 1 > /sys/bus/pci/rescan
setpci -s 0000:00:00.0 CAP_EXP+10.w=0020
sleep 1
lspci -n -s 01:00.0
cd /userdata/ep_test
insmod ./infer_rc.ko
ls /dev/infer_rc0
dmesg | grep infer_rc | tail -5
./inferlat /dev/infer_rc0 w 100
./inferlat /dev/infer_rc0 r 100
```

---

## 9. 已知限制与后续

1. **成对重启**：BST EP 被对端 RC 复位后的软恢复未彻底修好，日常仍建议双板一起 reboot。
2. **DMA 落点**：当前 EP 把数据搬进本地 4MB coherent 缓冲；真正接 NPU 时需把缓冲换成 NPU 可见内存 / IOMMU 映射。
3. **单实例**：`/dev/pci_epf_infer_ctl` 与全局 `g_epf_infer` 目前按单 function 设计。
4. **完成通知**：关键路径用 status 轮询；若以后 MSI 稳定，可作可选加速，但不要作为唯一完成路径。
5. **双链路 EP2**：每板 EP 模块 + 对板 RC 模块已打通；上层推理调度可在此数据面上叠。

---

## 10. 相关文档与提交

- 简要说明：`README.md`
- 编译细节：`BUILD.md`
- 协议头：`infer_proto.h`
- 分支：`cursor/pcie-epf-infer-minlat-888f`
- 关键修复提交：
  - 进树编译 / 6.6 probe 适配
  - post-start reprogram（`/dev/pci_epf_infer_ctl`）
  - eDMA slave + 回调/超时修复
  - **控制寄存器从 BAR0 迁到 BAR1**（主机可见 magic 的根因修复）
