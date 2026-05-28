# OpenClaw + QMD 本地知识库实践文档

本文档总结一种实践方案：用户通过自然语言让 OpenClaw 在服务端扫描本地目录，将多级子目录下的大量文件整理/索引为 QMD 本地知识库；用户随后在 OpenClaw 聊天中可以使用该知识库内容；客户端上的 Claude Code 也可以访问服务端的同一套 QMD 知识库。

## 1. 目标

实现以下能力：

1. 用户用自然语言告诉 OpenClaw：
   - 要导入哪个本地目录；
   - 知识库叫什么名字；
   - 哪些文件要处理、哪些目录要跳过；
   - 是否需要 OCR、PDF/Word 转 Markdown、代码索引等。
2. OpenClaw 在服务端完成：
   - 目录扫描；
   - 文件清单生成；
   - 文档转换；
   - Markdown 知识库归一化；
   - QMD 配置；
   - 索引构建；
   - 检索验证。
3. 用户和 OpenClaw 聊天时，可以基于 QMD 本地知识库回答问题。
4. 客户端 Claude Code 也能通过受控方式使用服务端 QMD 知识库内容。

## 2. 推荐架构

```text
用户 / 浏览器 / 聊天入口
        |
        v
OpenClaw Gateway（服务端）
        |
        +-- QMD sidecar / memory backend
        |
        +-- 本地知识库 Markdown 目录
        |
        +-- 原始资料目录，只读或受控访问

客户端 Claude Code
        |
        +-- SSH / MCP / HTTP wrapper
        |
        v
服务端 QMD / OpenClaw memory search
```

推荐把知识库分成两层：

```text
/srv/raw-docs/company-files/              # 原始资料，尽量只读
/srv/openclaw-kb/company-files/           # 转换后的 Markdown 知识库
~/.openclaw/workspace/MEMORY.md           # 高层说明，不放大量正文
~/.openclaw/workspace/memory/kb-index.md  # 知识库索引说明
```

## 3. QMD 在这里扮演什么角色

QMD 是 OpenClaw 的增强本地检索后端。它负责：

- 索引 Markdown、文档、笔记、会议记录等文本资料；
- 支持 BM25 关键词检索；
- 支持语义检索、rerank、query expansion，取决于 QMD 配置和模型；
- 索引 OpenClaw workspace 之外的目录；
- 可选索引历史 session transcript。

QMD 不是大模型，也不是知识库内容本身。知识库内容仍建议以 Markdown 文件为主。

## 4. 自然语言导入流程

用户可以直接对 OpenClaw 说：

```text
请把 /srv/raw-docs/company-files 构建成名为 company-files 的 QMD 本地知识库。

要求：
1. 递归扫描多级子目录；
2. 跳过 .git、node_modules、dist、build、.cache、临时文件和大于 100MB 的文件；
3. 对 md/txt/html/pdf/docx/csv/xlsx/代码文件分别处理；
4. 文本类文件转换成 Markdown；
5. PDF/Word 尽量提取正文并转成 Markdown；
6. 图片先 OCR 或生成文字说明，再写入 Markdown；
7. 代码文件可直接索引，必要时生成模块说明；
8. 输出到 /srv/openclaw-kb/company-files；
9. 生成 index.md、manifest.json、failed-files.md；
10. 配置 QMD paths 并重建索引；
11. 用 openclaw memory search 测试检索效果；
12. 不要删除或修改原始文件。
```

对于很大的目录，建议分两步：

```text
第一步：先只扫描 /srv/raw-docs/company-files，不处理正文。
请生成文件清单、文件类型统计、大小统计、跳过规则建议和导入计划。
```

确认计划后再说：

```text
按刚才的计划，先导入 docs、faq、manuals 三个目录，其他目录暂缓。
```

## 5. 文件类型处理建议

| 类型 | 推荐处理方式 |
| --- | --- |
| `.md` | 直接纳入知识库或复制到规范化目录 |
| `.txt` | 转成 Markdown |
| `.html` | 提取正文，转成 Markdown |
| `.pdf` | 提取文本，必要时 OCR，再转 Markdown |
| `.docx` | 用 pandoc、mammoth 等工具转 Markdown |
| `.csv` / `.xlsx` | 转成 Markdown 表格、字段说明或摘要 |
| 代码文件 | 可直接让 QMD 索引，或额外生成模块说明 |
| 图片 | OCR 或生成说明 Markdown；也可使用 Gemini 多模态索引 |
| 音频/视频 | 先转录成文本，再写入 Markdown |
| 二进制/压缩包 | 默认跳过，只记录元数据 |

QMD 的主路径仍然是文本检索。图片、音频、视频最好先转换为文本描述。

## 6. 推荐知识库输出结构

```text
/srv/openclaw-kb/company-files/
  index.md
  manifest.json
  failed-files.md
  docs/
    product-overview.md
    deployment-guide.md
  faq/
    billing.md
    troubleshooting.md
  code/
    backend-module-summary.md
  images/
    architecture-diagram.md
```

`index.md` 示例：

```md
# company-files 知识库索引

## 来源

原始目录：/srv/raw-docs/company-files
整理目录：/srv/openclaw-kb/company-files

## 内容分类

- docs/: 产品文档、部署文档、技术手册
- faq/: 常见问题
- code/: 代码模块说明
- images/: 图片 OCR 或图片说明

## 使用方式

回答问题前优先通过 memory_search 或 QMD 搜索本知识库。
若检索不到依据，应明确说明“知识库中没有找到相关内容”。
```

`manifest.json` 建议记录：

- 原始路径；
- 输出路径；
- 文件类型；
- 文件大小；
- 修改时间；
- 内容 hash；
- 转换状态；
- 错误信息。

这样后续可以做增量更新。

## 7. OpenClaw QMD 配置示例

在 `openclaw.json` 中启用 QMD：

```json5
{
  memory: {
    backend: "qmd",
    citations: "auto",
    qmd: {
      includeDefaultMemory: true,
      paths: [
        {
          name: "company-files",
          path: "/srv/openclaw-kb/company-files",
          pattern: "**/*.md"
        }
      ],
      update: {
        interval: "5m",
        debounceMs: 15000
      },
      limits: {
        maxResults: 8,
        timeoutMs: 8000
      },
      scope: {
        default: "deny",
        rules: [
          { action: "allow", match: { chatType: "direct" } },
          { action: "allow", match: { chatType: "channel" } }
        ]
      }
    }
  }
}
```

重建或检查索引：

```bash
openclaw memory status --deep
openclaw memory index --force
openclaw memory search "部署安全要求"
```

## 8. 用户聊天时如何使用知识库

用户可以直接问：

```text
请根据 company-files 本地知识库回答：我们的云端部署安全要求是什么？
```

或者更严格：

```text
请先搜索 QMD 本地知识库 company-files，再回答。
如果知识库里没有依据，请明确说明没有找到，不要猜测。
回答时请列出引用来源。
```

建议在 OpenClaw 的长期记忆或系统说明里写入：

```md
本实例有一个 QMD 本地知识库 company-files，路径为 /srv/openclaw-kb/company-files。
回答与产品、部署、安全、FAQ、内部规范相关的问题前，应优先使用 memory_search 检索。
如果检索结果不足，应说明缺少依据。
```

## 9. 客户端 Claude Code 如何使用服务端 QMD 知识库

前提：OpenClaw、QMD 和知识库都运行在服务端。客户端 Claude Code 不能天然访问服务端文件或 QMD 索引，需要提供一个访问桥接方式。

### 方式 A：Claude Code 通过 SSH 调用服务端 OpenClaw memory search

这是最简单的方式。客户端配置一个脚本：

```bash
#!/usr/bin/env bash
set -euo pipefail

query="$*"
if [ -z "$query" ]; then
  echo "Usage: kb-search <query>" >&2
  exit 1
fi

ssh openclaw-server "openclaw memory search --query $(printf '%q' "$query") --max-results 8"
```

然后在项目的 `CLAUDE.md` 中写：

````md
# Knowledge Base

服务端有 OpenClaw QMD 知识库。

回答产品、部署、安全、FAQ 或内部规范问题前，先运行：

```bash
kb-search "<你的问题>"
```

根据搜索结果回答；如果没有结果，不要编造。
````

优点：

- 实现简单；
- 不需要把服务端知识库同步到客户端；
- 复用 OpenClaw 的 QMD 配置。

注意：

- SSH 用户权限应最小化；
- 只允许执行检索命令更安全；
- 不要把 `~/.openclaw/credentials`、`.env`、`~/.ssh` 暴露给 Claude Code。

### 方式 B：通过 MCP 暴露 `kb_search` / `kb_get`

更产品化的方式是在服务端提供 MCP 工具：

```text
kb_search(query, kb_name?, max_results?)
kb_get(source_path, line_range?)
```

Claude Code 通过 MCP 调用这些工具，而不是直接 SSH。

优点：

- 更适合多客户端；
- 可以做权限控制；
- 可以做审计日志；
- 可以限制只读访问；
- 可以隐藏服务端真实路径。

建议 MCP 工具内部调用：

```bash
openclaw memory search --query "<query>"
```

或直接调用 QMD。

### 方式 C：同步只读 Markdown 知识库到客户端

如果客户端经常离线使用，可以将 `/srv/openclaw-kb/company-files` 只读同步到本地：

```text
服务端 /srv/openclaw-kb/company-files
        |
        v
客户端 ~/kb/company-files
```

Claude Code 直接读本地 Markdown，或者客户端也安装 QMD 建索引。

优点：

- 离线可用；
- 客户端查询快。

缺点：

- 有同步一致性问题；
- 可能扩大数据暴露面；
- 不适合敏感资料或多租户资料。

## 10. 多用户和权限建议

如果 OpenClaw 是多人使用：

```text
共享知识库：/srv/shared-kb/<kb-name>，只读挂载
用户私有知识库：/srv/users/<user-id>/kb/<kb-name>
```

建议：

- 同一信任边界的小团队可以共用共享知识库；
- 互不信任用户应使用独立 OpenClaw gateway、独立 QMD home、独立凭据；
- 共享知识库目录尽量只读；
- 不要允许普通用户导入任意系统路径；
- 导入路径应做 allowlist，例如只允许 `/srv/raw-docs`、`/srv/uploads`；
- 禁止导入 `.env`、SSH key、凭据目录、浏览器 profile 等敏感文件。

## 11. 安全导入规则

建议默认跳过：

```text
.git/
node_modules/
dist/
build/
.cache/
tmp/
*.key
*.pem
*.p12
*.env
.env*
id_rsa
credentials/
secrets/
```

建议导入前输出计划：

```text
将处理 1234 个文件：
- Markdown: 420
- PDF: 120
- Word: 80
- 图片: 300
- 代码: 314

将跳过 98 个文件：
- 大文件: 20
- 敏感文件: 8
- 二进制文件: 70
```

用户确认后再执行转换和索引。

## 12. 大规模知识库最佳实践

资料很多时，不要让 OpenClaw 一次性把所有内容读入上下文。

推荐：

1. 先扫描并生成 manifest；
2. 分类型、分目录、分批导入；
3. 原始文件不移动、不删除；
4. 转换后的 Markdown 放入稳定知识库目录；
5. QMD 只索引 Markdown 或明确配置的代码文件；
6. 使用增量更新，避免每次全量重建；
7. `MEMORY.md` 只放知识库说明和高层索引，不放大量正文；
8. 通过 `memory_search` 按需检索。

## 13. 代码和图片的处理

### 代码

QMD 可以索引代码文本。对代码库建议两种方式结合：

1. 直接索引代码文件，例如 `**/*.{ts,tsx,js,jsx,py,go,rs,md}`；
2. 生成 Markdown 说明，例如模块职责、关键函数、API 入口、依赖关系。

对问答来说，模块说明通常比直接索引全部代码更稳定。

### 图片

图片建议先转换成文本：

- OCR；
- 人工或模型生成图片说明；
- 将图片说明保存成同名 `.md`。

例如：

```text
architecture.png
architecture.md
```

`architecture.md` 中记录图片来源、摘要、关键元素和业务含义。

如果使用 Gemini Embedding 2 的多模态索引，可以直接索引部分图片格式，但这会把图片内容发送到对应 embedding endpoint。敏感资料应谨慎使用。

## 14. 验证清单

导入完成后，至少验证：

```bash
openclaw memory status --deep
openclaw memory search "产品定价"
openclaw memory search "部署安全要求"
openclaw memory search "常见故障排查"
```

检查：

- 是否能搜到正确知识库；
- 是否返回来源；
- 是否有明显无关结果；
- 是否有转换失败文件；
- OpenClaw 聊天是否会基于检索结果回答；
- Claude Code 是否能通过 SSH/MCP/同步目录访问同一知识库。

## 15. 推荐落地顺序

1. 先做单用户、单知识库验证；
2. 只支持 Markdown、txt、PDF、docx 这几类；
3. 建立 manifest 和 failed-files；
4. 接入 QMD；
5. 让 OpenClaw 聊天可检索；
6. 给 Claude Code 增加 SSH 或 MCP 检索入口；
7. 再扩展图片 OCR、代码索引、音视频转录；
8. 最后再做多用户权限隔离和审计。

## 16. 简短结论

可以让用户用自然语言在 OpenClaw 中构建 QMD 本地知识库。实践上应让 OpenClaw 把自然语言请求转成一套受控导入流程：扫描、转换、生成 Markdown、配置 QMD、重建索引、验证检索。

OpenClaw 聊天时通过 `memory_search` 使用该知识库。客户端 Claude Code 若要使用服务端上的同一套 QMD 知识库，需要通过 SSH、MCP、HTTP wrapper 或只读同步目录来访问，不能假设会自动共享。
