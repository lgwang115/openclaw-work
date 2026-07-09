# 双 AI SoC PCIe Gen4 互联：用 pci_epf_test / pcitest 验证 RC→EP 通信

场景：两块 AI SoC 通过 PCIe Gen4 x4 互联，准备做 EP2 方式推理。

- A 板：`10.28.10.201`，作为 **EP**，controller 为 `73000000.pcie2_ep`，跑 `pci_epf_test`（vendor `0x1ef1`，device `0x0300`）
- B 板：`10.28.10.130`，作为 **RC**，已完成枚举 + retrain 到 16.0 GT/s x4，BAR0/BAR1 各 1MB，MMIO 读写回读验证 OK

本文回答：**枚举成功之后，怎么系统地测试 B→A（以及 A→B）的通信**。

## 1. 测试链路的三个组件

`pci_epf_test` 不是孤立的，它是内核 endpoint 测试框架的 EP 侧一半，完整链路是：

```
B 板 (RC)                                A 板 (EP)
pcitest (用户态工具)
    │ ioctl
pci-endpoint-test (drivers/misc/)  ←──→  pci_epf_test (drivers/pci/endpoint/functions/)
    │ 把命令/地址写进 EP 的 BAR0 寄存器区      │ 轮询命令，发起 memcpy 或 eDMA，回中断
    └────────────── PCIe 链路 ──────────────┘
```

工作方式：RC 侧驱动在自己的内存里分配 buffer，把 buffer 的 PCI 地址和命令写进 EP 的 **BAR0 寄存器区**；EP 侧 `pci_epf_test` 收到命令后**由 EP 主动发起**对 RC 内存的读/写（CPU memcpy 或 DMA 引擎），完成后校验 CRC32 并向 RC 打中断报告结果。

所以三种数据测试对应的方向是：

| pcitest 命令 | EP 执行的动作 | 数据方向 |
| --- | --- | --- |
| `pcitest -w` (WRITE) | EP 从 RC 的 buffer **读**数据并校验 CRC | **B → A** |
| `pcitest -r` (READ) | EP 向 RC 的 buffer **写**数据，RC 校验 CRC | A → B |
| `pcitest -c` (COPY) | EP 从 RC buffer 读、再写回 RC 另一 buffer | B → A → B |

注意命令名是站在 RC 视角的（"我要写出去" = WRITE），别和 EP 侧 dmesg 里打印的 READ/WRITE 搞混——EP dmesg 里的 `READ` 对应 `pcitest -w`。

另外：你们已经做过的"B 板 devmem 写 BAR、回读一致"本身就已经证明了 B→A 的 **MMIO 通路**（写落到 A 板 DDR，读再从 A 板取回来）。下面的测试补齐的是**中断通路**和 **EP 主动 DMA 的大块数据通路**——后者才是推理数据面真正会用的模式。

## 2. B 板（RC）准备

### 2.1 确认/加载 host 侧驱动

```bash
# 确认内核配置（也可看 /boot/config-$(uname -r)）
zcat /proc/config.gz | grep -E 'CONFIG_PCI_ENDPOINT_TEST'
# 期望 =y 或 =m

modprobe pci_endpoint_test   # =m 时
```

### 2.2 绑定你们的自定义 ID（关键一步）

`pci-endpoint-test` 驱动的 id_table 里只有 TI/Cadence/Synopsys 等一批固定 ID，**没有 `1ef1:0300`**，所以枚举后驱动不会自动 probe。用动态 ID 绑上即可：

```bash
echo "1ef1 0300" > /sys/bus/pci/drivers/pci-endpoint-test/new_id
```

验证绑定成功：

```bash
ls /sys/bus/pci/drivers/pci-endpoint-test/
# 应出现 0000:01:00.0
ls /dev/pci-endpoint-test.0
# 字符设备节点出现，后面 pcitest 默认就打开它
```

替代方案：把 A 板 configfs 里的 vendorid/deviceid 改成 id_table 里已有的一组（比如 `0x104c/0xb500`），驱动就能自动 probe，不用 `new_id`。但用自家 vendor ID + `new_id` 更干净，推荐保持现状。

注意：一旦驱动绑定，**不要再用 devmem 裸写 BAR0**——BAR0 是测试协议的寄存器区（magic/command/status/地址寄存器），裸写会干扰测试。

## 3. 获取 pcitest

先看板子 BSP 是不是已经预装了（比如黑芝麻 A2000 的 SDK 自带 `/usr/bst/bin/pcitest`）：

```bash
which pcitest && pcitest -h
```

预装的二进制和板上内核配套，直接用即可；用 `-h` 确认支持 `-d`（DMA）选项（内核 5.7 之后才有）。BSP 里通常也带 `pcitest.sh` 全量脚本。

没有预装时，从**和板子内核版本对应**的源码树编译：

```bash
# 内核 <= 6.12：工具在 tools/pci/
cd <kernel-src>
make -C tools/pci                          # 板上本地编译
make -C tools/pci CROSS_COMPILE=aarch64-linux-gnu-   # 交叉编译
# 产物 tools/pci/pcitest，另有 pcitest.sh 一键全量测试脚本

# 内核 >= 6.13：pcitest 被移除，改成了 kselftest
make -C tools/testing/selftests TARGETS=pci_endpoint
```

把编译产物拷到 B 板即可。

## 4. 测试步骤（在 B 板执行）

建议按这个顺序，每一步通过再进下一步，出问题时定位面小。

### 4.1 BAR 测试

```bash
pcitest -b 0
pcitest -b 1
# BAR Test (bar 0): OKAY / NOT OKAY
```

RC 向该 BAR 写入图案再回读比对。你们只映射了 BAR0/BAR1，测 2~5 报 NOT OKAY 是正常的。

### 4.2 中断测试（先设类型，再测）

`pcitest -i` 设置中断类型（0=INTx legacy，1=MSI，2=MSI-X），**必须先设再测**：

```bash
# Legacy INTx
pcitest -i 0 && pcitest -l

# MSI（你们 EP 配了 32 个）
pcitest -i 1
for n in 1 8 16 32; do pcitest -m $n; done

# MSI-X（配了 256 个）
pcitest -i 2
for n in 1 64 128 256; do pcitest -x $n; done
```

中断测试非常重要：后面的读写测试依赖"EP 完成后打中断"这条通知路径，中断不通则读写测试全部超时失败。

### 4.3 数据搬运测试（B→A 是 `-w`）

```bash
pcitest -i 2                 # 后续完成通知用 MSI-X

# CPU memcpy 路径（EP 用 memcpy_fromio/toio 搬）
pcitest -w -s 1024           # B → A，1KB
pcitest -w -s 1048576        # B → A，1MB
pcitest -r -s 1048576        # A → B
pcitest -c -s 1048576        # 环回 copy

# DMA 路径（EP 用自己的 DMA 引擎搬，推理数据面的真实形态）
pcitest -d -w -s 4194304     # B → A，4MB，DMA
pcitest -d -r -s 4194304
pcitest -d -c -s 4194304
```

每条输出 `OKAY` 表示数据搬完且 **CRC32 校验通过**，即 B→A 数据完整性已验证。

也可以直接跑全量脚本：`pcitest.sh`（依次跑 BAR、中断、多种 size 的读写拷）。

### 4.4 看吞吐：A 板 dmesg

EP 侧 `pci_epf_test` 每次搬运都会打印速率，**在 A 板**看：

```bash
dmesg | tail
# pci_epf_test pci_epf_test.0: READ => Size: 4194304 B, DMA: YES, Time: ..., Rate: ... KB/s
```

参考预期：Gen4 x4 理论线速 ~7.88 GB/s；DMA 大块（≥1MB）通常能到数 GB/s；CPU memcpy 路径只有几十~几百 MB/s，慢是正常的，不代表链路有问题。如果 `-d` 报 NOT OKAY 或速率和非 DMA 一样低，检查 A 板内核是否使能了 EP 的 DMA 引擎驱动（DWC 控制器对应 `CONFIG_DW_EDMA`），以及 `pci_epf_test` probe 时 dmesg 有没有 "Failed to get DMA channel" 之类的提示。

## 5. 结果怎么解读

一次真实调试(BST A2000 双板)的首轮结果和结论,可作对照:

| 结果 | 解读 |
| --- | --- |
| `pcitest -w -s 1048576` OKAY | **B→A 主链路已闭环**:命令写入 BAR0 → EP 读 RC 内存 1MB → CRC 通过 → MSI-X 回中断。这一条过了,核心通信就是通的 |
| `pcitest -b 0` NOT OKAY | BAR0 是协议寄存器区,不同内核版本对它的 BAR 测试行为不一;`-w` 能过说明寄存器区实际可读写,不当阻塞项。注意 `-b 0 && -b 1` 会短路,BAR1 要单独跑。**带 doorbell 功能的内核(EP dmesg 有 `Doorbell info: bar_no=0, offset=...`)BAR0 测试必然失败**:门铃区是硬件寄存器不是内存;且整段图案写会污染 COMMAND 等协议寄存器(EP 报 `Invalid command 0xcafebabe`),这种平台直接跳过 `-b 0` |
| `-m 32` / `-x 256` NOT OKAY | 不是中断不通(`-w` 的完成通知就是中断),是**高号向量**不通,通常是 RC 实际分配的向量数少于 EP 声明值。用 `for i in 1 2 4 8 ...; do pcitest -x $i; done` 扫出边界,配合 `/proc/interrupts` 和 `lspci -vv` 的 MSI-X Count 确认。够用即可 |
| 所有 `-d` NOT OKAY | EP 侧多半没拿到 DMA 通道。查 A 板 dmesg(probe 时 "Failed to get DMA" 类打印、跑 `-d` 时的报错),确认 eDMA 驱动使能。对照:`-w -s 4194304`(不带 `-d` 的大块)排除 buffer 分配问题,`-d -w -s 65536`(小块 DMA)确认是 DMA 本身 |

## 6. 常见问题

- **`/dev/pci-endpoint-test.0` 不出现**：`new_id` 没写、或写在 rescan 之前设备还不存在。绑定后 `lspci -k -s 01:00.0` 应显示 `Kernel driver in use: pci-endpoint-test`。
- **中断测试 NOT OKAY**：优先查 RC 侧 MSI 分配（`cat /proc/interrupts | grep endpoint`）；ARM 平台确认 GIC ITS 可用；INTx 不通但 MSI 通在 EP 控制器上很常见（很多 EP 控制器不支持发 INTx），不阻塞后续。
- **A 板重启/重新 start EP 后测试挂死**：RC 侧残留的是旧设备状态。先 `echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove`，A 板重新初始化 EP 后 B 板再 rescan + retrain + 重新绑定。
- **每次都要 retrain 才到 Gen4**：现阶段可以先用 setpci 顶着，之后查 EP 侧控制器的 target link speed 配置（DT 里 `max-link-speed`）和均衡参数，让它训练时直接上 Gen4。

## 7. 通了之后：面向 EP2 推理的下一步

`pci_epf_test` 只是链路验证工具，跑通它说明**枚举、BAR、MSI/MSI-X、EP 主动 DMA、双向数据完整性**全部就绪。推理数据面有两条路：

1. **快速打通：IP over PCIe。** EP 侧换 `pci_epf_vntb`（RC 侧配 `ntb_transport` + `ntb_netdev`），两板之间出一个虚拟网卡，直接跑 TCP/IP。已有的推理框架 RPC/张量传输代码不用改就能先跑起来，代价是协议栈开销，带宽利用率一般在线速的一半上下。
2. **正式方案：自定义 EPF 驱动（已落地）。** 见仓库目录 **`pcie-epf-infer-minlat/`**：
   - 门铃 IRQ → 直接 submit eDMA + status 轮询（不依赖 MSI）
   - 控制面在 **BAR1**，门铃在 BAR0+`0xe00`
   - v1：`inferlat` 实测 4KB 中位 **~17µs**，2MB ~6 GB/s
   - v2：双缆单向 WRITE + `POST_RECV`/`PUSH` + `MAP_USER`/`MAP_DMABUF`（`ZEROCOPY.md`）
   - 文档入口：`pcie-epf-infer-minlat/README.md`、`BRINGUP.md`、`BUILD.md`、`ZEROCOPY.md`

建议：链路验证继续用本文的 `pcitest`；延迟与零拷贝数据面切到 `pcie-epf-infer-minlat`。
