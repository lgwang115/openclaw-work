# 最低延迟版 EP2 数据面（门铃 + 状态轮询 + 预分配 DMA）

## 设计要点（相对 pci_epf_test）

| 项目 | pci_epf_test | 本方案 |
| --- | --- | --- |
| 命令发现 | 1ms `delayed_work` 轮询；门铃 handler 空转 | 门铃 IRQ → **直接提交 eDMA**（无 workqueue） |
| 完成通知 | MSI/MSI-X（你们链路上会随机失效） | **RC 轮询 status 寄存器**（关键路径不用中断） |
| 缓冲区 | 每次传输分配 + CRC + 随机数 | 启动预分配 4MB，无 CRC；v2 可指定 ADDR/DMABUF |
| 目标 | 功能验证 | 4KB 端到端目标 **20~40µs**（实测中位 **~17µs**） |

协议见 `infer_proto.h` / `infer_proto_v2.h`。设备 ID：`1ef1:0301`。

完整工作记录（设计动机、平台坑、编译部署、板测步骤、实测数据）见 **`BRINGUP.md`**。  
编译与拷板细节见 **`BUILD.md`**。  
零拷贝 NPU 互联（双缆单向推送、指定 NPU 地址）见 **`ZEROCOPY.md`**（驱动已接线并板测；真实 NPU runtime 待联调）。

## BST BAR 布局（必读）

| BAR | 内容 | 主机可见 |
| --- | --- | --- |
| **BAR0** | MSI-X 表 + 门铃（硬件，通常 `+0xe00`） | **不是** DDR；读 offset 0 得到 0 / 垃圾 |
| **BAR1** | 协议寄存器（`infer_regs`，含 magic） | inbound-ATU → EP DDR；**magic 必须在这里** |

旧版把 magic 写在 BAR0+0，EP 本地能看到，主机永远读不到 → `infer_rc` probe 失败 `-22`。

## 文件

- `pci_epf_infer.c` — EP 侧（v1 staging + v2 POST_RECV/PUSH，`/dev/pci_epf_infer_ctl` + `/dev/pci_epf_infer0`）
- `infer_rc.c` — RC 侧（`XFER` + `PUSH` + `MAP_USER`/`MAP_DMABUF`）
- `inferlat.c` — v1 延迟扫表
- `inferpush.c` — v2 staging smoke（POST_RECV STAGING）
- `inferzc.c` — v2 零拷贝接口测试（MAP_USER + DMABUF）
- `infer_dmabuf_test.c` — 测试用连续 dma-buf 导出（`/dev/infer_dmabuf_test`）
- `infer_proto.h` / `infer_proto_v2.h` — 协议
- `install-into-kernel.sh` — 推荐编译入口
- `ZEROCOPY.md` / `BRINGUP.md` / `BUILD.md` — 设计与板测

## 编译

```bash
./install-into-kernel.sh /path/to/linux-6.6
# 产物: pci_epf_infer.ko、infer_rc.ko、infer_dmabuf_test.ko、
#       inferlat、inferpush、inferzc
# 两板都要拷全套（拓扑对称）
```

详见 `BUILD.md`。

## 部署与枚举（两板成对重启后）

**每板先做本板 EP（两端都 ready 后再各自做 RC 枚举）：**

```bash
insmod /userdata/ep_test/pci_epf_infer.ko
ls /dev/pci_epf_infer_ctl   # 必须存在
ls /dev/pci_epf_infer0      # v2 收包

cd /sys/kernel/config/pci_ep/
mkdir -p functions/pci_epf_infer/func1
echo 0x1ef1 > functions/pci_epf_infer/func1/vendorid
echo 0x0301 > functions/pci_epf_infer/func1/deviceid
ln -s functions/pci_epf_infer/func1 controllers/73000000.pcie2_ep/
echo 1 > controllers/73000000.pcie2_ep/start
sleep 1
echo 1 > /dev/pci_epf_infer_ctl    # 必须：start 会清掉 bind 时的 BAR/ATU
dmesg | grep -E 'reprogram|ctrl BAR|eDMA' | tail -5
# 必须: ctrl BAR1 programmed ...
# 必须: eDMA channels: tx=dma1chan0 rx=dma1chan8
# 必须: reprogram done magic=0x494e4652
```

**再在本板做对端 EP 的 RC 枚举：**

```bash
rmmod infer_rc 2>/dev/null
echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove 2>/dev/null || true
sleep 1
echo 1 > /sys/bus/pci/rescan
setpci -s 0000:00:00.0 CAP_EXP+10.w=0020
sleep 1

lspci -n -s 01:00.0
# 期望: 1ef1:0301

cd /userdata/ep_test
insmod ./infer_rc.ko
dmesg | grep infer_rc | tail -5
# 期望: infer regs on BAR1 ... doorbell mapped BAR0+0xe00 ... /dev/infer_rc0
ls /dev/infer_rc0
```

> 不要在 `insmod infer_rc` 之前用 `devmem` 验 magic：设备未 enable Memory 时 BAR1 也常读到 `0xFFFFFFFF`。以 dmesg 为准。

**测延迟（v1）：**

```bash
./inferlat /dev/infer_rc0 w 100
./inferlat /dev/infer_rc0 r 100
# 验收参考: 4KB 中位 ~17µs，2MB ~6 GB/s（见 BRINGUP.md）
```

**测零拷贝接口（v2，可选）：**

```bash
# 接收板
./inferpush ep 0 4096
# 或: insmod infer_dmabuf_test.ko && ./inferzc ep 0 4096

# 发送板
./inferpush rc 0 4096
# 或: ./inferzc rc 0 4096 / ./inferzc rc-dmabuf 0 4096
```

## 关键路径时序

```
RC: status=IDLE → size/pci_addr → command → wmb → writel(BAR0+db_off)
EP: doorbell IRQ → 直接 submit eDMA → DMA callback 写 status=OK（BAR1）
RC: poll BAR1 status until OK  → 返回耗时(ns)
```

单 outstanding（`ctx->busy`）；同缆串行。双缆各自独立。

## 已知限制

1. 成对重启纪律不变（BST EP 软复位恢复未修好前）。
2. v1 `inferlat` 仍走 EP 本地 4MB staging；业务收包用 v2 `POST_RECV`/`WAIT`。
3. 双链路 EP2：每板各装 EP 模块 + 对面板装 RC 模块；数据面应按缆单向 WRITE 推送（见 `ZEROCOPY.md`）。
4. 指定 NPU 地址：RC 可用 `PUSH` / `MAP_USER` / **`MAP_DMABUF`**；EP 可用 `POST_RECV(ADDR|DMABUF)`。
5. `prep_slave_single`：MAP_* / EP DMABUF 要求单连续 DMA 段。
6. 真实 NPU runtime 绑定仍为 P3（当前 smoke 用 staging 或 `infer_dmabuf_test`）。
