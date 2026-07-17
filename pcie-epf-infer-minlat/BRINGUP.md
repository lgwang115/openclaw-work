# PCIe 最低延迟 EP2 数据面：从内核模块到板测全记录

> 平台：黑芝麻 A2000（双板互连，PCIe Gen4 x4）  
> 内核：`6.6.64-rt47`  
> 设备 ID：`1ef1:0301`  
> 代码目录：`pcie-epf-infer-minlat/`  
> 实测结果（IRQ→eDMA 直提后）：4KB 中位约 **17µs**，2MB 带宽约 **6.0～6.1 GB/s**  
> （此前门铃→workqueue 路径约 24µs / 6.2 GB/s，见下文对照表）

本文记录从「自研内核模块」开始到「板子上跑通 `inferlat` / v2 零拷贝接口」的全部工作：设计动机、平台坑、编译部署、板测步骤、验收标准与实测数据。

---

## 1. 背景与目标

两块 A2000 通过 PCIe Gen4 x4 双线缆互连，每板同时是一端的 EP、另一端的 RC（对称拓扑）。上层要做 EP2 风格推理数据面，需要：

1. 可靠的 RC↔EP 内存搬运（DMA）
2. **端到端延迟尽量低**（尤其是 4KB 小包）
3. 可指定 NPU 可见地址的零拷贝推送（v2）

前期用 BSP 自带的 `pci_epf_test` + `pcitest` 验证了链路与 DMA 吞吐（约 6.5 GB/s），但用其做延迟测量（`dmalat`）得到 **2～5 ms**，远高于链路能力。根因是框架开销，不是物理链路：

| 开销来源 | 说明 |
| --- | --- |
| EP 侧 `delayed_work` | 门铃 IRQ 只打印，真正处理命令仍按 **1ms** 轮询 |
| 主机侧重试 / CRC | `pci_endpoint_test` 路径带校验与多次往返 |
| MSI 不稳定 | 本平台 MSI 偶发失效，不能当关键路径完成通知 |

因此自研一套 **门铃触发 + 状态寄存器轮询 + 预分配 DMA** 的最小延迟栈，目标：

- 4KB 端到端：**20～40µs**（IRQ→eDMA 直提后实测中位 **~17µs**）
- 大包带宽：接近 `pcitest` DMA 峰值（~6 GB/s）
- 关键路径 **不依赖 MSI**
- v2：双缆单向 WRITE + per-transfer ADDR/DMABUF（见 `ZEROCOPY.md`）

---

## 2. 软件架构

### 2.1 组件

| 文件 | 角色 | 装在哪 |
| --- | --- | --- |
| `pci_epf_infer.c` | EP function（门铃 IRQ→eDMA；v1 staging + v2 POST_RECV） | 每板 EP 口（`73000000.pcie2_ep`） |
| `infer_rc.c` | RC 主机驱动（`XFER`/`PUSH`/`MAP_*`） | 每板 RC 口（`0000:01:00.0`） |
| `infer_proto.h` / `infer_proto_v2.h` | 共享协议 | 两边模块 + 用户态 |
| `inferlat.c` | v1 延迟扫表（4KB→2MB） | RC 侧用户态 |
| `inferpush.c` | v2 staging smoke | 两端用户态 |
| `inferzc.c` | v2 MAP_USER / MAP_DMABUF smoke | 两端用户态 |
| `infer_dmabuf_test.c` | 测试用连续 dma-buf 导出 | 两端（可选） |
| `install-into-kernel.sh` | 拷进内核树并编 `.ko` + 用户态工具 | 开发机 |

设备 ID 故意用 **`1ef1:0301`**（不用 `0300`），避免和 `pci_epf_test` / `pci-endpoint-test` 冲突。

### 2.2 关键路径时序

```
RC (infer_rc / inferlat)
  1. 写 BAR1: status=IDLE, size, pci_addr, command
  2. wmb
  3. writel(msg) → BAR0 + 0xe00   ← 门铃
  4. 轮询 BAR1 status，直到 OK / FAIL

EP (pci_epf_infer)
  1. 门铃 IRQ → 直接 submit eDMA（无 workqueue）← 单 outstanding via ctx->busy
  2. 读 command/size/pci_addr，清 command，置 BUSY
  3. eDMA slave：RC↔EP 本地 4MB 预分配缓冲（v2 可为 POST_RECV 目标）
  4. DMA callback 写 status=OK/FAIL
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

### 3.7 门铃 IRQ 直接提交 eDMA（延迟优化）

早期路径：门铃 IRQ → `queue_work(system_highpri_wq)` → worker 里 submit eDMA。  
当前路径：门铃 IRQ **直接** `prep` + `submit` eDMA，DMA callback 写 status；用 `ctx->busy` 保证单 outstanding。  
板测：4KB 中位从 ~24µs 降到 **~17µs**（约再降 7µs）。

### 3.8 成对重启纪律

软 `remove` + `rescan` **不能**可靠恢复对端 EP 被 RC 复位后的 BAR 尺寸掩码（曾出现 BAR1 变成 8MB 等异常）。  
**两板一起 reboot**，再按「先两边 EP ready，再各自 RC 枚举」的顺序操作。

### 3.9 其它操作注意

- 不要跑 `pcitest -c`（会挂）
- 不要对 BAR0 做普通 `-b 0` 功能测试（门铃/MSI-X 表区域）
- MSI 在本链路偶发失效；本方案关键路径用 **status 轮询**，不依赖 MSI
- `busybox devmem` 在设备 **未 enable Memory Space** 前读 BAR 会得到 `0xFFFFFFFF`；以 `insmod infer_rc` 之后的 dmesg 为准
- `MAP_USER` 不要 mmap RC staging（`VM_PFNMAP` → `Bad address`）；用匿名页或 `rc-dmabuf`
- dma-buf 模块需 `MODULE_IMPORT_NS(DMA_BUF)`

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
~/workspace/linux-6.6/drivers/misc/infer_dmabuf_test.ko
./inferlat ./inferpush ./inferzc
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
      $KDIR/drivers/misc/infer_dmabuf_test.ko \
      $SRC/inferlat $SRC/inferpush $SRC/inferzc \
      root@$IP:/userdata/ep_test/
done

# 建议核对 md5，避免板子上还是旧 ko
md5sum $KDIR/drivers/pci/endpoint/functions/pci_epf_infer.ko \
       $KDIR/drivers/misc/infer_rc.ko \
       $KDIR/drivers/misc/infer_dmabuf_test.ko
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
| 4KB 中位延迟 | 约 **15～25µs**（IRQ 直提实测 ~17µs） |
| 2MB 带宽 | 约 **6 GB/s** 量级 |
| w / r | 延迟大致对称 |
| v2 smoke | `inferpush` / `inferzc` PUSH 成功（单次 ~42µs，冷启动） |
| `/dev/pci_epf_infer0` | EP `insmod` 后存在（v2 收包） |

### 5.5 v2 零拷贝 smoke（可选）

成对 EP/RC ready 后：

```bash
# 接收板
cd /userdata/ep_test
./inferpush ep 0 4096
# 或: insmod ./infer_dmabuf_test.ko && ./inferzc ep 0 4096

# 发送板（60s 内）
./inferpush rc 0 4096
# 或: ./inferzc rc 0 4096
# 或: insmod ./infer_dmabuf_test.ko && ./inferzc rc-dmabuf 0 4096
```

期望：EP `WAIT result=0` + `payload OK`；RC `PUSH result=0`。细节见 `ZEROCOPY.md`。

---

## 6. 实测数据（A 板，2026-07）

环境：A 板作 RC 测对端（B）EP；`inferlat` 每档 100 次。  
当前路径：**门铃 IRQ → 直接 submit eDMA**（无 `system_highpri_wq`）。

### WRITE（RC→EP）— IRQ 直提

| 块大小 | 最小(µs) | 中位(µs) | 平均(µs) | 99%(µs) | 最大(µs) | 带宽(MB/s) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 4KB | 16.4 | 17.5 | 18.3 | 56.5 | 56.5 | 224 |
| 8KB | 16.4 | 17.5 | 17.4 | 35.0 | 35.0 | 447 |
| 16KB | 15.8 | 17.5 | 17.5 | 40.9 | 40.9 | 891 |
| 32KB | 16.7 | 18.7 | 19.1 | 56.9 | 56.9 | 1674 |
| 64KB | 20.9 | 22.0 | 22.9 | 82.2 | 82.2 | 2842 |
| 128KB | 30.9 | 31.3 | 31.8 | 46.5 | 46.5 | 3992 |
| 256KB | 50.5 | 51.5 | 51.4 | 53.6 | 53.6 | 4858 |
| 512KB | 90.3 | 91.2 | 91.3 | 98.5 | 98.5 | 5485 |
| 1MB | 169.6 | 171.0 | 171.1 | 178.6 | 178.6 | 5848 |
| 2MB | 328.7 | 329.8 | 329.9 | 332.7 | 332.7 | 6064 |

### READ（EP→RC）— IRQ 直提

| 块大小 | 最小(µs) | 中位(µs) | 平均(µs) | 99%(µs) | 最大(µs) | 带宽(MB/s) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 4KB | 16.7 | 17.1 | 18.1 | 56.3 | 56.3 | 228 |
| 8KB | 16.2 | 17.2 | 17.9 | 56.4 | 56.4 | 455 |
| 16KB | 16.4 | 17.1 | 17.3 | 22.8 | 22.8 | 912 |
| 32KB | 16.8 | 17.2 | 18.2 | 85.2 | 85.2 | 1822 |
| 64KB | 21.1 | 21.8 | 22.0 | 29.5 | 29.5 | 2868 |
| 128KB | 31.1 | 32.0 | 32.0 | 44.5 | 44.5 | 3909 |
| 256KB | 50.9 | 52.0 | 52.2 | 72.8 | 72.8 | 4803 |
| 512KB | 90.9 | 91.9 | 92.1 | 96.4 | 96.4 | 5440 |
| 1MB | 170.9 | 171.9 | 172.3 | 199.3 | 199.3 | 5818 |
| 2MB | 331.1 | 332.4 | 333.1 | 363.3 | 363.3 | 6017 |

相对 `pci_epf_test` 路径的 2～5ms，4KB 延迟约改善 **100×+**。小包地板约 **16～18µs**（门铃 IRQ + eDMA 提交 + status 轮询）；大包受链路吞吐限制，约 6.0～6.1 GB/s。

### eDMA 自写完成标志(hw_done),绕开完成中断/回调

HDMA native 是 linked-list 模式。WRITE(EP 读 RC → 本地,DEV_TO_MEM)时,EP 把传输编成 **2 元素 LL**:

```
元素0: 数据      远端 pci_addr      → 本地 buf/local_dst   len=size
元素1: 完成标志  远端 pci_addr+size → 本地 regs->hw_done    len=4
```

dw-edma 的远端地址逐元素累加(见 `dw_edma_device_transfer`),所以元素1 从 RC 内存 `pci_addr+size` 读出 RC 预置的 `INFER_HW_DONE_MAGIC`,写进 EP 的 `regs->hw_done`。**RC 轮询 `hw_done` 即完成——数据一落地就置位,不等 EP 完成中断→tasklet→回调那 ~10µs。** EP 回调仍会跑(写 `status=OK` 作兜底 + slot 记账),但已不在 RC 关键路径上。

- RC 侧:发送前把标志写到 `buf[size]`、`hw_done=0`、`xfer_flags=INFER_XF_HWDONE`,轮询 `hw_done==MAGIC`(`status` 兜底)。要求 staging 缓冲有 4 字节尾部余量(`size+4 ≤ buf_size`)。
- 目前接线在 **v1 WRITE(`inferlat w`)**;READ(MEM_TO_DEV)和 PUSH 暂走原 `status` 回调路径。
- 前提:host 无需写 HDMA 寄存器(只有 EP 的 eDMA 在写),规避了 A2000 上 host 不能写 HDMA 的限制。

### 对照：门铃 → workqueue 路径（优化前）

4KB 中位约 **24µs**，2MB 约 **6.2 GB/s**。去掉 workqueue 后小包约再降 **7µs**。

### 拆解延迟：EP 侧 eDMA 计时（`inferdmastat`）

EP 驱动累积每次 eDMA 的 **submit→完成回调** 时间（零逐包 `printk`），用来看 ~17µs 里 DMA 占多少：

```bash
# EP 板
./inferdmastat reset
# RC 板：跑 inferlat / inferpush / test_pcie_comm_ep2
./inferlat /dev/infer_rc0 w 100
# EP 板
./inferdmastat
# eDMA submit->cb: n=... min=.. avg=.. max=.. last=..  ~avg_rate=.. GB/s
```

或加载时开周期性 dmesg 汇总：`insmod pci_epf_infer.ko dma_log_every=1000`。

**零拷贝 4K dma-buf 计时**（`inferzc` 带 `iters`，两板同 N）：

```bash
# EP 板
insmod ./infer_dmabuf_test.ko
./inferdmastat reset
./inferzc ep 0 4096 1000            # POST_RECV/WAIT dma-buf ×1000
# RC 板（EP 起来后）
insmod ./infer_dmabuf_test.ko
./inferzc rc-dmabuf 0 4096 1000     # MAP_DMABUF + PUSH ×1000
# RC 侧结尾自动打印每次总用时统计：
#   RC total(doorbell→status): n=1000 size=4096B min=.. median=.. avg=.. p99=.. max=.. µs (~.. GB/s)
# EP 板
./inferdmastat                       # 4K dma-buf 的 min/avg/max eDMA 时间
```

两处口径：`inferzc rc-dmabuf` 的 `RC total` 是**每次端到端**（门铃→status，含 eDMA + 门铃 + 轮询）；  
`inferdmastat` 是同一批传输里 **EP eDMA submit→回调** 那段。两者相减≈门铃 IRQ + status 开销。  
`iters ≤ 20` 时还会逐次打印 `iter i: total .. µs`。

`inferdmastat` 现在打印 **EP 内部分段**（同一 EP 时钟,准确）:

```
EP-internal breakdown  n=1000  size=4096 B/xfer
  prologue (parse+lock):       min=.. avg=.. max=.. µs   # handler 入口→dma_submit 入口
  setup (slave_cfg+prep):      min=.. avg=.. max=.. µs   # slave_config + prep + submit
  xfer (issue->cb):            min=.. avg=.. max=.. µs   # eDMA 搬运 + 完成回调派发
  EP total (handler->cb):      min=.. avg=.. max=.. µs   # EP 内部总时长
```

口径说明:

- `setup`（slave_config + prep_slave_single）就是"5µs gap"里那段,**现在可精确读**。
- **`门铃 → handler`（IRQ 延迟)测不到**:起点在 RC(写门铃)、终点在 EP(handler 入口),两颗芯片时钟不同源。
  - 合并桶:`RC 端到端(inferzc RC total) − EP total = 门铃传播 + IRQ 延迟 + status 回传 + RC 轮询`。
  - 纯 IRQ 延迟用 EP 本地 **ftrace**:
    ```bash
    cd /sys/kernel/debug/tracing
    echo 1 > events/irq/irq_handler_entry/enable
    # 或用 RT 的 irqsoff / hwlat tracer 看中断关闭/硬件延迟分布
    ```

### v2 零拷贝 smoke（单次冷启动，非中位）

| 路径 | 结果 | 单次 latency |
| --- | --- | --- |
| `inferpush rc`（staging） | `result=0` | ~42.6µs |
| `inferzc rc`（MAP_USER） | `result=0` | ~42.6µs |
| `inferzc rc-dmabuf` | `result=0` | ~43.2µs |

单次高于 `inferlat` 中位属正常（冷路径 / 无预热）；功能上 MAP_USER / MAP_DMABUF / EP DMABUF 均已通过。

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
| `MAP_USER` → `Bad address` | 不要 mmap RC staging（`VM_PFNMAP`）；用匿名页或 `rc-dmabuf` |
| MAP_* / EP DMABUF `-EINVAL` | 映射不是单连续 SG 段 |
| `inferpush` EP WAIT 超时 | RC 未在 60s 内 PUSH；先起 EP 再起 RC |
| modpost `DMA_BUF` namespace | 模块缺 `MODULE_IMPORT_NS(DMA_BUF)` |

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
2. **v1 DMA 落点**：`inferlat` 仍走 EP 本地 4MB staging；业务收包用 v2 `POST_RECV`/`WAIT`（ADDR/DMABUF）。
3. **单 outstanding**：同缆 `ctx->busy` 串行；双缆独立。多线程同缆需用户态锁/队列。
4. **单 SG 段**：`prep_slave_single` 要求 MAP_USER / MAP_DMABUF / EP DMABUF 映射为单连续段。
5. **单实例**：`/dev/pci_epf_infer_ctl` 与全局 `g_epf_infer` 目前按单 function 设计。
6. **完成通知**：关键路径用 status 轮询；若以后 MSI 稳定，可作可选加速，但不要作为唯一完成路径。
7. **P3 待做**：真实 NPU runtime 的 dma-buf / IOVA 接入（当前 smoke 用 staging 或 `infer_dmabuf_test`）。

---

## 10. 相关文档与提交

- 简要说明：`README.md`
- 编译细节：`BUILD.md`
- 零拷贝协议与板测：`ZEROCOPY.md`
- 协议头：`infer_proto.h` / `infer_proto_v2.h`
- 前期链路验证（`pci_epf_test`）：仓库根目录 `pcie-ep-rc-pci-epf-test-communication.md`
- 分支：`cursor/pcie-epf-infer-minlat-888f`（PR #4）
- 关键修复 / 演进：
  - 进树编译 / 6.6 probe 适配
  - post-start reprogram（`/dev/pci_epf_infer_ctl`）
  - eDMA slave + 回调/超时修复
  - **控制寄存器从 BAR0 迁到 BAR1**（主机可见 magic 的根因修复）
  - v2：`POST_RECV`/`PUSH`/`MAP_USER`/`MAP_DMABUF` + `inferpush`/`inferzc`
  - **门铃 IRQ 直接 submit eDMA**（4KB ~24µs → ~17µs）
