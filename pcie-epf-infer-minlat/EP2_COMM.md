# EP2 PCIe 通信联调：问题定位与正确用法

> 针对 `BstEpCommPcie` + `test_moe_ep_pcie` 双板失败日志（`slot not POSTED` / `WAIT result=-5` / `-110`）。

## 1. 日志怎么读

| 用户态打印 | errno 含义 | 驱动侧含义 |
| --- | --- | --- |
| `send: slot 0 not POSTED after 5s` | 本地 credit 轮询超时 | 对端 EP **还没** `POST_RECV`，`posted_mask` bit0=0 |
| `send PUSH failed result=-5` | `-EIO` | RC 看到 BAR1 `status=FAIL`（对端拒收或 DMA 失败） |
| `recv WAIT failed result=-5` | `-EIO` | EP slot 已是 **ERROR**（未 POSTED 的 PUSH / capacity 不够 / DMA 失败） |
| `recv WAIT failed result=-110` | `-ETIMEDOUT` | 一直等不到 DONE（对端没成功 PUSH） |

你这次双板日志的典型时间线：

```
两边同时进 MoE forward
  → 两边都先 send（等对端 POSTED）
  → 两边都还没 post_recv
  → 5s 后 credit 超时（not POSTED）
  → 再 recv / WAIT：对端已放弃 send → -110
  → 偶发 -5：对端门铃在本地尚未 POSTED（或 capacity 不够）时打进来
```

**PCIe 链路本身是通的**（`init` 已拿到 staging / `ep_flags=0x7`）。失败在 **credit 握手顺序**，不是枚举/门铃硬件坏了。

## 2. 根因（按优先级）

### 根因 A（主因）：双边同时 `send()`，没有先 `post_recv`

v2 协议硬性要求：

```
接收方: POST_RECV → (posted_mask 置位)
发送方: 见 credit → PUSH
接收方: WAIT → DONE
```

`BstEpCommPcie::send()` 会轮询 `INFER_IOC_GET_CREDIT` 直到 `posted_mask` 含 slot。  
若 MoE dispatcher 在 **dispatch / combine** 阶段写成：

```text
rank0: send(1) ; rank1: send(0)     ← 双方都在等对方 POSTED
```

则必然死锁到 5s。

头文件里已经写了正确拆分 API，**dispatcher 必须用**：

```text
for peer: post_recv(peer, capacity)
for peer: send(peer, data, size)
for peer: wait_recv(peer, buf, size)
```

或至少保证：**每一条缆上，recv 侧的 POST_RECV 早于对端 PUSH**。

`test_pcie_comm_ep2` 里「rank0 先 recv、rank1 先 send」的串行 ping-pong 是对的；  
`test_moe_ep_pcie` 的 all-to-all dispatch **不能**两边同时阻塞在 `send()`。

### 根因 B：单 slot=0 + 同缆串行

驱动当前：

- `INFER_V2_SLOTS=2`，但 `BstEpCommPcie` **固定 SLOT=0**
- EP `ctx->busy`：**同缆单 outstanding**

因此同一时刻只能有一包 in-flight。MoE 若对同一 peer 重叠多次 `send`/`recv`，或未等 WAIT 完成就下一包，会乱。

### 根因 C（驱动行为）：未 POSTED 的 PUSH 会把 slot 标成 ERROR

当前逻辑：门铃收到 `PUSH` 但 slot≠POSTED 时，仍把 slot 标成 **ERROR** 并给 RC 写 `status=FAIL`。  
随后本地若已在 `WAIT`，会立刻得到 `-EIO`（日志里的 `result=-5`）。

这不是主因（主因仍是两边先 `send` 等 credit），但会放大竞态下的 `-5`。优先把上层改成 `post_recv → send → wait_recv`。

### 根因 D：`POST_RECV.capacity` < `PUSH.size`

若 `post_recv(capacity)` 小于对端实际 `send` 长度 → EP 拒收 `-EMSGSIZE` → slot ERROR → `WAIT -5`。  
MoE 必须按 **本包最大 payload**（含 header）设 capacity。

## 3. MoE dispatcher 应改成的形态

```cpp
// ---- dispatch 交换 token（双缆，可并行准备）----
// 1) 先在本板 EP 挂好接收缓冲（非阻塞）
comm.post_recv(peer, max_dispatch_bytes);

// 2) 再经本板 RC PUSH 到对端（此时对端也应已 post_recv）
comm.send(peer, dispatch_bytes, dispatch_len);

// 3) 最后等本板 EP eDMA 完成并拷出
comm.wait_recv(peer, recv_buf, max_dispatch_bytes);

// combine 同理：先 post_recv，再 send，再 wait_recv
```

**禁止**在未 `post_recv` 时调用会阻塞等 credit 的 `send()` 做双向同时发送。

若暂时只能改通信层、不能改 dispatcher：可在 `send()` 里 **不要**死等 5s credit，改为短超时并让上层重试；但这治标不治本，all-to-all 仍需 `post_recv` 先行。

## 4. 建议验证顺序

1. **单方向 staging**（已知可过）  
   `inferpush ep` / `inferpush rc`
2. **双向 EP2 通信层**（本目录 `test_pcie_comm_ep2`）  
   两板同时跑，确认 hello + 50KB ping-pong 与完整性 OK  
3. **再跑** `test_moe_ep_pcie`（dispatcher 已改成 post/send/wait）

```bash
# 两板 EP+RC bring-up 完成后：

# B 板 rank0
./test_pcie_comm_ep2 0 /dev/infer_rc0

# A 板 rank1
./test_pcie_comm_ep2 1 /dev/infer_rc0
```

## 5. 和数值 FAIL 的关系

`Max abs diff ... M3 FAIL` 是 **通信失败后的连带结果**：  
远端 expert 的 token 没收到（日志里 `data[0..3]=0`、`combine_recv_count` 异常），本地只算了部分 expert，和单卡 full dispatch 对不上。  
先把 PCIe send/recv 握手跑绿，再看数值。

## 6. 换驱动后看 dmesg

拒收 PUSH 时 EP 侧会出现：

```text
pci_epf_infer: PUSH reject: slot 0 state=0 (need POSTED) size=...
```

若频繁出现，就是上层仍在「未 post_recv 就 PUSH」。
