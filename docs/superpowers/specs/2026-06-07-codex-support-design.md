# Codex CLI 支持 — Design Spec

**Date**: 2026-06-07
**Status**: Approved (pending user review)
**Authors**: liuyingwen + Claude
**Depends on**: [2026-06-07-clawd-mood-design.md](./2026-06-07-clawd-mood-design.md)

## 1. 目标

让现有 clawd-mood 表情桌面摆件除了响应 Claude Code，也响应 **OpenAI Codex CLI**（codex-cli ≥ 0.133.0）的运行状态，复用同一套 daemon / FIFO / 串口 / 固件管道。

两个 CLI 可独立装、可同时挂、并发运行不冲突。

## 2. 范围

**做什么**：
- 新增 Codex 插件清单 `plugin/.codex-plugin/plugin.json`
- 新增 Codex 专用 hooks 文件 `plugin/hooks/hooks-codex.json`（10 个事件）
- 新增 `.agents/plugins/marketplace.json`（codex 官方期望路径，实测反推确认），让 codex 能 `plugin marketplace add .`
- 修改 `plugin/scripts/hook.sh`，追加 3 行 case 处理 Codex 独有事件
- 新增 `AGENTS.md`（一行指向 CLAUDE.md，让 codex 读到项目守则）
- 更新 README、CLAUDE.md 增加 Codex 挂载段和事件映射表

**不做什么**：
- 不改 `daemon.py`
- 不改固件
- 不动现有 `hooks.json` 和 `.claude-plugin/plugin.json`
- 不预防性兼容 `$CLAUDE_PLUGIN_ROOT` 未来可能被移除（codex 0.133.0 仍提供）
- 不为 codex 写独立 daemon / FIFO / 端口
- 不做 Windows/Linux 适配（仍 macOS only）
- 不写自动化测试

## 3. 整体架构

新增 Codex 作为并联事件源：

```
Claude Code event ─┐
                   │
                   ├─→ hook.sh → /tmp/clawd-mood.fifo → daemon.py → USB → ESP32
                   │
Codex CLI event ───┘
```

并发安全性：
- **FIFO 多写者单读者**：两个 CLI 的 hook 进程都写同一个 FIFO，daemon 单点串行消费
- **daemon singleton**：现有 `pgrep -f "$DAEMON"` 单实例策略不变
- **串口独占**：daemon 启动时打开 `/dev/cu.usbmodem*`，两个 CLI 共享同一个 daemon → 共享同一个串口
- **表情序列**：两端事件按到达顺序交错驱动，不做合并/去重

## 4. 文件结构（diff）

```
clawd-mood/
├── .agents/plugins/marketplace.json     ← 新增（codex 官方期望路径）
├── plugin/
│   ├── .claude-plugin/plugin.json       (不变)
│   ├── .codex-plugin/plugin.json        ← 新增
│   ├── hooks/
│   │   ├── hooks.json                   (不变；Claude Code 用)
│   │   └── hooks-codex.json             ← 新增（Codex 用）
│   └── scripts/
│       ├── hook.sh                      ← 改：追加 3 行 case
│       └── daemon.py                    (不变)
├── firmware/                            (不变)
├── AGENTS.md                            ← 新增
├── CLAUDE.md                            ← 改
├── README.md                            ← 改
└── docs/superpowers/specs/
    └── 2026-06-07-codex-support-design.md   ← 本文件
```

## 5. hook.sh 改动

`plugin/scripts/hook.sh` 的 case 块追加 3 个分支，其它一律不动：

```diff
   PostToolUseFailure) STATE="error" ;;
   Notification)       STATE="waiting" ;;
   Stop)               STATE="done" ;;
   SubagentStart)      STATE="working" ;;
   SubagentStop)       STATE="working" ;;
+  PermissionRequest)  STATE="waiting" ;;
+  PreCompact)         STATE="thinking" ;;
+  PostCompact)        STATE="working" ;;
   *)                  exit 0 ;;
 esac
```

**保持不变**：
- `$CLAUDE_PLUGIN_ROOT` 引用（codex 0.133.0 提供为兼容别名）
- `SessionStart` 拉 daemon 的 `nohup … & disown` 块
- FIFO 读写打开模式 `exec 3<>"$FIFO"` 防阻塞
- `pgrep -f "$DAEMON"` singleton 判断
- `command -v jq` 检查
- `printf %s` 防 shell 注入

## 6. `.codex-plugin/plugin.json`

```json
{
  "name": "clawd-mood",
  "description": "Drives a clawd-mood ESP32-C3 desk mascot via USB serial, reflecting Codex CLI state on a TFT display.",
  "version": "0.1.0",
  "author": { "name": "liuyingwen" },
  "hooks": "./hooks/hooks-codex.json"
}
```

字段说明：
- `hooks` 字段显式指向 `hooks-codex.json`，避免 codex 误读 `hooks/hooks.json`（那是 Claude Code 用的）
- 其它字段对标现有 `.claude-plugin/plugin.json`

## 7. `hooks-codex.json` 事件映射

10 个事件（7 共有 + 3 Codex 独有），每项 `matcher: ""`、command 仍指 `"$CLAUDE_PLUGIN_ROOT"/scripts/hook.sh`（**不带 `async: true`**——见 R4）：

| 事件 | 类型 | → STATE | 说明 |
|------|------|---------|------|
| `SessionStart` | 共有 | `idle` | 同时承担拉 daemon 职责 |
| `UserPromptSubmit` | 共有 | `thinking` | 用户提问 |
| `PreToolUse` | 共有 | `working` | 工具调用前 |
| `PostToolUse` | 共有 | `working` | 工具调用后 |
| `PermissionRequest` | **Codex 独有** | `waiting` | 等用户批准（对标 Claude Notification） |
| `PreCompact` | **Codex 独有** | `thinking` | 上下文压缩中 |
| `PostCompact` | **Codex 独有** | `working` | 压缩完恢复 |
| `Stop` | 共有 | `done` | 任务结束（固件 3s 后自动回 idle） |
| `SubagentStart` | 共有 | `working` | 子代理启动 |
| `SubagentStop` | 共有 | `working` | 子代理结束 |

**未声明的 Codex 事件**（例如 Notification、PostToolUseFailure）：Codex 本就不会触发，无需处理。

**Claude Code 独有的 PostToolUseFailure / Notification**：保留在原 `hooks.json` 里，hook.sh 的 case 也保留，Codex 那边永远不会进这些分支。

## 8. `.agents/plugins/marketplace.json`

实施 Task 1 反推确认 codex CLI 期望的 manifest 路径为 `.agents/plugins/marketplace.json`（**不是**仓库根 `marketplace.json`）。schema 抄自 codex 自带的 `~/.codex/.tmp/bundled-marketplaces/openai-bundled/.agents/plugins/marketplace.json`：

```json
{
  "name": "clawd-mood",
  "interface": { "displayName": "clawd-mood" },
  "plugins": [
    { "name": "clawd-mood", "source": { "source": "local", "path": "./plugin" } }
  ]
}
```

关键字段差异（vs 文档推测）：
- `displayName` 在 `interface.displayName` 里，不是顶层
- `source` 字段嵌套：外层 `source` 是字段名、内层 `source: "local"` 是值
- `policy`、`category` 是 optional（bundled 都加了但 codex 不要求）

## 9. 安装流

### Claude Code（不变）

```bash
claude --plugin-dir /absolute/path/to/clawd-mood/plugin
```

### Codex CLI（新增）

```bash
cd /absolute/path/to/clawd-mood
codex plugin marketplace add .
codex plugin add clawd-mood@clawd-mood
```

之后任意目录跑 `codex` 都会激活插件。`codex plugin list` 可确认。卸载：`codex plugin remove clawd-mood@clawd-mood`。

## 10. `AGENTS.md`

Codex 读项目根 `AGENTS.md` 作为项目守则。本项目守则已经在 `CLAUDE.md` 里写完，避免重复维护：

```markdown
# AGENTS.md

See [CLAUDE.md](./CLAUDE.md) for project guidance — it applies to all coding agents (Claude Code, Codex CLI, etc.) working in this repo.
```

不做 symlink（跨平台不友好、git diff 不友好），就一行 markdown 指过去。

## 11. CLAUDE.md / README 改动要点

**CLAUDE.md** 改动：
- §"项目概要" 第一句：加上 "/ Codex CLI" 作为接收方
- §"三段式架构" 数据流图：上游加一条 "Codex CLI event" 分支
- §"状态机设计原则" 事件映射段：追加 Codex 三个独有事件的映射规则
- §"常用命令"→"挂载插件"：新增 Codex 子段
- 新增一节 §"双 CLI 并发"：说明 FIFO 多写者无冲突、表情交错、daemon 仍 singleton

**README** 改动：
- "挂载插件" 部分拆成 "Claude Code" 和 "Codex CLI" 两小节
- 新增一句话说明两端可并发挂载

## 12. 测试策略

**单元 — Codex hook 映射**：
```bash
for ev in PermissionRequest PreCompact PostCompact; do
  echo "{\"hook_event_name\":\"$ev\",\"tool_name\":\"Bash\"}" | plugin/scripts/hook.sh
done
# 观察 /tmp/clawd-mood.fifo 三条记录：waiting / thinking / working
```

**集成 — Codex 真实任务**：
- 装好插件后跑 `codex "列出当前目录"`，肉眼确认表情序列符合预期
- 触发一次需用户批准的命令（如 `rm`），观察 `waiting` 表情亮起
- 触发一次 `/compact` 手动压缩，观察 `thinking → working` 切换

**集成 — 双 CLI 并发**：
- 一终端开 Claude Code 长任务、另一终端开 Codex 长任务
- 观察：daemon 不崩、串口 log 无错乱 JSON、表情按事件到达顺序切换
- 确认 `pgrep -f scripts/daemon.py | wc -l` 始终 ≤ 1（macOS pgrep 无 -c）

**配置 lint**：
```bash
python3 -m json.tool marketplace.json
python3 -m json.tool plugin/.codex-plugin/plugin.json
python3 -m json.tool plugin/hooks/hooks-codex.json
```

**`codex plugin list`**：确认插件状态 healthy、hooks 已加载。

## 13. 风险 / 待验证

| # | 风险 | 应对 |
|---|------|------|
| R1 | ~~`marketplace.json` 精确 schema 文档未给全~~ | **已解决（实施 Task 1）**：manifest 路径是 `.agents/plugins/marketplace.json`，schema 见 § 8 |
| R2 | `$CLAUDE_PLUGIN_ROOT` 是 Codex 标注的 "compatibility alias"，未来可能下线 | 当前 0.133.0 确认提供，记录但不预防性改造；真出问题改成 `${PLUGIN_ROOT:-$CLAUDE_PLUGIN_ROOT}` |
| R3 | Codex 对未声明事件可能 warn 或拒载 | hooks-codex.json 只声明 codex 真支持的事件；hook.sh 用 `*) exit 0` 兜底 |
| R4 | codex 0.133.0 不支持 `async: true`（`plugin add` 接受但运行时 skip 并打 warning `async hooks are not supported yet`） | **已解决**：移除 hooks-codex.json 所有 `async: true`；hook.sh 本身是非阻塞实现（FIFO RW open），不需要 async 标记 |
| R5 | 两端表情并发抢屏可能"鬼畜" | 接受现状；用户嫌乱可只挂一端，未来再考虑事件队列 |
| R6 | **Codex 拷贝 plugin 到 `~/.codex/plugins/cache/`（不是 symlink）**：改 `plugin/scripts/*` 后 codex 仍跑旧版本 | **开发流约束**：改完源仓库要 `codex plugin remove clawd-mood@clawd-mood && codex plugin add clawd-mood@clawd-mood`。Claude Code 走 `--plugin-dir` 是 live path 无此问题。已写入 CLAUDE.md 挂载段和 README 故障排除 |

## 14. 关键约束沿用

仍受现有项目约束（详见 CLAUDE.md），新增工作必须遵守：
- macOS only（不引入 `timeout` 或其它 GNU coreutils 依赖）
- 屏幕 VCC 接 3.3V 不动
- `USB CDC On Boot` Enabled 不动
- `tft.setRotation(1)` 不动
- daemon 仍由 `SessionStart` 拉起（**注意**：codex 的 SessionStart 同样会触发，已确认 codex 提供此事件，所以 codex 端首次启动也能自动拉 daemon）

## 15. 完成定义（DoD）

- [ ] `marketplace.json`、`.codex-plugin/plugin.json`、`hooks-codex.json`、`AGENTS.md` 创建并通过 JSON lint
- [ ] `hook.sh` 3 行 case 追加，shellcheck 无新警告
- [ ] `codex plugin marketplace add .` 和 `codex plugin add clawd-mood@clawd-mood` 成功
- [ ] `codex plugin list` 显示 clawd-mood healthy
- [ ] Codex 端真实任务 → 表情序列正确（含 `waiting` 在批准请求时触发）
- [ ] Claude Code 端回归测试通过（确认未破坏现有行为）
- [ ] 双 CLI 并发 5 分钟 → daemon 不崩、表情切换合理
- [ ] README 和 CLAUDE.md 更新合并
- [ ] 全部改动一次 commit，包含 spec 文档
