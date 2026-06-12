# 用自研 AI 编译器编译一层 DeepSeek-V4-Flash（FP4 → INT4）实践方案

目标：在不支持 FP4、但支持 INT4 的自研编译器 + NPU 上，把 DeepSeek-V4-Flash 的**单个 Transformer 层**跑通，作为整网移植前的 PoC。

本文中关于 checkpoint 的格式信息，来自对 `deepseek-ai/DeepSeek-V4-Flash` 官方 Hugging Face 仓库的 `config.json`、`model.safetensors.index.json` 以及 safetensors 分片头部（HTTP Range 请求直接读取，无需下载权重）的实际解析，截至 2026-06。

## 1. 先搞清楚：FP4 在 V4-Flash 里到底用在哪

V4-Flash 是 284B 总参 / 13B 激活的 MoE 模型，43 层（`num_hidden_layers = 43`），混合精度存储。**FP4 只用于路由专家权重，其他部分都不是 FP4**，所以"编译器不支持 FP4"这个问题的影响面比想象的小。

实测单层（以 `layers.2` 为例）的张量精度分布：

| 模块 | 张量 | 存储格式 | 说明 |
| --- | --- | --- | --- |
| 路由专家 × 256 | `ffn.experts.{i}.w1/w2/w3.weight` | **FP4 (E2M1)**，两两打包进 I8 张量 | 唯一的 FP4 部分 |
| 路由专家 × 256 | `ffn.experts.{i}.w1/w2/w3.scale` | F8_E8M0，每 32 元素一个 | 2 的幂缩放（MXFP4 风格） |
| 共享专家 × 1 | `ffn.shared_experts.w1/w2/w3` | FP8 E4M3 + 128×128 块缩放（ue8m0） | |
| 注意力投影 | `attn.wq_a/wq_b/wkv/wo_a/wo_b` | FP8 E4M3 + 128×128 块缩放 | MLA 风格低秩 Q/O |
| 注意力压缩器 / indexer | `attn.compressor.*`、`attn.indexer.*` | BF16 为主，indexer 的 `wq_b` 是 FP8 | lightning indexer |
| 路由门控 | `ffn.gate.weight` | BF16 | 256 专家、top-6 |
| 各类 norm | `attn_norm/ffn_norm/q_norm/kv_norm` | BF16 | |
| mHC 超连接 | `hc_attn_*/hc_ffn_*` | F32 | hc_mult=4 的流形约束超连接 |

证据链：

- `config.json` 中 `"expert_dtype": "fp4"`，`quantization_config` 为 fp8/e4m3/ue8m0 + `weight_block_size [128,128]`（这是非专家权重的格式）；
- safetensors 头部里专家权重 `w1.weight` 形状 `[2048, 2048]` dtype `I8`，对应逻辑形状 `[2048, 4096]`（`moe_intermediate_size=2048`、`hidden_size=4096`），即**每个 I8 字节装两个 FP4**；
- 配套 `w1.scale` 形状 `[2048, 128]` dtype `F8_E8M0`，4096 / 128 = 32，即**每 32 个权重一个 2 的幂缩放因子**——这就是 MXFP4 的组织方式。

FP4 E2M1 只有 16 个码点：`{0, ±0.5, ±1, ±1.5, ±2, ±3, ±4, ±6}`（含 ±0）。这是后面做 INT4 转换时的核心事实。

另外注意：43 层并不同构。`compress_ratios = [0, 0, 4, 128, 4, 128, ..., 4, 0]`，即第 0/1 层和最后一层是普通（非压缩）注意力，中间层在 CSA（压缩比 4）和 HCA（压缩比 128）之间交替；所有层的 FFN 都是 MoE。选哪一层做 PoC 会影响注意力侧的算子集合。

## 2. 总体路线：FP4 → INT4 的三种做法

### 路线 A：逐组直接映射（快速打通，精度有损）

每个 32 元素的 MXFP4 组内，先用 E8M0 scale 反量化回真实值，再对该组做对称 INT4 重量化：

- FP4 组内最大码点是 ±6，INT4 对称范围是 ±7；
- 取新 scale `s_int = s_fp4 * 6 / 7`（或逐组用实际 max），把每个 FP4 码点就近舍入到 INT4 网格。

问题：FP4 网格是**非均匀**的（0.5/1/1.5/2/3/4/6），INT4 是均匀网格。小数值区（±0.5、±1、±1.5）能精确对齐的码点很少，逐元素会引入最大约 4–7% 的相对舍入误差。优点是不需要校准数据、纯离线张量变换、几分钟搞定，适合先把编译/运行链路打通。

### 路线 B：反量化 + PTQ 重量化（推荐的正式路线）

1. 把专家权重 FP4 → BF16/FP32 完全反量化（数学上无损，FP4+E8M0 的每个值都能被 BF16 精确表示）；
2. 用带校准的 PTQ 算法（GPTQ / AWQ / AutoRound，或 NVIDIA ModelOpt 的 INT4 配置）重新量化为 INT4，组大小建议沿用 32（与原 FP4 分组一致，硬件友好的话也可用 64/128），scale 用 FP16/BF16 而不是 E8M0，去掉 2 的幂限制能明显降低量化误差；
3. 校准数据用几百条真实推理 prompt 经过前面层得到的该层输入激活。

由于原模型本来就是按 4 bit 训练/发布的（QAT 或 FP4-aware 训练），权重分布天然适合 4 bit 表达，INT4-PTQ 之后的误差通常显著小于"从 BF16 模型直接压到 INT4"的场景。这是最值得投入的路线。

### 路线 C：QAT 微调

只有在路线 B 精度仍不达标时才考虑，需要训练管线，单层 PoC 阶段不要碰。

补充一个常见误区：**不要试图用 INT4 码点"模拟" FP4 语义**（比如查表+反量化在 NPU 上做 LUT）。除非 NPU 有高效 LUT 指令，否则不如老老实实 INT4 重量化。

## 3. 只拉一层的权重

整个模型 160 GB，但单层约 3.5 GB，且 checkpoint 恰好基本按层分片（`layers.2` 完整落在 `model-00004-of-00046.safetensors` 一个分片里）。做法：

```python
import json
from huggingface_hub import hf_hub_download

repo = "deepseek-ai/DeepSeek-V4-Flash"
LAYER = 2  # 选一个 CSA 层；HCA 层选 3，非压缩层选 0/1

idx = json.load(open(hf_hub_download(repo, "model.safetensors.index.json")))
shards = sorted({v for k, v in idx["weight_map"].items()
                 if k.startswith(f"layers.{LAYER}.")})
files = [hf_hub_download(repo, s) for s in shards]  # 单层通常只有 1 个分片
```

注意张量命名是 `layers.N.*`（不是 transformers 常见的 `model.layers.N.*`），加载时按 `layers.{LAYER}.` 前缀过滤即可。

## 4. FP4 反量化参考实现（golden 基准）

先在 PyTorch 里构建 BF16/FP32 的单层 golden 模型，它有两个用途：作为 INT4 重量化的输入，以及作为 NPU 比对的参考输出。

```python
import torch

# E2M1 的 16 个码点，索引即 4bit 编码（按 sign|exp|mantissa 排布）
FP4_LUT = torch.tensor(
    [0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0,
     -0.0, -0.5, -1.0, -1.5, -2.0, -3.0, -4.0, -6.0])

def dequant_mxfp4(packed_i8: torch.Tensor, scale_e8m0: torch.Tensor):
    """packed_i8: [out, in//2] int8（每字节两个fp4）
       scale_e8m0: [out, in//32] uint8（E8M0，值 = 2**(x-127)）"""
    b = packed_i8.view(torch.uint8)
    lo, hi = b & 0xF, b >> 4               # 低半字节在前；如对不上 golden 就换序
    w = torch.stack([lo, hi], dim=-1).reshape(b.shape[0], -1)
    w = FP4_LUT[w.long()]
    s = torch.exp2(scale_e8m0.view(torch.uint8).float() - 127.0)
    return (w.reshape(*s.shape, 32).float() * s[..., None]).reshape(w.shape)
```

两个需要对 golden 验证的细节（不同导出工具约定不同，务必用官方推理代码或 vLLM/SGLang 的 V4 实现对一遍数值）：

1. 一个 I8 字节里低 4 位和高 4 位哪个是前一个元素；
2. E8M0 的偏置（标准为 127，值域 2^-127 … 2^127，无符号）。

非专家权重的 FP8 E4M3 + 128×128 块缩放反量化是常规操作，`torch.float8_e4m3fn` 直接 `.float()` 后乘块 scale 即可。

## 5. INT4 重量化（路线 A 示例）

```python
def fp4_group_to_int4(w_bf16: torch.Tensor, group: int = 32):
    """w_bf16: 已反量化的专家权重 [out, in]，按行内每 group 个元素一组对称量化"""
    g = w_bf16.reshape(w_bf16.shape[0], -1, group)
    scale = g.abs().amax(dim=-1, keepdim=True).clamp(min=1e-8) / 7.0
    q = (g / scale).round().clamp(-7, 7).to(torch.int8)   # 留 -8 不用，保持对称
    return q.reshape(w_bf16.shape), scale.squeeze(-1).to(torch.float16)
```

路线 B 则把反量化后的单层权重 + 校准激活喂给 GPTQ/AutoRound，产出同样的 `(int4 权重, fp16 scale)` 布局，再 pack 成自家编译器的 INT4 权重格式。

各部分的目标精度建议：

| 模块 | 建议 NPU 精度 |
| --- | --- |
| 路由专家 w1/w2/w3 | INT4（per-group 32, 对称, FP16 scale），激活 INT8 或 FP16（即 W4A8 / W4A16） |
| 共享专家、注意力投影 | NPU 支持 FP8 就保留 FP8；不支持则 INT8（per-channel）或直接 FP16 |
| gate / norm / mHC | FP16/FP32，**不要量化**（router 的 top-6 选择对扰动极其敏感，mHC 是流形约束的小矩阵，量化收益为零） |
| indexer | FP16 即可（官方在 GPU 上跑 FP4，但参数量很小） |
| KV cache | FP8 不支持就 INT8（per-token 或 per-head scale） |

## 6. 单层图的算子清单（编译器侧要准备什么）

单层 = mHC 展开 + 注意力 + MoE FFN。按 `config.json` 的真实结构，需要覆盖的算子：

**通用**：RMSNorm、SiLU/SwiGLU（注意 `swiglu_limit = 10.0`，激活有 clamp，自研 kernel 别漏）、RoPE（YaRN 缩放，`rope_theta=10000`，压缩支路另有 `compress_rope_theta=160000`）、逐元素加乘。

**注意力（以 CSA 层为例）**：
- 低秩 Q/O 投影：`wq_a [1024,4096]` → `wq_b [32768,1024]`，`wo_a [8192,4096]` → `wo_b [4096,8192]`（q_lora_rank / o_lora_rank = 1024，o_groups=8）；
- KV 压缩器（compressor）：`wkv/wgate [1024,4096]` + 可学习位置嵌入 ape + softmax 门控，对 KV 做 4 倍压缩；
- lightning indexer：小型打分头（64 头 × 128 维）+ **top-k 选择（`index_topk = 512`）**——这是最难编译的部分，需要 top-k 算子和 gather/稀疏注意力支持；
- 注意力本体：单 KV 头（`num_key_value_heads = 1`，MQA 形态）、64 个 Q 头、head_dim 512、带 `attn_sink`、滑窗 128。

**MoE FFN**：
- gate：`[256, 4096]` matmul + `sqrtsoftplus` 打分 + noaux_tc top-6 + `routed_scaling_factor = 1.5` 加权（保持 FP32）；
- 256 路由专家的 grouped/batched GEMM（INT4 权重 × INT8/FP16 激活）+ top-6 gather/scatter；
- 共享专家走普通 GEMM。

**mHC（hyper-connections）**：每层 attn/ffn 前后各一组 `hc_*` 参数（hc_mult=4，残差流是 4 份宽），涉及 Sinkhorn 迭代得到的混合矩阵。这部分计算量极小但图结构特殊，PoC 阶段可以放 CPU/host 侧，或者直接选择"冻结展开"为固定矩阵乘。

**PoC 简化建议**：第一步可以把序列长度限制在滑窗内（≤128 token）并强制 dense attention，绕开 indexer top-k 和压缩器，先验证 INT4 MoE + FP8/INT8 注意力投影的数值正确性；第二步再补稀疏注意力链路。

## 7. 导出与编译流程

1. **建参考模型**：用官方 `modeling_deepseek_v4`（HF repo 自带，`trust_remote_code`）或 vLLM/SGLang 的实现，把单层包成一个 `nn.Module`，权重换成第 4 节反量化后的 BF16 版本。固定一组输入（hidden_states `[B, S, 4096]`，从真实前向里 dump，不要用随机数——MoE 路由对输入分布敏感，随机输入会路由到不具代表性的专家组合）；
2. **dump golden**：保存该层输入、输出、以及关键中间值（attn 输出、router logits、top-6 专家 id、每个专家的输出）；
3. **导出图**：`torch.export` 或 ONNX。indexer top-k、MoE dispatch、mHC 大概率要注册成自定义算子，由编译器后端 pattern-match 到自研 kernel；
4. **量化 pass**：在编译器前端把专家 GEMM 替换为 INT4 weight + group scale 的量化 GEMM 节点（权重离线 pack），其余按第 5 节的精度表落格式；
5. **NPU 运行 + 比对**。

## 8. 精度验收标准

逐层级比对 NPU 输出 vs golden（BF16 参考）：

- **专家 GEMM 单算子**：路线 A 直接映射下，预期 cosine 相似度 ≥ 0.995；路线 B（GPTQ）应能到 ≥ 0.999；
- **整层输出**：cosine ≥ 0.99、相对误差（`|y_npu - y_gold| / |y_gold|` 的 P99）≤ 5% 是合理起点；
- **路由一致性**：top-6 专家命中率单独统计——如果 gate 保持 FP32，应当 100% 一致；不一致说明输入激活的量化已经影响 router，需要把 ffn_norm 输出到 gate 的支路保持高精度；
- 比对务必用多条真实激活（不同 token 位置、不同 prompt），单条输入的 cosine 会虚高。

如果路线 A 整层误差超标，先别上 QAT，按顺序尝试：组大小 32 → 16；对称 → 非对称（带 zero-point）；最后上 GPTQ/AutoRound（路线 B）。

## 9. 常见坑

1. **E8M0 scale 是无符号 2 的幂**，没有 NaN/Inf 语义之外的特殊值，bias 127；转 INT4 后改用 FP16 scale 时记得在编译器中声明新的 scale dtype，不要沿用 ue8m0 路径；
2. **FP4 打包字节序**（低/高半字节顺序）各家约定不一，必须和官方推理代码对数；
3. **`gate.tid2eid [129280, 6]`（I64）**：这是 token-id 到专家的查找表（疑似与 noaux_tc 路由或专家亲和初始化相关），导出图时别丢，按官方 forward 逻辑处理；
4. **`attn_sink`、`swiglu_limit`、`routed_scaling_factor`** 这类小常数/小张量最容易在手写 kernel 里遗漏，逐个核对 config；
5. 层与层不同构（CSA/HCA/非压缩），PoC 选定的层编号要和 `compress_ratios` 对准，别拿第 2 层的图去编第 3 层的权重;
6. INT4 用 `[-7, 7]` 对称范围（弃用 -8）可以让正负对称、且和 FP4 的 ±6 网格对齐更自然；如果硬件按 `[-8, 7]` 实现，注意 scale 计算的分母用 7 还是 8 要与 kernel 一致；
7. MTP（multi-token prediction）块在主 43 层之外（`num_nextn_predict_layers = 1`），单层 PoC 不涉及，整网移植时再处理。

## 10. 参考

- 模型与权重：`deepseek-ai/DeepSeek-V4-Flash`（Hugging Face，MIT 许可）
- 官方混合精度说明："FP4 + FP8 Mixed: MoE expert parameters use FP4 precision; most other parameters use FP8."（模型卡）
- 同类先例：NVIDIA `DeepSeek-V4-Flash-NVFP4`（ModelOpt 量化，NVFP4 = group 16 + E4M3 scale，与官方 checkpoint 的 MXFP4 风格 group 32 + E8M0 不同，注意区分）；社区 W4A16 转换版（针对无 FP4 算力的硬件，思路与本文路线 B 相同）
- PTQ 工具：GPTQ / AWQ / AutoRound / NVIDIA TensorRT Model Optimizer（INT4 配置）
