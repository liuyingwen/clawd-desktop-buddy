# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概要

`clawd-mood` 是一个 ESP32-C3 + ST7789 1.54" TFT 桌面摆件，通过 USB 串口接收 **Claude Code / OpenAI Codex CLI** 的运行状态，实时切换 7 种像素眼睛表情（idle / thinking / working / waiting / done / error / sleeping）。

**仅支持 macOS**。明确不做：WiFi 控制、OTA、launchd 自启、Windows/Linux 兼容、自动化单元测试、蜂鸣器、多设备。

## 三段式架构

整个数据流单向，**Mac → ESP32**：

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

三个组件**互相独立**，可单独测试：
- 不启动 daemon 时，hook.sh 检测 FIFO 不存在直接静默退出（不阻塞 Claude Code / Codex）
- 不启动 CLI 时，可直接 `echo '{"state":"working"}' > /tmp/clawd-mood.fifo` 测 daemon + 固件
- 不接 ESP32 时，daemon glob 找不到设备直接退出
- 不挂 Claude Code 时，可只挂 Codex（`codex plugin add clawd-mood@clawd-mood`），或反之；两端可独立、可同装

## 关键约束（容易踩坑）

### 1. macOS 没有 `timeout` 命令
`hook.sh` 写 FIFO 时**不能**用 `timeout 1 bash -c ...` —— 那是 GNU coreutils，stock macOS 没有。必须用 bash 原生的读写打开模式防阻塞：

```bash
exec 3<>"$FIFO"
printf '...' >&3
exec 3>&-
```

修改 `hook.sh` 时如果重新引入 timeout 兜底，会在没装 coreutils 的机器上静默坏掉。参考 commit `0c1da90` 和 `4242ed2`。

### 2. 显示器 VCC 接 3.3V，**不要接 5V**
ST7789 的 VCC 接到 ESP32-C3 的 3V3 引脚。接 5V 会烧屏。README 烧录段已写明，改文档时不要弄错。

### 3. `USB CDC On Boot` 必须 Enabled
板子在 Arduino IDE 配置里必须开 USB CDC On Boot。关掉了 daemon 看不到串口、屏幕也不会更新。`-b esp32:esp32:esp32c3:CDCOnBoot=cdc,...` 这串参数里的 `CDCOnBoot=cdc` 就是这意思。

### 4. SPI 引脚需要手动 remap
ESP32-C3 用 `SPI.begin(8, -1, 10, TFT_CS)` 显式指定 SCK=8 / MOSI=10，不要用默认值。

### 5. 屏幕方向
`tft.setRotation(1)` 是当前用户面板的安装方向。`rotation` 改 0/2/3 都是合法值，但**只在用户报告方向不对时**才改。参考 commit `e8f6e6e`（之前从 2 改到 1）。

### 6. 多 USB CDC 设备
连了多台 ESP32 时，daemon 默认拿 `sorted(glob.glob("/dev/cu.usbmodem*"))[0]`。要指定其他端口用 `CLAWD_MOOD_PORT=/dev/cu.usbmodemXXX ./plugin/scripts/daemon.py`。

### 7. 插件级 daemon 自启 ≠ launchd 自启
`hook.sh` 在 `SessionStart` 事件里会自动 `pgrep` 检测 `daemon.py`，没跑就 `nohup … & disown` 起一个脱离的后台进程。这是**插件级 / 会话触发**的自启：只在 Claude Code 第一次启动时拉起 daemon，不依赖系统服务。"明确不做：launchd 自启"指的仍是系统级 LaunchAgents/LaunchDaemons —— 我们不写 `.plist` 让 daemon 跟随开机/登录启动。两者不冲突。

实现上的取舍：
- **Singleton 用 `pgrep -f "$DAEMON"` 而非 PID 文件**：避免脏 PID、避免改 daemon.py。代价是命令行字符串匹配，对 cmdline 不含路径的极端情况会失效，但实际上 `nohup`/`uv run` 都会把 `daemon.py` 路径写进 argv，够稳。
- **首条 SessionStart 状态消息会丢**：daemon 冷启动约 2–3 秒（uv 首次装 pyserial），此时 FIFO 还没建好，`hook.sh` 就走 `[ -p "$FIFO" ] || exit 0` 退出。下一条 `UserPromptSubmit` 会把状态补推上去，所以表情滞后但不会卡。
- **daemon 永驻**：不在 `Stop` 事件里关 daemon，避免多个 Claude Code 会话相互踩踏。需要重启 daemon 就手动 `pkill -f scripts/daemon.py`。
- **日志位置**：`/tmp/clawd-mood-daemon.log`（追加模式 nohup 默认行为；如需查问题直接 `tail -f`）。

## 状态机设计原则

7 个状态由 hook → daemon → 固件单向推送。**有两个转移在固件本地完成**，不靠上游：

- `Done` 进入后 3 秒（`DONE_REVERT_MS`）自动回 `Idle` —— done 是瞬态，否则会卡在笑脸
- 任何状态在 5 分钟（`SLEEP_IDLE_MS`）没有串口消息时进入 `Sleeping`
- 任何串口消息立即唤醒 `Sleeping`

hook 端的事件→状态映射是**多对一收敛**：`PreToolUse` / `PostToolUse` / `SubagentStart` / `SubagentStop` 都映射到 `working`，避免表情在工具调用过程中乱跳。改 `hook.sh` 时保持这个收敛行为。

**Codex CLI 独有事件映射**（codex 0.133.0 起）：

- `PermissionRequest` → `waiting`（codex 等用户批准命令，对标 Claude Code 的 `Notification`）
- `PreCompact` → `thinking`（codex 正在压缩上下文）
- `PostCompact` → `working`（codex 压缩完成回到工作流）

Claude Code 独有事件 `PostToolUseFailure` / `Notification` 仍保留映射，Codex 不会触发，互不干扰。

state 枚举（小写、严格匹配）：`idle | thinking | working | waiting | done | error | sleeping`。固件里 `parseMood` 用 `strcmp`，**大小写敏感**。

## 双 CLI 并发

两端可同时挂载、同时运行，**无冲突**：

- FIFO 是多写者单读者：两个 CLI 的 hook 进程各自写 `/tmp/clawd-mood.fifo`，daemon 单点串行消费
- daemon singleton 不变：`SessionStart` 拉 daemon 时 `pgrep -f scripts/daemon.py` 已防重复
- 串口由 daemon 独占：两个 CLI 共享同一个 daemon → 共享同一个串口
- 表情序列按事件到达顺序交错驱动，**不做合并/去重**——这意味着两端长任务同时跑时表情会快速切换，是预期行为

如果觉得"鬼畜"，停掉其中一端的会话即可（不需要卸载插件）。

## 串口协议

- 方向：**单向** Mac → ESP32，ESP32 不回包
- 格式：NDJSON（换行分隔），UTF-8，115200 baud
- 必需字段：`state`
- 可选调试字段：`event`、`tool`
- 未知 `state` 或坏 JSON：固件打 `[warn]` 到串口，不切换、不死

示例：`{"state":"working","event":"PreToolUse","tool":"Bash"}`

## 常用命令

### 烧固件

```bash
# 编译 + 上传（端口名按实际改）
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
arduino-cli upload -p /dev/cu.usbmodem101 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

板子检测：`ls /dev/cu.usbmodem*`

首次环境（README §1-3）：`brew install arduino-cli uv jq` + ESP32 核心 + Adafruit GFX / ST7789 / ArduinoJson 库。

### 跑 daemon

```bash
./plugin/scripts/daemon.py
# 或指定端口
CLAWD_MOOD_PORT=/dev/cu.usbmodem101 ./plugin/scripts/daemon.py
# 日志不缓冲
PYTHONUNBUFFERED=1 ./plugin/scripts/daemon.py
```

`daemon.py` 头部是 PEP 723 内联依赖，`uv run` 自动建临时 venv 装 pyserial。不需要手动 `pip install`。

### 挂载插件

**Claude Code**：

```bash
claude --plugin-dir /absolute/path/to/clawd-mood/plugin
```

在 Claude Code 里 `/hooks` 确认 9 个事件都挂上（SessionStart / UserPromptSubmit / PreToolUse / PostToolUse / PostToolUseFailure / Notification / Stop / SubagentStart / SubagentStop）。

**Codex CLI**（≥ 0.133.0）：

```bash
cd /absolute/path/to/clawd-mood
codex plugin marketplace add .          # 注册本地 marketplace（读 .agents/plugins/marketplace.json）
codex plugin add clawd-mood@clawd-mood  # 装插件（拷贝到 ~/.codex/plugins/cache/）
```

任意目录 `codex` 就能用。在 codex 里 `/hooks` 确认 10 个事件都挂上（含 `PermissionRequest` / `PreCompact` / `PostCompact`）。卸载：`codex plugin remove clawd-mood`。

**Codex 改 plugin 后必须重装**：codex 是把 plugin 内容**拷贝**到 `~/.codex/plugins/cache/clawd-mood/clawd-mood/0.1.0/`，不是 symlink。修改了源仓库的 `plugin/scripts/hook.sh` / `hooks/hooks-codex.json` 后，codex 仍跑 cache 里的旧版本。开发时改完执行：

```bash
codex plugin remove clawd-mood
codex plugin add clawd-mood@clawd-mood
```

Claude Code 走 `--plugin-dir` 是 live path，无此问题。

### 手动测试（不依赖 Claude Code）

```bash
# 灌 FIFO 走完整管道（daemon → 串口 → 屏幕）
echo '{"state":"working"}' > /tmp/clawd-mood.fifo

# 直接喂 hook.sh（模拟 Claude Code 事件）
echo '{"hook_event_name":"PreToolUse","tool_name":"Bash"}' | ./plugin/scripts/hook.sh

# Arduino 串口监视器手输（绕过 daemon）
# 在 IDE 监视器里输：{"state":"idle"} 等
```

JSON 校验：`python3 -m json.tool plugin/hooks/hooks.json`

## 测试策略

**没有自动化测试**——硬件依赖重、IO 逻辑短，靠手测 + 集成测覆盖：

- **固件单独**：Arduino 串口监视器手输 7 种 JSON，肉眼确认 7 表情
- **daemon + 固件**：`echo … > /tmp/clawd-mood.fifo`
- **端到端**：跑真实任务（"列出当前目录"），观察 Idle → Thinking → Working → Done → Idle 序列
- **Done 3 秒回 Idle、Sleeping 5 分钟进入**：临时把 `SLEEP_IDLE_MS` 改成 `15000UL` 缩短验证

完整测试清单见 `docs/superpowers/plans/2026-06-07-clawd-mood-implementation.md` Task 13 / 17。

## 仓库结构要点

- `firmware/clawd_mood/clawd_mood.ino` —— 单文件 Arduino sketch；7 个 `drawXxx()` 函数 + `tickMoodMachine()` 计时器 + `pollSerial()` 拼行 + `handleLine()` JSON 解析
- `plugin/.claude-plugin/plugin.json` —— Claude Code 插件清单
- `plugin/.codex-plugin/plugin.json` —— Codex CLI 插件清单（指向 `hooks-codex.json`）
- `plugin/hooks/hooks.json` —— Claude Code 9 个事件全 `async: true`，命令统一指 `"$CLAUDE_PLUGIN_ROOT"/scripts/hook.sh`
- `plugin/hooks/hooks-codex.json` —— Codex 10 个事件（含 PermissionRequest / PreCompact / PostCompact），同一个 hook.sh
- `plugin/scripts/hook.sh` —— event → state 映射，写 FIFO（共享给两端）
- `plugin/scripts/daemon.py` —— PEP 723 内联依赖，FIFO ↔ 串口桥
- `.agents/plugins/marketplace.json` —— Codex 本地 marketplace 入口
- `AGENTS.md` —— Codex 项目守则，指回本文件
- `docs/superpowers/specs/2026-06-07-clawd-mood-design.md` —— 原始设计说明书（**改架构前先读这个**）
- `docs/superpowers/specs/2026-06-07-codex-support-design.md` —— Codex 支持设计说明书
- `docs/superpowers/plans/2026-06-07-clawd-mood-implementation.md` —— 原始 18 任务实施计划
- `docs/superpowers/plans/2026-06-07-codex-support-implementation.md` —— Codex 支持 8 任务实施计划

修改设计时同步更新 `docs/superpowers/specs/` 下的文档。
