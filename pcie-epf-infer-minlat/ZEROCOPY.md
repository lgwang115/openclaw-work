# 零拷贝 NPU 互联协议（双缆单向推送）

> 状态：**驱动已接线并板测通过**（P1/P2）；真实 NPU runtime 联调为 P3。  
> v1 延迟基线（staging + `INFER_IOC_XFER`，门铃 IRQ→eDMA 直提）：4KB 中位 **~17µs**，见 `BRINGUP.md`。  
> 本文件 + `infer_proto_v2.h` 描述接口与拓扑；实现见 `pci_epf_infer.c` / `infer_rc.c`。  
> 工具：`inferpush`（staging smoke）、`inferzc`（MAP_USER / MAP_DMABUF）、`infer_dmabuf_test.ko`。

## 1. 目标

- 双缆 PCIe Gen4 x4：每板既是 RC 又是 EP
- **推理卡互联**：A↔B 传 activation / KV / 中间结果
- **避免 staging 搬运**：eDMA 直接在「发送方 NPU 可见地址」与「接收方 NPU 可见地址」之间搬
- 保持低延迟完成路径：门铃 IRQ → **直接 submit eDMA** → DMA callback 写 status（不依赖 MSI）

## 2. 拓扑与方向

```
缆1:  A(RC) ──WRITE──► B(EP)     只承载 A→B 数据
缆2:  B(RC) ──WRITE──► A(EP)     只承载 B→A 数据
```

| 方向 | 谁发 ioctl | 谁跑 eDMA | 数据落点 |
| --- | --- | --- | --- |
| A→B | A 的 `/dev/infer_rc0` | **B** 的 EP eDMA | B 本板已注册的 NPU dst |
| B→A | B 的 `/dev/infer_rc0` | **A** 的 EP eDMA | A 本板已注册的 NPU dst |

**不用 READ 做 recv。** READ 会走错链路/错 EP 缓冲。  
v2 数据面命令以 **WRITE（远端读 → 本地写）** 为主；READ 仅保留给调试/回读（`inferlat r`）。

同缆 **单 outstanding**（EP `ctx->busy`）；多线程同缆需用户态串行化。双缆各自独立。

**MoE / all-to-all：** 必须先 `POST_RECV` 再 `PUSH`（`post_recv` → `send` → `wait_recv`）。  
双边同时阻塞在等 credit 的 `send()` 会 5s 死锁。详见 **`EP2_COMM.md`**。

## 3. 为什么 v1 不够

v1：

```
NPU_src →(可能拷)→ RC staging(buf_dma) → eDMA → EP staging(4MB) →(无接口)→ NPU_dst
```

问题：

1. 两端都钉死驱动私有 staging
2. EP 收包后用户态读不到 EP 本地缓冲（v1 无 `/dev/pci_epf_infer0` 收包接口）
3. 对端 `infer_rc` 连的是另一条缆的 EP，拉不回这份数据

v2 目标：

```
NPU_src (PCI 总线地址 pci_addr)
        ── B EP eDMA ──►
NPU_dst (B 本板 eDMA 本地地址)
```

## 4. 地址两类（必须分清）

| 名字 | 谁提供 | 语义 | 填到哪 |
| --- | --- | --- | --- |
| **`pci_addr`（远端）** | 发送方 RC | 对端 EP 通过 PCI 能读到的地址 | BAR1 `pci_addr` / **每次** `INFER_IOC_PUSH.pci_addr` |
| **`local_dst`（本地）** | 接收方 EP | 本板 eDMA 的目的物理/IOVA | **每次** EP `POST_RECV` 传入；只存在 EP 内核，不进 BAR |

平台前提（已确认）：**eDMA 可以读写 NPU buffer**，但地址不是固定的——  
每次传输的 src/dst 位置可能不同，必须在发起 DMA 前把**当次**地址告诉 eDMA（`dma_slave_config` / `prep_slave_single`）。

因此协议默认是 **per-transfer 地址编程**：

- 发送方：每次 `PUSH` 都带本次 `pci_addr`（经 `dma_map` / `MAP_USER` / `MAP_DMABUF`）
- 接收方：每次 `POST_RECV` 都带本次 `local_dst`（ADDR / DMABUF / STAGING）
- 门铃 IRQ 里用这两次传入的地址配置 eDMA，打完即丢（或仅缓存到 DONE）

发送方要对 **本板 PCIe RC 设备**（`infer_rc` 的 `pdev`）做 `dma_map_*`。  
接收方 `local_dst` 必须是 **本板 EP eDMA 能写的** 地址。

限制：当前用 `prep_slave_single`，**MAP_USER / MAP_DMABUF / EP DMABUF 都要求映射结果为单个连续 DMA 段**；多段 SG 返回 `-EINVAL`。

## 5. 槽位模型（双缓冲起步）

每条 EP 链路维护 `INFER_V2_SLOTS`（建议 2）个接收槽：

```
slot[i]:
  local_dst, capacity   ← 每次 POST_RECV 覆盖
  state: EMPTY | POSTED | BUSY | DONE | ERROR
```

- **POSTED**：本槽已写入当次 `local_dst`，允许对端 PUSH
- **BUSY**：门铃已响，eDMA 正用该 `local_dst` 写入
- **DONE**：完成；用户态收走后再次 POST_RECV（可换新地址）

发送方在 PUSH 前应看到对端 credit（`INFER_IOC_GET_CREDIT` → `posted_mask`）。

## 6. 一次 A→B 推送时序（每包换地址）

```
B (EP / recv) — 本包 NPU_dst 可能与上包不同:
  1. INFER_EP_IOC_POST_RECV(slot, local_dst=NPU_dst, capacity)
       → 驱动记下 slot.local_dst；slot=POSTED；更新 posted_mask

A (RC / send) — 本包 NPU_src 也可能不同:
  2. dma_map / MAP_USER / MAP_DMABUF → pci_addr
  3. 确认 posted_mask 含该 slot（GET_CREDIT）
  4. INFER_IOC_PUSH { slot, size, pci_addr, timeout }
       → 写 BAR1；响门铃；轮询 status

B (EP):
  5. 门铃 IRQ → 直接 submit eDMA（无 workqueue；ctx->busy 单 outstanding）
  6. 校验 slot==POSTED && size<=capacity
  7. eDMA 编程（关键）:
       remote src = regs->pci_addr      // 当次远端 NPU_src
       local  dst = slot.local_dst      // 当次本板 NPU_dst
       len        = size
  8. DMA callback: status=OK；slot=DONE；seq++
  9. WAIT → 用户态/NPU 消费；下一包再 POST_RECV(新地址)
```

对称方向走缆2，角色对调。

## 7. BAR1 寄存器扩展（相对 v1）

v1 `infer_regs` 保留兼容；v2 字段见 `infer_proto_v2.h`：

| 字段 | 谁写 | 含义 |
| --- | --- | --- |
| `magic / command / status / size / pci_addr` | 同 v1 | PUSH 时 command=`INFER_CMD_PUSH` |
| `slot` | RC | 目标接收槽 |
| `seq` | EP | 完成序号 |
| `posted_mask` | EP | bit i = slot i 已 POSTED |
| `done_mask` | EP | bit i = slot i DONE |
| `local_dst` | — | **不放 BAR**；每次 POST_RECV 只进 EP 内核 |

门铃仍在 BAR0+`db_offset`（通常 `0xe00`）。

## 8. 已实现 ioctl

### 8.1 RC（`/dev/infer_rc0`）— 发送侧

| ioctl | 状态 | 作用 |
| --- | --- | --- |
| `INFER_IOC_PUSH` | **已实现** | **每次**带 `pci_addr`+`size`+`slot`，门铃+轮询 |
| `INFER_IOC_XFER` | **已实现** | **保留 v1**：驱动 staging，`inferlat` 回归延迟 |
| `INFER_IOC_MAP_USER` / `UNMAP_USER` | **已实现** | VA → `pci_addr`（要求 pin+map 成单段） |
| `INFER_IOC_MAP_DMABUF` / `UNMAP_DMABUF` | **已实现** | dma-buf fd → `pci_addr`（单段） |
| `INFER_IOC_GET_CREDIT` | **已实现** | 读对端 `posted_mask` / `done_mask` / `seq` |

运行时若已有 NPU 导出的总线地址，可直接 `PUSH`，驱动不保管缓冲。

### 8.2 EP（`/dev/pci_epf_infer0`）— 接收侧

| ioctl | 状态 | 作用 |
| --- | --- | --- |
| `INFER_EP_IOC_POST_RECV` | **已实现** | **每次**带 dst（ADDR / STAGING / **DMABUF**）→ POSTED |
| `INFER_EP_IOC_WAIT` | **已实现** | 等 slot → DONE（默认最长 60s） |
| `INFER_EP_IOC_GET_INFO` | **已实现** | 查询 slot / credit / staging 信息 |
| `INFER_EP_IOC_ARM` | 别名 | 兼容旧名，等同 `POST_RECV` |

`POST_RECV` 的 `flags`：

| flag | 含义 |
| --- | --- |
| `INFER_EP_REG_F_STAGING` | 写进 EP 预分配 4MB（smoke / 非零拷贝） |
| `INFER_EP_REG_F_ADDR` | 显式 `local_dst` |
| `INFER_EP_REG_F_DMABUF` | dma-buf fd + offset → local_dst |

eDMA 编程点：门铃 IRQ 里用 **本包** `regs->pci_addr` 与 **本包** `slot.local_dst`，不要默认用固定 `ctx->buf_dma`（除非选了 staging）。

## 9. 平台验证（能力已确认，剩 NPU 联调）

已确认：

1. **eDMA ↔ NPU buffer 可读可写**（平台侧）
2. 每包把地址喂给 eDMA（slave config + prep）已接线
3. 测试用连续 dma-buf（`infer_dmabuf_test.ko`）两端对称通路已板测通过
4. 双 slot 协议字段已有；当前关键路径仍单 outstanding（`ctx->busy`）

P3 重点：把真实 NPU runtime 的 dma-buf fd / IOVA 接到 `POST_RECV` / `MAP_DMABUF`。

## 10. 分阶段落地

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| P0 | 文档 + `infer_proto_v2.h` | 完成 |
| **P1/P2（已接线+板测）** | EP：`POST_RECV`/`WAIT`（ADDR/STAGING/**DMABUF**）；RC：`PUSH` + **`MAP_USER`/`MAP_DMABUF`** + `GET_CREDIT`；IRQ→eDMA | `inferpush` / `inferzc` 通过；`inferlat` 4KB ~17µs |
| P3 | 真实 NPU/dma-buf 端到端零拷贝 | payload 进 NPU buffer |
| P4 | 推理 runtime 绑定 | 业务路径 |

### MAP_USER / DMABUF 用法（runtime）

```c
/* RC: userspace VA → pci_addr（要求物理连续且 dma_map 成单段） */
struct infer_map_req m = { .user_ptr = (uintptr_t)buf, .size = n };
ioctl(rc_fd, INFER_IOC_MAP_USER, &m);

/* RC: dma-buf fd → pci_addr（同样要求单 SG 段） */
struct infer_map_dmabuf d = { .dmabuf_fd = fd, .dmabuf_offset = 0, .size = n };
ioctl(rc_fd, INFER_IOC_MAP_DMABUF, &d);

struct infer_push p = { .slot = 0, .size = n, .pci_addr = m.pci_addr /* or d.pci_addr */, ... };
ioctl(rc_fd, INFER_IOC_PUSH, &p);
ioctl(rc_fd, INFER_IOC_UNMAP_USER, &m.pci_addr);   /* or UNMAP_DMABUF */

/* EP: dma-buf fd → local_dst（要求 map 后单 SG 段） */
struct infer_ep_recv_reg r = {
  .slot = 0,
  .flags = INFER_EP_REG_F_DMABUF,
  .dmabuf_fd = fd,
  .dmabuf_offset = 0,
  .capacity = n,
};
ioctl(ep_fd, INFER_EP_IOC_POST_RECV, &r);
ioctl(ep_fd, INFER_EP_IOC_WAIT, &w);
```

注意：`inferzc rc` 的用户缓冲必须是 **匿名可 pin 页**（不要 mmap RC staging `VM_PFNMAP`，会 `Bad address`）。连续缓冲优先用 `rc-dmabuf`。

### 板测 v2 smoke（EP staging）

成对重启并完成 EP reprogram + RC `insmod` 后：

```bash
# 接收板（本板 EP）
./inferpush ep 0 4096          # POST_RECV staging，阻塞 WAIT（最长 60s）
# 发送板
./inferpush rc 0 4096          # PUSH；EP 应打印 payload OK
```

期望：EP `WAIT result=0` 且 **`payload OK`**；RC `PUSH result=0`（单次冷启动约 ~42µs）。

### 板测 v2 零拷贝接口（MAP_USER + DMABUF）

```bash
# 接收板
insmod /userdata/ep_test/infer_dmabuf_test.ko
./inferzc ep 0 4096
# 期望: POST_RECV DMABUF ... WAIT result=0 ... payload OK via DMABUF

# 发送板（60s 内）— MAP_USER
./inferzc rc 0 4096
# 期望: MAP_USER ... -> pci_addr=... ; PUSH result=0

# 或对称 dma-buf
insmod /userdata/ep_test/infer_dmabuf_test.ko
./inferzc rc-dmabuf 0 4096
# 期望: MAP_DMABUF -> pci_addr=... ; PUSH result=0
```

### 板测实测摘要（2026-07）

| 路径 | 结果 | 说明 |
| --- | --- | --- |
| `inferlat` WRITE/READ | 4KB 中位 ~17µs；2MB ~6.0–6.1 GB/s | v1 staging，预热扫表 |
| `inferpush rc` | `result=0`，~42.6µs | staging PUSH，单次冷启动 |
| `inferzc rc` | `result=0`，~42.6µs | MAP_USER |
| `inferzc rc-dmabuf` | `result=0`，~43.2µs | MAP_DMABUF |

完整 `inferlat` 表见 `BRINGUP.md` §6。

## 11. 明确不做的事

- 不靠 READ 实现跨板 recv
- 不把 NPU 私有地址不经 map 直接塞进 `pci_addr`
- 不在关键路径依赖 MSI
- 不在一条缆上同时跑双向大数据（credit/门铃可以双向，数据面保持单向）

## 12. 与 v1 的关系

- v1 模块与 `inferlat` **保持可用**（延迟基线；IRQ→eDMA 后 4KB ~17µs）
- v2 用新 command / 新 ioctl；`INFER_IOC_XFER` 不删除
- 设备 ID 暂仍 `1ef1:0301`；若需并行加载两套协议，再议 `0302`
