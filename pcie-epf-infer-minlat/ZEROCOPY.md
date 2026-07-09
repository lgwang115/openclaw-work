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
| **`pci_addr`（远端）** | 发送方 RC | 对端 EP 通过 PCI 能读到的地址 | BAR1 `pci_addr` / `INFER_IOC_PUSH.pci_addr` |
| **`local_dst`（本地）** | 接收方 EP | 本板 eDMA 的目的物理/IOVA | EP `REG_RECV` 槽，不经过 PCI |

发送方要对 **本板 PCIe RC 设备**（`infer_rc` 的 `pdev`）做 `dma_map_*`，得到的才是合法 `pci_addr`。  
接收方要对 **本板 PCIe EP / eDMA 设备** 保证 `local_dst` 可写（同一 IOMMU 域或物理连续 DDR）。

## 5. 槽位模型（双缓冲起步）

每条 EP 链路维护 `INFER_V2_SLOTS`（建议 2）个接收槽：

```
slot[i]:
  local_dst, capacity, flags
  state: EMPTY | POSTED | BUSY | DONE | ERROR
```

- **POSTED**：recv 侧已注册好 NPU dst，允许对端 PUSH
- **BUSY**：门铃已响，eDMA 进行中
- **DONE**：eDMA 完成，等用户态收完再 EMPTY/再次 POSTED

发送方在 PUSH 前应看到对端 credit（`posted_mask` 或 per-slot status），避免覆盖。

## 6. 一次 A→B 推送时序

```
B (EP / recv):
  1. INFER_EP_IOC_REG_RECV(slot, local_dst, capacity)
  2. INFER_EP_IOC_POST(slot)     → slot=POSTED，更新 BAR1 credit

A (RC / send):
  3. dma_map(NPU_src) → pci_addr
  4. 可选: 读 BAR1 posted_mask，确认 slot 可用
  5. INFER_IOC_PUSH { slot, size, pci_addr, timeout }
       驱动写: slot, size, pci_addr, command=PUSH；响门铃；轮询 status
  6. 返回耗时；unmap（或保持长期 map）

B (EP):
  7. 门铃 IRQ → workqueue
  8. 校验 slot==POSTED && size<=capacity
  9. eDMA: src=pci_addr → dst=slot.local_dst, len=size
 10. status=OK；slot=DONE；seq++
 11. 用户态 INFER_EP_IOC_WAIT/POLL → 得知 DONE，交给 NPU；再 POST 下一轮
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
| `local_dst_lo/hi` | — | **不放 BAR**（安全/无意义）；只存在 EP 内核 |

门铃仍在 BAR0+`db_offset`。

## 8. ioctl 草案

### 8.1 RC（`/dev/infer_rc0`）— 发送侧

| ioctl | 作用 |
| --- | --- |
| `INFER_IOC_PUSH` | 指定 `pci_addr`+`size`+`slot`，门铃+轮询（零拷贝发送） |
| `INFER_IOC_XFER` | **保留 v1**：用驱动 staging，便于回归延迟 |
| `INFER_IOC_MAP_USER`（可选） | 传入用户 VA，驱动 `pin_user_pages`+`dma_map`，返回 `pci_addr` |
| `INFER_IOC_GET_CREDIT`（可选） | 读对端 `posted_mask` |

长期 map 的 NPU buffer 更适合运行时自己 `dma_buf`/heap 导出，驱动只收 `pci_addr`。

### 8.2 EP（新 `/dev/pci_epf_infer0` 或扩展 ctl）— 接收侧

| ioctl | 作用 |
| --- | --- |
| `INFER_EP_IOC_REG_RECV` | 注册 slot 的 `local_dst` + `capacity` |
| `INFER_EP_IOC_POST` | slot → POSTED，更新 credit |
| `INFER_EP_IOC_WAIT` | 阻塞/超时等待 slot → DONE |
| `INFER_EP_IOC_UNREG` | 取消注册 |

`local_dst` 来源优先级建议：

1. **dma-buf fd**（最佳，接 NPU runtime）
2. 用户显式物理/IOVA（需 CAP_SYS_RAWIO，仅调试）
3. 驱动暂用 staging（兼容，非零拷贝）

## 9. 平台探针（实现前必做）

在改大量 ioctl 之前，板上先验证：

1. **EP eDMA 能否写 NPU/共享 DDR 地址**  
   临时把 `epf_infer_dma_xfer` 的本地地址从 `ctx->buf_dma` 换成候选 NPU buffer，做一次 WRITE，校验内容。
2. **RC 侧 NPU buffer 的 `dma_map` 是否被对端正确读到**  
   用已知图案填 NPU/DDR，PUSH 后在 EP 侧比对。
3. **对齐**  
   记录 eDMA 最小对齐；不对齐则头尾软件补齐、中间零拷贝。

探针失败则退化为「eDMA ↔ 共享 DDR ↔ NPU 私有搬」——仍比双 staging 好，但不是全零拷贝。

## 10. 分阶段落地

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| P0 | 文档 + `infer_proto_v2.h`（本阶段） | 评审通过 |
| P1 | EP：`REG_RECV`/`POST`/`WAIT`，dst 仍可用 staging 地址做通 | recv 用户态能取到数据 |
| P2 | RC：`INFER_IOC_PUSH` 接受外部 `pci_addr` | 发送端去掉 RC staging 拷贝 |
| P3 | EP dst 换 NPU/dma-buf；双 slot credit | 端到端零拷贝 + 流水 |
| P4 | 推理 runtime 绑定（两 rank send/recv） | 业务路径 |

## 11. 明确不做的事（本草案）

- 不靠 READ 实现跨板 recv
- 不把 NPU 私有地址不经 map 直接塞进 `pci_addr`
- 不在关键路径依赖 MSI
- 不在一条缆上同时跑双向大数据（credit/门铃可以双向，数据面保持单向）

## 12. 与 v1 的关系

- v1 模块与 `inferlat` **保持可用**（延迟基线）
- v2 用新 command / 新 ioctl 号；`INFER_IOC_XFER` 不删除
- 设备 ID 可暂仍 `1ef1:0301`；若需并行加载两套协议，再议 `0302`
