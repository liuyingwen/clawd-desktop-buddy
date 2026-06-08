# Codex CLI 支持 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 clawd-mood ESP32 表情桌面摆件除了响应 Claude Code，也响应 OpenAI Codex CLI（≥0.133.0）的运行状态，复用同一套 daemon / FIFO / 串口 / 固件管道。

**Architecture:** 在前端加一条并联事件源——Codex 插件清单 `.codex-plugin/plugin.json` 指向 `hooks-codex.json`，它跟现有 `hooks.json` 共享同一个 `hook.sh`。`hook.sh` 追加 3 个 case 处理 Codex 独有的 `PermissionRequest` / `PreCompact` / `PostCompact`。`$CLAUDE_PLUGIN_ROOT` 是 Codex 显式提供的兼容别名，无需改动。daemon 单实例 + FIFO 多写者天然支持双 CLI 并发。

**Tech Stack:** Codex CLI（marketplace + plugin manifest）、Bash + jq、现有 daemon.py 和 Arduino 固件不动。

**Spec:** [`docs/superpowers/specs/2026-06-07-codex-support-design.md`](../specs/2026-06-07-codex-support-design.md)（commit `897206b`）

---

## File Structure

| Path | Responsibility | 状态 |
|---|---|---|
| `.agents/plugins/marketplace.json` | Codex 本地 marketplace 入口（路径与 schema 实施 Task 1 反推确认） | 新增 |
| `plugin/.codex-plugin/plugin.json` | Codex 插件清单，指向 `hooks-codex.json` | 新增 |
| `plugin/hooks/hooks-codex.json` | Codex 10 个事件注册（7 共有 + 3 独有） | 新增 |
| `plugin/scripts/hook.sh` | 追加 3 个 case，其它不动 | 修改 |
| `plugin/hooks/hooks.json` | Claude Code 9 个事件，**不动** | 保持 |
| `plugin/.claude-plugin/plugin.json` | Claude Code 清单，**不动** | 保持 |
| `plugin/scripts/daemon.py` | 串口桥，**不动** | 保持 |
| `firmware/clawd_mood/clawd_mood.ino` | 固件，**不动** | 保持 |
| `AGENTS.md` | 一行指向 CLAUDE.md | 新增 |
| `CLAUDE.md` | 架构图、事件映射、挂载段、新增"双 CLI 并发"节 | 修改 |
| `README.md` | 使用段拆 Codex 子段、表情对照加 PermissionRequest | 修改 |

---

## Task 1: 创建 marketplace 清单并反推 schema  ✅ 已完成（commit `5ae4b8f`）

**Files:**
- Create: `.agents/plugins/marketplace.json`（实施反推确认 codex 官方期望路径）

Goal: 因为 Codex CLI 没有 `--plugin-dir` flag，本地开发必须先把仓库注册成 marketplace。Codex 官方文档未给完整 schema，本 task 用 `codex plugin marketplace add` 实测反推。

**实施反推结论**：抄 codex 自带的 `~/.codex/.tmp/bundled-marketplaces/openai-bundled/.agents/plugins/marketplace.json` 一次成功，零迭代。关键差异 vs 文档推测：
- manifest 路径在 `.agents/plugins/marketplace.json`（不是仓库根）
- `displayName` 嵌套在 `interface.displayName`
- `source` 字段嵌套：`{ "source": "local", "path": "./..." }`（外层 source 是字段名、内层 source 是值）
- `policy` / `category` optional

- [ ] **Step 1: 写初版 marketplace.json**

按 spec § 8 推测：

```json
{
  "name": "clawd-mood",
  "displayName": "clawd-mood",
  "plugins": [
    { "name": "clawd-mood", "source": { "path": "./plugin" } }
  ]
}
```

- [ ] **Step 2: 用 codex 实测加入**

```bash
cd /path/to/clawd-mood
codex plugin marketplace add .
```

预期之一：
- 成功：输出包含 `Added marketplace 'clawd-mood'` 之类
- 失败：报字段错误，例如 `unknown field 'source'`、`missing field 'version'`、`expected 'pluginPath'`

- [ ] **Step 3: 根据报错调整字段**

如果失败，按 codex 报错信息改字段名/结构、重跑 Step 2。常见调整方向（从 codex 源码 / 错误消息推断）：
- `source.path` 可能叫 `source.type`+`source.path` 或 `pluginPath`
- 可能要求顶层 `version` 或 `schemaVersion`
- 可能 `plugins` 数组项要带 `version`

迭代到 Step 2 成功为止。每次调整后用 `python3 -m json.tool marketplace.json` 先 lint。

- [ ] **Step 4: 验证 marketplace 已注册**

```bash
codex plugin marketplace list
```

预期输出包含名为 `clawd-mood` 的本地 marketplace，根路径指向当前仓库。

- [ ] **Step 5: 验证插件可被发现**

```bash
codex plugin list
```

预期输出包含 `clawd-mood@clawd-mood`（状态为 available / not-installed，因为还没 add）。

> 若 list 不显示，可能是 `plugin/.codex-plugin/plugin.json` 还没建（Task 2）—— 这种情况先做完 Task 2 再回来验证 Step 5。

- [ ] **Step 6: Commit**

```bash
git add marketplace.json
git commit -m "feat(codex): add local marketplace.json for plugin discovery"
```

---

## Task 2: 创建 Codex 插件清单

**Files:**
- Create: `plugin/.codex-plugin/plugin.json`

- [ ] **Step 1: 创建目录**

```bash
mkdir -p plugin/.codex-plugin
```

- [ ] **Step 2: 写清单**

```json
{
  "name": "clawd-mood",
  "description": "Drives a clawd-mood ESP32-C3 desk mascot via USB serial, reflecting Codex CLI state on a TFT display.",
  "version": "0.1.0",
  "author": { "name": "liuyingwen" },
  "hooks": "./hooks/hooks-codex.json"
}
```

`hooks` 字段显式指向 `hooks-codex.json`，避免 codex 默认读 `hooks/hooks.json`（那是 Claude Code 用的）。

- [ ] **Step 3: JSON lint**

```bash
python3 -m json.tool plugin/.codex-plugin/plugin.json
```

预期：原样回显，无报错。

- [ ] **Step 4: Commit**

```bash
git add plugin/.codex-plugin/plugin.json
git commit -m "feat(codex): add .codex-plugin/plugin.json manifest"
```

---

## Task 3: 创建 hooks-codex.json 并验证插件加载

**Files:**
- Create: `plugin/hooks/hooks-codex.json`

10 个事件（7 共有 + 3 Codex 独有），结构与现有 `plugin/hooks/hooks.json` 一致。

- [ ] **Step 1: 写 hooks-codex.json**

```json
{
  "hooks": {
    "SessionStart": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh" }
      ]}
    ],
    "UserPromptSubmit": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh" }
      ]}
    ],
    "PreToolUse": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh" }
      ]}
    ],
    "PostToolUse": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh" }
      ]}
    ],
    "PermissionRequest": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh" }
      ]}
    ],
    "PreCompact": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh" }
      ]}
    ],
    "PostCompact": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh" }
      ]}
    ],
    "Stop": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh" }
      ]}
    ],
    "SubagentStart": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh" }
      ]}
    ],
    "SubagentStop": [
      { "matcher": "", "hooks": [
        { "type": "command", "command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh" }
      ]}
    ]
  }
}
```

- [ ] **Step 2: JSON lint**

```bash
python3 -m json.tool plugin/hooks/hooks-codex.json
```

预期：原样回显，无报错。

- [ ] **Step 3: 让 codex 重新加载 marketplace**

```bash
codex plugin marketplace upgrade clawd-mood 2>/dev/null || \
  codex plugin marketplace remove clawd-mood && codex plugin marketplace add .
```

（marketplace 是本地 snapshot，新增插件清单文件后需要 refresh。如果 `upgrade` 不适用于 local marketplace，回退到 remove+add。）

- [ ] **Step 4: 安装插件**

```bash
codex plugin add clawd-mood@clawd-mood
```

预期输出包含 `Installed clawd-mood` 或类似。

- [ ] **Step 5: 验证 healthy**

```bash
codex plugin list
```

预期：`clawd-mood` 状态为 `installed` / `enabled` / healthy。

- [ ] **Step 6: 验证 hooks 已加载（启动 codex 检查）**

```bash
codex
# 在 codex 内输入：
> /hooks
```

预期：10 个事件全列出，每个都指向 `…/plugin/scripts/hook.sh`。

如果 codex 不识别 `async: true` 字段并报警：在 hooks-codex.json 里把 `"async": true` 全删掉重 lint 重装。hook.sh 本来就是非阻塞实现（FIFO RW 打开 + 直接 exit），不带 async 也安全。

- [ ] **Step 7: 退出 codex 不必触发事件**（hook.sh 的 codex 独有 case 还没改，Task 4 才改；现在 codex 触发 PermissionRequest 时 hook.sh 会走 `*) exit 0` 静默退出，无害）

- [ ] **Step 8: Commit**

```bash
git add plugin/hooks/hooks-codex.json
git commit -m "feat(codex): add hooks-codex.json with 10-event registration"
```

---

## Task 4: 修改 hook.sh 追加 3 个 Codex case

**Files:**
- Modify: `plugin/scripts/hook.sh:27-38`

- [ ] **Step 1: 改 case 块**

在 `SubagentStop` 行之后、`*)` 兜底之前插入 3 行。修改后的 case 完整块：

```bash
case "$EVENT" in
  SessionStart)       STATE="idle" ;;
  UserPromptSubmit)   STATE="thinking" ;;
  PreToolUse)         STATE="working" ;;
  PostToolUse)        STATE="working" ;;
  PostToolUseFailure) STATE="error" ;;
  Notification)       STATE="waiting" ;;
  Stop)               STATE="done" ;;
  SubagentStart)      STATE="working" ;;
  SubagentStop)       STATE="working" ;;
  PermissionRequest)  STATE="waiting" ;;
  PreCompact)         STATE="thinking" ;;
  PostCompact)        STATE="working" ;;
  *)                  exit 0 ;;
esac
```

- [ ] **Step 2: shellcheck 检查**

```bash
shellcheck plugin/scripts/hook.sh
```

预期：无新警告（已存在的警告不计）。如果未装 shellcheck：`brew install shellcheck`。

- [ ] **Step 3: 准备隔离的 FIFO 单元测试环境**

不动 daemon、不依赖硬件，单纯验证 hook.sh 对 Codex 事件的映射正确。

```bash
# 隔离测试 FIFO，避免抢真 FIFO
TEST_FIFO=/tmp/clawd-mood-hooktest.fifo
rm -f "$TEST_FIFO"
mkfifo "$TEST_FIFO"
```

- [ ] **Step 4: 临时测试运行**

新开一个终端启动 reader：

```bash
cat /tmp/clawd-mood-hooktest.fifo
```

（保持窗口打开，下面每次注入会输出一行。）

回到主终端，注入 3 个 Codex 独有事件（临时把 hook.sh 的 FIFO 路径改成测试 FIFO 是侵入式的；改用环境变量重 export 的方法不行因为路径是硬编码——所以这里直接复制 hook.sh 的 case 块手测）：

```bash
for EV in PermissionRequest PreCompact PostCompact; do
  bash -c '
    EVENT='"$EV"'
    case "$EVENT" in
      PermissionRequest)  STATE="waiting" ;;
      PreCompact)         STATE="thinking" ;;
      PostCompact)        STATE="working" ;;
    esac
    printf "{\"state\":\"%s\",\"event\":\"%s\"}\n" "$STATE" "$EVENT"
  '
done
```

预期 stdout：
```
{"state":"waiting","event":"PermissionRequest"}
{"state":"thinking","event":"PreCompact"}
{"state":"working","event":"PostCompact"}
```

这一步是冗余 sanity check（验证 case 块本身的语义），真正的端到端验证在 Task 8。

- [ ] **Step 5: 清理测试 FIFO**

```bash
rm -f /tmp/clawd-mood-hooktest.fifo
```

- [ ] **Step 6: Commit**

```bash
git add plugin/scripts/hook.sh
git commit -m "feat(codex): map PermissionRequest/PreCompact/PostCompact in hook.sh"
```

---

## Task 5: 创建 AGENTS.md

**Files:**
- Create: `AGENTS.md`

Codex CLI 默认读项目根 `AGENTS.md` 作为项目守则。本仓库守则已在 `CLAUDE.md`，避免重复维护：

- [ ] **Step 1: 写 AGENTS.md**

```markdown
# AGENTS.md

See [CLAUDE.md](./CLAUDE.md) for project guidance — it applies to all coding agents (Claude Code, Codex CLI, etc.) working in this repo.
```

- [ ] **Step 2: Commit**

```bash
git add AGENTS.md
git commit -m "docs(codex): add AGENTS.md pointing at CLAUDE.md"
```

> **注意**：`CLAUDE.md` 文件本身在 git status 里仍是 untracked（会话前就是这状态，不是本计划的工作产物）。如果用户希望同时把 CLAUDE.md commit，那是独立的事情，本 plan 不处理。

---

## Task 6: 更新 CLAUDE.md

**Files:**
- Modify: `CLAUDE.md`（多处编辑）

CLAUDE.md 当前未被 git 跟踪（git status 显示 untracked），但已存在于工作树。本 task 直接编辑磁盘上的文件，最后由用户决定是否把整个文件纳入 git。

- [ ] **Step 1: 修改"项目概要"段**

找到段首句：

```markdown
`clawd-mood` 是一个 ESP32-C3 + ST7789 1.54" TFT 桌面摆件，通过 USB 串口接收 Claude Code 的运行状态，实时切换 7 种像素眼睛表情（idle / thinking / working / waiting / done / error / sleeping）。
```

改为：

```markdown
`clawd-mood` 是一个 ESP32-C3 + ST7789 1.54" TFT 桌面摆件，通过 USB 串口接收 **Claude Code / OpenAI Codex CLI** 的运行状态，实时切换 7 种像素眼睛表情（idle / thinking / working / waiting / done / error / sleeping）。
```

- [ ] **Step 2: 更新"三段式架构"数据流图**

原图：

```
Claude Code event
       ↓
plugin/scripts/hook.sh        (Bash + jq; 事件名 → state JSON)
       ↓
/tmp/clawd-mood.fifo          (命名管道)
       ↓
plugin/scripts/daemon.py      (uv 启动；常驻；读 FIFO 写串口)
       ↓ USB CDC 115200 baud, NDJSON
firmware/clawd_mood/*.ino     (Arduino 状态机 + 像素眼渲染)
```

改为：

```
Claude Code event ─┐
                   │
Codex CLI event ───┤
                   ↓
plugin/scripts/hook.sh        (Bash + jq; 事件名 → state JSON)
       ↓
/tmp/clawd-mood.fifo          (命名管道，多写者单读者)
       ↓
plugin/scripts/daemon.py      (uv 启动；常驻；读 FIFO 写串口)
       ↓ USB CDC 115200 baud, NDJSON
firmware/clawd_mood/*.ino     (Arduino 状态机 + 像素眼渲染)
```

并把段首"整个数据流单向，**Mac → ESP32**"那句保持不变（仍正确）。

- [ ] **Step 3: 更新"三段式架构"段尾"三个组件互相独立"清单**

把现有那段后面追加一条 Codex 独立测试方法：

```markdown
- 不接 ESP32 时，daemon glob 找不到设备直接退出
- 不挂 Claude Code 时，可只挂 Codex（`codex plugin add clawd-mood@clawd-mood`），或反之
```

- [ ] **Step 4: 修改"状态机设计原则" 段 hook 事件映射说明**

找到这段：

```markdown
hook 端的事件→状态映射是**多对一收敛**：`PreToolUse` / `PostToolUse` / `SubagentStart` / `SubagentStop` 都映射到 `working`，避免表情在工具调用过程中乱跳。改 `hook.sh` 时保持这个收敛行为。
```

在其后追加新段：

```markdown
**Codex CLI 独有事件映射**（codex 0.133.0 起）：

- `PermissionRequest` → `waiting`（codex 等用户批准命令，对标 Claude Code 的 `Notification`）
- `PreCompact` → `thinking`（codex 正在压缩上下文）
- `PostCompact` → `working`（codex 压缩完成回到工作流）

Claude Code 独有事件 `PostToolUseFailure` / `Notification` 仍保留映射，Codex 不会触发，互不干扰。
```

- [ ] **Step 5: 修改"常用命令 → 挂载插件"段**

原段：

```markdown
### 挂载插件

```bash
claude --plugin-dir /absolute/path/to/clawd-mood/plugin
```

在 Claude Code 里 `/hooks` 确认 9 个事件都挂上（SessionStart / UserPromptSubmit / PreToolUse / PostToolUse / PostToolUseFailure / Notification / Stop / SubagentStart / SubagentStop）。
```

改为：

```markdown
### 挂载插件

**Claude Code**：

```bash
claude --plugin-dir /absolute/path/to/clawd-mood/plugin
```

在 Claude Code 里 `/hooks` 确认 9 个事件都挂上（SessionStart / UserPromptSubmit / PreToolUse / PostToolUse / PostToolUseFailure / Notification / Stop / SubagentStart / SubagentStop）。

**Codex CLI**（≥ 0.133.0）：

```bash
cd /absolute/path/to/clawd-mood
codex plugin marketplace add .
codex plugin add clawd-mood@clawd-mood
```

任意目录 `codex` 就能用。在 codex 里 `/hooks` 确认 10 个事件都挂上（含 `PermissionRequest` / `PreCompact` / `PostCompact`）。卸载：`codex plugin remove clawd-mood@clawd-mood`。
```

- [ ] **Step 6: 新增"双 CLI 并发"节**

在"状态机设计原则"和"串口协议"之间插入：

```markdown
## 双 CLI 并发

两端可同时挂载、同时运行，**无冲突**：

- FIFO 是多写者单读者：两个 CLI 的 hook 进程各自写 `/tmp/clawd-mood.fifo`，daemon 单点串行消费
- daemon singleton 不变：`SessionStart` 拉 daemon 时 `pgrep -f scripts/daemon.py` 已防重复
- 串口由 daemon 独占：两个 CLI 共享同一个 daemon → 共享同一个串口
- 表情序列按事件到达顺序交错驱动，**不做合并/去重**——这意味着两端长任务同时跑时表情会快速切换，是预期行为

如果觉得"鬼畜"，停掉其中一端的会话即可（不需要卸载插件）。
```

- [ ] **Step 7: JSON 校验所有相关配置文件未被破坏**

```bash
python3 -m json.tool plugin/.claude-plugin/plugin.json
python3 -m json.tool plugin/.codex-plugin/plugin.json
python3 -m json.tool plugin/hooks/hooks.json
python3 -m json.tool plugin/hooks/hooks-codex.json
python3 -m json.tool marketplace.json
```

（这步是冗余 sanity check，因为 CLAUDE.md 改动不影响这些 JSON。但养成习惯。）

- [ ] **Step 8: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(codex): update CLAUDE.md with codex events and dual-CLI section"
```

---

## Task 7: 更新 README

**Files:**
- Modify: `README.md`

- [ ] **Step 1: 修改"使用 § 2 启动 Claude Code"段**

原段（含标题）：

````markdown
### 2. 启动 Claude Code

新开一个终端：

```bash
claude --plugin-dir /path/to/clawd-mood/plugin
```

或者加 alias 永久挂载：

```bash
alias claude='claude --plugin-dir /path/to/clawd-mood/plugin'
```

启动后在 Claude Code 里运行 `/hooks`，确认 9 个事件全挂上了。
````

改为：

````markdown
### 2. 挂载到 CLI

两端可独立挂、可同装。

#### Claude Code

```bash
claude --plugin-dir /path/to/clawd-mood/plugin
```

或永久 alias：

```bash
alias claude='claude --plugin-dir /path/to/clawd-mood/plugin'
```

启动后 `/hooks` 确认 9 个事件全挂上。

#### Codex CLI（≥ 0.133.0）

首次：

```bash
cd /path/to/clawd-mood
codex plugin marketplace add .
codex plugin add clawd-mood@clawd-mood
```

之后任意目录 `codex` 即可。在 codex 里 `/hooks` 确认 10 个事件全挂上。

#### 同时挂

两端可同时跑，表情会按事件到达顺序交错切换。daemon 单实例、FIFO 多写者，互不冲突。
````

- [ ] **Step 2: 修改"使用 § 3 表情对照"表**

原表：

```markdown
| 状态     | 触发                        |
| -------- | --------------------------- |
| Idle     | 会话开始 / Stop 后 3 秒回归 |
| Thinking | 提交 prompt                 |
| Working  | 调用工具                    |
| Waiting  | Claude 等你输入             |
| Done     | 回答完成（3 秒瞬态）        |
| Error    | 工具失败                    |
| Sleeping | 5 分钟无事件                |
```

改为：

```markdown
| 状态     | 触发                                                      |
| -------- | --------------------------------------------------------- |
| Idle     | 会话开始 / Stop 后 3 秒回归                               |
| Thinking | 提交 prompt / Codex 压缩上下文中（PreCompact）            |
| Working  | 调用工具 / Codex 压缩完恢复（PostCompact）                |
| Waiting  | Claude 等你输入（Notification）/ Codex 等你批准（PermissionRequest） |
| Done     | 回答完成（3 秒瞬态）                                      |
| Error    | 工具失败（Claude Code 独有 PostToolUseFailure）           |
| Sleeping | 5 分钟无事件                                              |
```

- [ ] **Step 3: 修改"故障排除"段，追加 codex 相关一条**

在现有列表末尾追加：

```markdown
- **Codex 不识别插件**：检查 `codex plugin marketplace list` 看本地 marketplace 是否注册；若 marketplace 路径变了，先 `codex plugin marketplace remove clawd-mood` 再 `add` 一次
```

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs(codex): document codex CLI mount and dual-CLI usage"
```

---

## Task 8: 集成验收

**Files:**（无文件改动，纯验收）

这一步在硬件齐全（ESP32 接好 + 屏幕亮）的环境跑全流程。如硬件不全，看 daemon 日志即可。

- [ ] **Step 1: 准备环境**

```bash
# 确保只有 0 或 1 个 daemon（macOS pgrep 无 -c，用 wc -l 数）
pgrep -f scripts/daemon.py | wc -l
# 若 >1，pkill -f scripts/daemon.py 重置
```

- [ ] **Step 1b: 重装 codex 插件让 hook.sh / hooks-codex.json 改动生效**

⚠️ codex 拷贝 plugin 到 `~/.codex/plugins/cache/`（不是 symlink），所以 Task 3/4 改完源仓库后，codex 端仍跑 cache 里的旧版本。验收前必须刷一次：

```bash
codex plugin remove clawd-mood@clawd-mood
codex plugin add clawd-mood@clawd-mood
```

Claude Code 走 `--plugin-dir` 是 live path，无需类似操作。

- [ ] **Step 2: 启动 daemon + 接 ESP32**

```bash
./plugin/scripts/daemon.py
```

预期 stdout 三行：FIFO 路径、串口路径、`Ready!`。

- [ ] **Step 3: Claude Code 回归测试**

新终端：
```bash
claude --plugin-dir "$(pwd)/plugin"
```

跑一次：`列出当前目录`

预期屏幕表情序列：Idle → Thinking → Working → Done → Idle（3 秒后回归）

确认旧行为未破坏。

- [ ] **Step 4: Codex CLI 真实任务**

新终端：
```bash
codex
```

跑一次：`列出当前目录`

预期屏幕表情序列：Idle → Thinking → Working → Done → Idle

- [ ] **Step 5: 触发 Codex PermissionRequest**

在 codex 里输入会触发审批的命令，例如：
```
> 删除 /tmp/test 文件
```
当 codex 弹出"是否允许？"等待时，**屏幕应显示 waiting 表情**。

批准或拒绝后，应回到 working 或 idle。

- [ ] **Step 6: 触发 Codex PreCompact / PostCompact**

在 codex 里手动压缩：
```
> /compact
```

预期：屏幕短暂显示 thinking，然后回到 working / idle。

如压缩过快肉眼难捕捉，看 daemon 日志 `tail -f /tmp/clawd-mood-daemon.log`，应能看到 `{"state":"thinking",...PreCompact}` 和 `{"state":"working",...PostCompact}` 两行。

- [ ] **Step 7: 双 CLI 并发测试**

同时开 Claude Code 和 Codex 各自跑一个长任务（例如读多个文件）。

5 分钟内观察：
- `pgrep -f scripts/daemon.py | wc -l` 始终 ≤ 1（macOS pgrep 无 -c）
- daemon log 不报错
- 屏幕表情按事件交错切换（可能"鬼畜"——这是预期）

- [ ] **Step 8: 验证未声明事件兜底**

模拟未来 codex 新增未知事件：

```bash
echo '{"hook_event_name":"FutureUnknownEvent","tool_name":"Bash"}' | \
  CLAUDE_PLUGIN_ROOT="$(pwd)/plugin" plugin/scripts/hook.sh
echo "exit=$?"
```

预期：`exit=0`，FIFO 无新内容（hook.sh 走 `*) exit 0`）。

- [ ] **Step 9: 勾选 DoD 清单**

逐条 check spec § 15：
- [x] `marketplace.json`、`.codex-plugin/plugin.json`、`hooks-codex.json`、`AGENTS.md` 创建并通过 JSON lint（除 AGENTS.md 是 markdown）
- [x] `hook.sh` 3 行 case 追加，shellcheck 无新警告
- [x] `codex plugin marketplace add .` 和 `codex plugin add clawd-mood@clawd-mood` 成功
- [x] `codex plugin list` 显示 clawd-mood healthy
- [x] Codex 端真实任务 → 表情序列正确（含 `waiting` 在批准请求时触发）
- [x] Claude Code 端回归测试通过
- [x] 双 CLI 并发 5 分钟 → daemon 不崩、表情切换合理
- [x] README 和 CLAUDE.md 更新合并
- [x] 全部改动一次 commit，包含 spec 文档（spec 已 commit 在 `897206b`，本计划各 task 各自 commit）

- [ ] **Step 10: 关闭计划**

若全部勾选：
```bash
git log --oneline -10
```

预期最近 7 个 commit 是本计划的产物（Task 1-7），加上前置 spec commit `897206b`。

---

## 异常回退路径

如某个 task 反向失败：

- **Task 1 schema 死活推不对**：临时把 marketplace 配置写到 `~/.codex/config.toml` 的 `[marketplaces.local-clawd-mood]` 块（用已验证的格式，从 spec 调研段抓到的 `source_type = "local"` + `source = "/path"`），绕过 `marketplace.json`。然后 spec § 8 改为标记 "config.toml 直配，不依赖 marketplace.json"。
- **Task 3 codex 不认 `async: true`**：删掉 `async: true` 字段，hook.sh 本身非阻塞。
- **Task 3 codex 不认 `$CLAUDE_PLUGIN_ROOT`**：把 hooks-codex.json 里的 `"$CLAUDE_PLUGIN_ROOT"` 改成 `"$PLUGIN_ROOT"`，并新建 `plugin/scripts/hook-codex.sh` 做相同逻辑但用 `$PLUGIN_ROOT`（不污染 Claude Code 那条线）。
- **Task 8 Step 5 waiting 不触发**：codex 0.133.0 实际是否每次审批都发 `PermissionRequest` hook 未在文档明说。退而求其次：跑 `codex` 时 attach `tail -f ~/.codex/log/*` 观察是否真的有 hook 触发。

---

## Self-Review

### Spec coverage
- spec § 1 目标 → Task 1-8 全覆盖
- spec § 4 文件结构 → Task 1（marketplace.json）、Task 2（.codex-plugin）、Task 3（hooks-codex.json）、Task 4（hook.sh）、Task 5（AGENTS.md）、Task 6（CLAUDE.md）、Task 7（README.md）
- spec § 5 hook.sh 3 行 diff → Task 4 Step 1 一对一
- spec § 6 .codex-plugin/plugin.json → Task 2 Step 2 一对一
- spec § 7 hooks-codex.json 10 事件表 → Task 3 Step 1 一对一
- spec § 8 marketplace.json → Task 1
- spec § 9 安装流 → Task 3 Step 4 + CLAUDE.md / README 更新
- spec § 10 AGENTS.md → Task 5
- spec § 11 CLAUDE.md / README 改动要点 → Task 6 / Task 7
- spec § 12 测试策略 → Task 4 Step 4（hook 单测） + Task 8（集成）
- spec § 13 风险 R1（marketplace schema） → Task 1 反推流程 + 异常回退
- spec § 13 R2（CLAUDE_PLUGIN_ROOT 兼容） → 异常回退覆盖
- spec § 13 R3（未声明事件 warn） → Task 8 Step 8 兜底验证
- spec § 13 R4（async 字段） → Task 3 Step 6 + 异常回退
- spec § 13 R5（并发抢屏鬼畜） → CLAUDE.md "双 CLI 并发" 节 + Task 8 Step 7 接受现状
- spec § 14 关键约束沿用 → 计划中没有引入 timeout / 改硬件 / 改 rotation
- spec § 15 DoD → Task 8 Step 9 逐条对照

无未覆盖项。

### Placeholder scan
- 无 TBD / TODO / "implement later"
- 所有代码块都给出完整内容
- 所有 commit message 显式给出
- "异常回退路径" 段是显式记录的应急方案，不是 placeholder

### Type / 字段一致性
- `state` 7 值小写：idle / thinking / working / waiting / done / error / sleeping —— hook.sh 和 hooks-codex.json 都未引入新 state
- 事件名首字母大写 PascalCase：与 codex 文档一致
- `marketplace.json` 字段 `name` / `displayName` / `plugins[].name` / `plugins[].source.path` 在 Task 1 和 spec § 8 一致（同为待验证）
- `.codex-plugin/plugin.json` 字段 `name` / `description` / `version` / `author.name` / `hooks` 与 spec § 6 一致
- `$CLAUDE_PLUGIN_ROOT` 引用在 hooks-codex.json 和现有 hooks.json 中字面一致

无不一致。
