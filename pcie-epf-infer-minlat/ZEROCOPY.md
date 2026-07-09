# 零拷贝 NPU 互联协议草案（双缆单向推送）

> 状态：设计草案，驱动尚未实现。  
> 现行可跑通的延迟基线仍是 v1（staging 4MB + `INFER_IOC_XFER`），见 `BRINGUP.md`。  
> 本文件 + `infer_proto_v2.h` 定义目标接口，供评审后再改 `pci_epf_infer` / `infer_rc`。

## 1. 目标

- 双缆 PCIe Gen4 x4：每板既是 RC 又是 EP
- **推理卡互联**：A↔B 传 activation / KV / 中间结果
- **避免 staging 搬运**：eDMA 直接在「发送方 NPU 可见地址」与「接收方 NPU 可见地址」之间搬
- 保持低延迟完成路径：门铃 + status 轮询（不依赖 MSI）

## 2. 拓扑与方向

```
缆1:  A(RC) ──WRITE──► B(EP)     只承载 A→B 数据
缆2:  B(RC) ──WRITE──► A(EP)     只承载 B→A 数据
```

| 方向 | 谁发 ioctl | 谁跑 eDMA | 数据落点 |
| --- | --- | --- | --- |
| A→B | A 的 `/dev/infer_rc0` | **B** 的 EP eDMA | B 本板已注册的 NPU dst |
| B→A | B 的 `/dev/infer_rc0` | **A** 的 EP eDMA | A 本板已注册的 NPU dst |

**不用 READ 做 recv。** READ 会走错链路/错 EP 缓冲（见此前分析）。  
v2 数据面命令以 **WRITE（远端读 → 本地写）** 为主；READ 仅保留给调试/回读。

## 3. 为什么 v1 不够

v1：

```
NPU_src →(可能拷)→ RC staging(buf_dma) → eDMA → EP staging(4MB) →(无接口)→ NPU_dst
```

问题：

1. 两端都钉死驱动私有 staging
2. EP 收包后用户态读不到 EP 本地缓冲
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
| **`local_dst`（本地）** | 接收方 EP | 本板 eDMA 的目的物理/IOVA | **每次** EP `ARM`/`POST` 传入；只存在 EP 内核，不进 BAR |

平台前提（已确认）：**eDMA 可以读写 NPU buffer**，但地址不是固定的——  
每次传输的 src/dst 位置可能不同，必须在发起 DMA 前把**当次**地址告诉 eDMA（`dma_slave_config` / `prep_slave_single`）。

因此协议默认是 **per-transfer 地址编程**，而不是「注册一次用一辈子」：

- 发送方：每次 `PUSH` 都带本次 `pci_addr`（NPU_src 经 `dma_map` 后的总线地址）
- 接收方：每次 `ARM`（或带地址的 `POST`）都带本次 `local_dst`（NPU_dst）
- 驱动在门铃处理里用这两次传入的地址配置 eDMA，打完即丢（或仅缓存到 DONE）

长期 pin/map 可以做（同一块 NPU 缓冲反复用），但是**可选优化**；API 必须允许每包换地址。

发送方要对 **本板 PCIe RC 设备**（`infer_rc` 的 `pdev`）做 `dma_map_*`，得到的才是合法 `pci_addr`。  
接收方 `local_dst` 必须是 **本板 EP eDMA 能写的** NPU/共享 DDR 地址（物理或该 DMA 设备的 IOVA）。

## 5. 槽位模型（双缓冲起步）

每条 EP 链路维护 `INFER_V2_SLOTS`（建议 2）个接收槽，用于流水（一包 DMA 时另一包可先 ARM）：

```
slot[i]:
  local_dst, capacity   ← 每次 ARM 覆盖
  state: EMPTY | POSTED | BUSY | DONE | ERROR
```

- **POSTED/ARMED**：本槽已写入当次 `local_dst`，允许对端 PUSH
- **BUSY**：门铃已响，eDMA 正用该 `local_dst` 写入
- **DONE**：完成；用户态收走后再次 ARM（可换新地址）

发送方在 PUSH 前应看到对端 credit（`posted_mask`），避免覆盖未收完的槽。

## 6. 一次 A→B 推送时序（每包换地址）

```
B (EP / recv) — 本包 NPU_dst 可能与上包不同:
  1. INFER_EP_IOC_ARM(slot, local_dst=NPU_dst, capacity)
       → 驱动记下 slot.local_dst；slot=POSTED；更新 posted_mask

A (RC / send) — 本包 NPU_src 也可能不同:
  2. dma_map(本次 NPU_src) → pci_addr   （或复用已 map 的地址）
  3. 确认 posted_mask 含该 slot
  4. INFER_IOC_PUSH { slot, size, pci_addr, timeout }
       → 写 BAR1；响门铃；轮询 status

B (EP):
  5. 门铃 → workqueue
  6. 校验 slot==POSTED && size<=capacity
  7. eDMA 编程（关键）:
       remote src = regs->pci_addr      // 当次远端 NPU_src
       local  dst = slot.local_dst      // 当次本板 NPU_dst
       len        = size
  8. status=OK；slot=DONE；seq++
  9. WAIT → 用户态/NPU 消费；下一包再 ARM(新地址)
```

对称方向走缆2，角色对调。

## 7. BAR1 寄存器扩展（相对 v1）

v1 `infer_regs` 保留兼容；v2 在其后追加字段（或使用同一结构的 `reserved` 升级），见 `infer_proto_v2.h`：

| 字段 | 谁写 | 含义 |
| --- | --- | --- |
| `magic / command / status / size / pci_addr` | 同 v1 | PUSH 时 command=`INFER_CMD_PUSH` |
| `slot` | RC | 目标接收槽 |
| `seq` | EP | 完成序号 |
| `posted_mask` | EP | bit i = slot i 已 POSTED |
| `done_mask` | EP | bit i = slot i DONE（可选，便于 RC 侧调试） |
| `local_dst` | — | **不放 BAR**（对端不该知道本板物理地址）；每次 ARM 只进 EP 内核 |

门铃仍在 BAR0+`db_offset`。

## 8. ioctl 草案

### 8.1 RC（`/dev/infer_rc0`）— 发送侧

| ioctl | 作用 |
| --- | --- |
| `INFER_IOC_PUSH` | **每次**带 `pci_addr`+`size`+`slot`，门铃+轮询 |
| `INFER_IOC_XFER` | **保留 v1**：驱动 staging，回归延迟 |
| `INFER_IOC_MAP_USER`（可选） | VA → `pci_addr`；可每包 map，或 map 一次多包复用 |
| `INFER_IOC_GET_CREDIT`（可选） | 读对端 `posted_mask` |

运行时若已有 NPU/dma-buf 导出的总线地址，直接 `PUSH`，驱动不保管缓冲。

### 8.2 EP（新 `/dev/pci_epf_infer0`）— 接收侧

| ioctl | 作用 |
| --- | --- |
| `INFER_EP_IOC_ARM` | **每次**带 `local_dst`+`capacity`+`slot` → POSTED（推荐主路径） |
| `INFER_EP_IOC_WAIT` | 等 slot → DONE |
| `INFER_EP_IOC_REG_RECV` | 可选：长期绑定某 slot 的默认 dst（仍允许 ARM 覆盖） |

`local_dst` 来源：

1. **dma-buf fd**（接 NPU runtime，每包或每层换 fd/offset 均可）
2. 显式 IOVA/物理地址（调试 / 已翻译好的 NPU 地址）
3. staging（兼容，非零拷贝）

eDMA 编程点（实现时务必）：在门铃 work 里用 **本包** 的 `regs->pci_addr` 与 **本包** 的 `slot.local_dst`，不要用模块加载时分配的固定 `ctx->buf_dma`（除非 ARM 显式选了 staging）。

## 9. 平台验证（能力已确认，剩联调）

已确认：**eDMA ↔ NPU buffer 可读可写**。实现时重点变成：

1. **每包把地址喂给 eDMA**（slave config + prep），换地址不重启通道即可
2. RC 侧 `dma_map(NPU_src)` 得到的 `pci_addr` 对端能否读到（图案校验）
3. 对齐 / 最大段长；不对齐则头尾补齐
4. 双 slot：一包 BUSY 时另一包可先 ARM 新地址，避免气泡

## 10. 分阶段落地

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| P0 | 文档 + `infer_proto_v2.h`（本阶段） | 评审通过 |
| P1 | EP：`ARM`/`WAIT`（先允许 staging 当地址源）+ 门铃里用 slot.local_dst | recv 通 |
| P2 | RC：`PUSH` 接受外部 `pci_addr` | 发送端零拷贝 |
| P3 | ARM 传入真实 NPU/dma-buf 地址；双 slot | 端到端每包换地址零拷贝 |
| P4 | 推理 runtime 绑定 | 业务路径 |

## 11. 明确不做的事（本草案）

- 不靠 READ 实现跨板 recv
- 不把 NPU 私有地址不经 map 直接塞进 `pci_addr`
- 不在关键路径依赖 MSI
- 不在一条缆上同时跑双向大数据（credit/门铃可以双向，数据面保持单向）

## 12. 与 v1 的关系

- v1 模块与 `inferlat` **保持可用**（延迟基线）
- v2 用新 command / 新 ioctl 号；`INFER_IOC_XFER` 不删除
- 设备 ID 可暂仍 `1ef1:0301`；若需并行加载两套协议，再议 `0302`
