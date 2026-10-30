# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概要

`clawd-mood` 是一个 ESP32-C3 + ST7789 1.54" TFT 桌面摆件，通过 USB 串口接收 **Claude Code / OpenAI Codex CLI** 的运行状态，以「桌面小队」形式实时显示多个会话：每个会话是一只像素小生物，同时显示项目名和 7 种状态之一（idle / thinking / working / waiting / done / error / sleeping）。

**跨平台桌面摆件（macOS / Linux / Windows）**。明确不做：WiFi 控制、OTA、launchd/systemd 系统级自启、自动化单元测试、蜂鸣器、多设备。Windows 端代码完成但 **untested on Windows** —— 欢迎社区验证（见 README）。Linux 同样未在 CI 验证，但 POSIX 路径与 macOS 共用，回归风险低。

## 三段式架构

整个数据流单向，**Mac → ESP32**：

```
Claude Code event ─┐
                   │
Codex CLI event ───┤
                   ↓
plugin/scripts/hook.py        (uv run; 标准库 only; 事件名 → state JSON)
       ↓
TCP 127.0.0.1:<port>          (portfile: <tempdir>/clawd-mood.port)
       ↓
plugin/scripts/daemon.py      (uv 启动；常驻；TCP server 写串口)
       ↓ USB CDC 115200 baud, NDJSON
firmware/clawd_mood/*.ino     (Arduino 状态机 + 像素眼渲染)
```

三个组件**互相独立**，可单独测试：
- 不启动 daemon 时，hook.py 读 portfile 拿不到端口（或连不上）直接静默退出（不阻塞 Claude Code / Codex）
- 不启动 CLI 时，可直接 `printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat ${TMPDIR:-/tmp}/clawd-mood.port)` 测 daemon + 固件（Windows 见下文"手动测试"）
- 不接 ESP32 时，daemon 找不到 USB CDC 设备直接退出
- 不挂 Claude Code 时，可只挂 Codex（`codex plugin add clawd-mood@clawd-mood`），或反之；两端可独立、可同装

## 关键约束（容易踩坑）

### 1. 显示器 VCC 接 3.3V，**不要接 5V**
ST7789 的 VCC 接到 ESP32-C3 的 3V3 引脚。接 5V 会烧屏。README 烧录段已写明，改文档时不要弄错。

### 2. `USB CDC On Boot` 必须 Enabled
板子在 Arduino IDE 配置里必须开 USB CDC On Boot。关掉了 daemon 看不到串口、屏幕也不会更新。`-b esp32:esp32:esp32c3:CDCOnBoot=cdc,...` 这串参数里的 `CDCOnBoot=cdc` 就是这意思。

### 3. SPI 引脚需要手动 remap
ESP32-C3 用 `SPI.begin(8, -1, 10, TFT_CS)` 显式指定 SCK=8 / MOSI=10，不要用默认值。

### 4. 屏幕方向
`tft.setRotation(1)` 是当前用户面板的安装方向。`rotation` 改 0/2/3 都是合法值，但**只在用户报告方向不对时**才改。参考 commit `e8f6e6e`（之前从 2 改到 1）。

### 5. 多 USB CDC 设备
连了多台 ESP32 时，daemon 默认拿排序后的第一个，扫描规则跨平台：
- macOS: `/dev/cu.usbmodem*`
- Linux: `/dev/ttyACM*`（ESP32-C3 原生 CDC）或 `/dev/ttyUSB*`（CH340/CP2102 桥）
- Windows: `COM*`（仅当 USB VID 不为空，过滤蓝牙 / 调制解调器虚拟口）

指定其他端口用环境变量：`CLAWD_MOOD_PORT=/dev/cu.usbmodemXXX`（mac/linux）或 `CLAWD_MOOD_PORT=COM7`（Windows）。

### 6. 插件级 daemon 自启 ≠ 系统级自启

`hook.py` 在 `SessionStart` 事件里会先 `probe_daemon()` 探活，没活就用 `subprocess.Popen` 拉脱离的后台进程：
- mac/linux: `start_new_session=True`
- Windows: `creationflags=DETACHED_PROCESS | CREATE_NO_WINDOW`

这是**插件级 / 会话触发**的自启：只在 CLI 第一次启动时拉起 daemon，不依赖系统服务。"明确不做：launchd/systemd 系统级自启"指的仍是写 LaunchAgents / `.service` 让 daemon 跟随开机/登录启动 —— 两者不冲突。

实现上的取舍：
- **Singleton 用 portfile + connect() 探活而非 PID 文件**：避免脏 PID、避免 `pgrep -f` 在 Windows 上不存在的问题。代价是 daemon 异常崩了 portfile 可能残留，但下次启动会自动覆盖（`check_singleton()` 探不到活就 take over）。
- **首条 SessionStart 状态消息会丢**：daemon 冷启动约 2–3 秒（uv 首次装 pyserial），此时 portfile 还没出现，`hook.py` 走 `port is None` 路径静默退出。下一条 `UserPromptSubmit` 会把状态补推上去，所以表情滞后但不会卡。
- **daemon 永驻**：不在 `Stop` 事件里关 daemon，避免多个 CLI 会话相互踩踏。需要重启 daemon 就 `pkill -f scripts/daemon.py`（mac/linux）；Windows 上更稳的做法是按 PID 杀（避免 `taskkill /F /IM python.exe` 误杀其他 python 进程）。
- **日志位置**：`<tempfile.gettempdir()>/clawd-mood-daemon.log` —— mac/linux 上 `${TMPDIR:-/tmp}/clawd-mood-daemon.log`（macOS 的 `$TMPDIR` 是 `/var/folders/...`，不是 `/tmp`），Windows 上 `%TEMP%\clawd-mood-daemon.log`。

### 7. portfile 服务发现 + 端口自适应

daemon 启动时尝试 bind 默认端口 `48756`，被占则回退到 `bind(0)` 让 OS 分配空闲端口。实际监听端口写入 portfile：`<tempfile.gettempdir()>/clawd-mood.port`（mac/linux: `${TMPDIR:-/tmp}/clawd-mood.port`，Windows: `%TEMP%\clawd-mood.port`）。

hook 端不直接读 `48756`，而是读 portfile 拿实际端口。这样：
- 默认端口冲突时自动避让，零配置
- hook 不需要管 daemon 是不是 singleton（read portfile → connect → 通了就发，连不上就静默退）
- 没有 pgrep / tasklist 依赖，三平台一致

显式覆盖：`CLAWD_MOOD_PORT_TCP=N` 强制 daemon 用 N。此时冲突直接报错退出（既然显式指定，应让用户知晓冲突）。

## 状态机设计原则

7 个状态由 hook → daemon → 固件单向推送。状态按会话独立维护，**以下转移在固件本地完成**，不靠上游：

- `Done` 进入后 3 秒（`DONE_REVERT_MS`）自动回 `Idle` —— done 是瞬态，否则会卡在笑脸
- 除 `Waiting` 外，会话 5 分钟（`SLEEP_IDLE_MS`）没有消息时进入 `Sleeping`；`Waiting` 不睡，避免漏掉待批准
- 该会话的任何消息立即唤醒 `Sleeping`
- 会话 30 分钟（`SESSION_EXPIRE_MS`）没有消息时从屏幕移除，兜底没发 `SessionEnd` 就退出的 CLI

## 多会话屏幕（桌面小队）

- 每页最多 4 个并撑满屏幕：1 个全屏，2 个左右拆分，3 个左大右二，4 个 2×2；固件最多记 12 个（`MAX_SESSIONS`），满了淘汰最久没动静的非紧急会话
- 超过 4 个时每 5 秒自动向左翻页（硬件没有按键），底部 4 像素页码条
- 别的页有会话**进入** `waiting` / `error` 时立即翻过去，停留到解除或最多 10 秒；该页页码块闪状态色
- 会话位置稳定，新会话追加到末尾，状态变化不换位；有会话退出时后面的依次前移
- 状态靠形状 + 颜色区分（地砖色 + 头顶图标）：idle 灰、thinking 蓝齿轮、working 绿键盘、waiting 黄问号、done 白对勾、error 红叉、sleeping 深灰 Z
- 标签优先显示会话标题，还没有标题时显示目录名；标题一旦拿到就保留，目录名随消息更新。按单元格宽度在 UTF-8 边界截断
- 标题来源是 CLI 内部数据，可能随版本失效，读不到时静默退回目录名：Claude Code 读 `transcript_path` 里的 `ai-title` 记录，Codex 读 `~/.codex/session_index.jsonl` 的 `thread_name`
- 中文用 `U8g2_for_Adafruit_GFX` 的 `u8g2_font_wqy12_t_gb2312`（GB2312 外的字符不显示，字库约占 208 KB 闪存）
- 用 `GFXcanvas16` 整屏帧缓冲（115 KB）一次刷新，不要回到 `fillScreen` 直接画屏，会闪

hook 端的事件→状态映射是**多对一收敛**：`PreToolUse` / `PostToolUse` / `SubagentStart` / `SubagentStop` 都映射到 `working`，避免表情在工具调用过程中乱跳。改 `hook.py` 的 `EVENT_TO_STATE` 表时保持这个收敛行为。

**两端共有的事件映射**：

- `PermissionRequest` → `waiting`（等用户批准）
- `PreCompact` → `thinking`（正在压缩上下文）
- `PostCompact` → `working`（压缩完成回到工作流）
- `SessionEnd` → `idle`（会话退出，避免停在 `working`）

`SessionStart` 的 `source` 为 `compact` 时只探活、不推状态：它在回合中途触发，推 `idle` 会打断工作表情。

**单端独有的事件映射**，另一端不会触发，互不干扰：

- Claude Code：`PostToolUseFailure` → `error`、`StopFailure` → `error`（API 报错结束回合）、`Notification` → `waiting`
- Codex：`Interrupt` → `idle`（用户中断回合）

state 枚举（小写、严格匹配）：`idle | thinking | working | waiting | done | error | sleeping`。固件里 `parseMood` 用 `strcmp`，**大小写敏感**。

## 双 CLI 并发

两端可同时挂载、同时运行，**无冲突**：

- TCP server 接受多客户端：两个 CLI 的 hook 进程各自短连接写 `127.0.0.1:<port>`，daemon `accept()` 循环串行消费
- daemon singleton 不变：`SessionStart` 拉 daemon 时 hook 走 `probe_daemon()`（portfile + 试连）防重复
- 串口由 daemon 独占：两个 CLI 共享同一个 daemon → 共享同一个串口
- 表情序列按事件到达顺序交错驱动，**不做合并/去重**——这意味着两端长任务同时跑时表情会快速切换，是预期行为

如果觉得"鬼畜"，停掉其中一端的会话即可（不需要卸载插件）。

## 串口协议

- 方向：**单向** Mac → ESP32，ESP32 不回包
- 格式：NDJSON（换行分隔），UTF-8，115200 baud
- 必需字段：`state`
- 会话字段：`session`（hook 对 session_id 取 8 位 hash，缺失时用 cwd）、`name`（cwd 目录名，最多 16 字符）、`title`（可选，会话标题，最多 16 字符）、`src`（`CC` / `CX`）、`end`（`SessionEnd` 时为 `true`，移除该会话）
- 不带 `session` 的旧格式消息归入 `default` 会话，手动测试仍可用
- 可选调试字段：`event`、`tool`（截断到 32 字符）
- 单行上限 320 字节（`LINE_MAX`）；daemon 只转发，不解析会话
- 未知 `state` 或坏 JSON：固件打 `[warn]` 到串口，不切换、不死

示例：`{"state":"working","event":"PreToolUse","tool":"Bash","session":"a00eada8","name":"clawd-mood","src":"CC"}`

## 常用命令

### 烧固件

```bash
# 编译 + 上传（端口按平台改）
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood

# macOS
arduino-cli upload -p /dev/cu.usbmodem101 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood

# Linux (可能需先 sudo usermod -aG dialout $USER 后重登)
arduino-cli upload -p /dev/ttyACM0 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood

# Windows (查设备管理器拿 COM 号)
arduino-cli upload -p COM3 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

板子检测：
- macOS: `ls /dev/cu.usbmodem*`
- Linux: `ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null`
- Windows PowerShell: `Get-CimInstance Win32_SerialPort | Select Name,DeviceID`

首次环境（README §1-3）：
- macOS: `brew install arduino-cli uv`（不再需要 jq）
- Linux: 系统包管理器装 `arduino-cli` + `uv`，加入 `dialout` 组
- Windows: `winget install ArduinoSA.CLI astral-sh.uv`

全平台都要装 ESP32 核心 + Adafruit GFX / ST7789 / ArduinoJson / U8g2_for_Adafruit_GFX 库。

### 跑 daemon

```bash
./plugin/scripts/daemon.py

# 指定串口（mac/linux 给路径，Windows 给 COM 名）
CLAWD_MOOD_PORT=/dev/cu.usbmodem101 ./plugin/scripts/daemon.py
CLAWD_MOOD_PORT=COM3 uv run plugin/scripts/daemon.py   # Windows PowerShell: $env:CLAWD_MOOD_PORT='COM3'

# 指定 TCP 端口（默认 48756，被占则自动回退；显式指定时冲突会直接报错退出）
CLAWD_MOOD_PORT_TCP=49000 ./plugin/scripts/daemon.py

# 日志不缓冲
PYTHONUNBUFFERED=1 ./plugin/scripts/daemon.py
```

`daemon.py` 头部是 PEP 723 内联依赖，`uv run` 自动建临时 venv 装 pyserial。不需要手动 `pip install`。

### 挂载插件

**Claude Code**：

```bash
claude --plugin-dir /absolute/path/to/clawd-mood/plugin
```

在 Claude Code 里 `/hooks` 确认 14 个事件都挂上（SessionStart / UserPromptSubmit / PreToolUse / PostToolUse / PostToolUseFailure / PermissionRequest / Notification / PreCompact / PostCompact / Stop / StopFailure / SubagentStart / SubagentStop / SessionEnd）。

**Codex CLI**（≥ 0.133.0）：

```bash
cd /absolute/path/to/clawd-mood
codex plugin marketplace add .          # 注册本地 marketplace（读 .agents/plugins/marketplace.json）
codex plugin add clawd-mood@clawd-mood  # 装插件（拷贝到 ~/.codex/plugins/cache/）
```

在 codex 里运行 `/hooks` 审阅并**信任** 12 个事件（比 Claude Code 少 `PostToolUseFailure` / `Notification` / `StopFailure`，多 `Interrupt`）：未信任的 hook 不会执行，hook 内容变更后要重新信任。之后任意目录 `codex` 就能用。卸载：`codex plugin remove clawd-mood@clawd-mood`。

**Codex 改 plugin 后必须重装**：codex 是把 plugin 内容**拷贝**到 `~/.codex/plugins/cache/clawd-mood/clawd-mood/<version>/`，不是 symlink。修改了源仓库的 `plugin/scripts/hook.py` / `hooks/hooks-codex.json` 后，codex 仍跑 cache 里的旧版本。开发时改完执行：

```bash
codex plugin remove clawd-mood@clawd-mood
codex plugin add clawd-mood@clawd-mood
```

Claude Code 走 `--plugin-dir` 是 live path，无此问题。

### 手动测试（不依赖 Claude Code）

```bash
# 灌 TCP 走完整管道（daemon → 串口 → 屏幕）
# mac/linux:
printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat ${TMPDIR:-/tmp}/clawd-mood.port)
# Windows PowerShell:
#   $p = Get-Content $env:TEMP\clawd-mood.port
#   $c = New-Object Net.Sockets.TcpClient('127.0.0.1', $p)
#   $s = $c.GetStream(); $b = [Text.Encoding]::UTF8.GetBytes('{"state":"working"}'+"`n")
#   $s.Write($b,0,$b.Length); $c.Close()

# 直接喂 hook.py（模拟 CLI 事件，三平台一致）
echo '{"hook_event_name":"PreToolUse","tool_name":"Bash"}' | ./plugin/scripts/hook.py

# Arduino 串口监视器手输（绕过 daemon）
# 在 IDE 监视器里输：{"state":"idle"} 等
```

JSON 校验：`python3 -m json.tool plugin/hooks/hooks.json`

## 测试策略

**没有自动化测试**——硬件依赖重、IO 逻辑短，靠手测 + 集成测覆盖：

- **固件单独**：Arduino 串口监视器手输 7 种 JSON，肉眼确认 7 种状态；带不同 `session` 字段可测 1–5+ 个会话的布局和翻页
- **daemon + 固件**：`printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat ${TMPDIR:-/tmp}/clawd-mood.port)`（mac/linux），Windows PowerShell 见上"手动测试"段
- **端到端**：跑真实任务（"列出当前目录"），观察 Idle → Thinking → Working → Done → Idle 序列
- **Done 3 秒回 Idle、Sleeping 5 分钟进入**：临时把 `SLEEP_IDLE_MS` 改成 `15000UL` 缩短验证
- **Linux / Windows**：未在本地端到端验证；代码层 review 确认无 macOS-isms。社区验证 welcome

完整测试清单见 `docs/superpowers/plans/2026-06-07-clawd-mood-implementation.md` Task 13 / 17。

## 仓库结构要点

- `firmware/clawd_mood/clawd_mood.ino` —— 单文件 Arduino sketch；会话表 + `tickSessions()` 计时器 + `tickPaging()` 翻页 + `drawCell()` / `drawCreature()` 渲染 + `pollSerial()` 拼行 + `handleLine()` JSON 解析
- `plugin/.claude-plugin/plugin.json` —— Claude Code 插件清单
- `plugin/.codex-plugin/plugin.json` —— Codex CLI 插件清单（指向 `hooks-codex.json`）
- `plugin/hooks/hooks.json` —— Claude Code 14 个事件全 `async: true`，命令统一指 `uv run "$CLAUDE_PLUGIN_ROOT"/scripts/hook.py`
- `plugin/hooks/hooks-codex.json` —— Codex 12 个事件（含 Interrupt），除 `SessionEnd`（Codex 强制同步）外全 `async: true`，同一个 hook.py，命令多传 `codex` 参数标记来源
- `plugin/scripts/hook.py` —— event → state 映射，TCP 短连接写 daemon（PEP 723，标准库 only，共享给两端）
- `plugin/scripts/daemon.py` —— PEP 723 内联依赖，TCP server ↔ 串口桥；portfile 服务发现
- `.agents/plugins/marketplace.json` —— Codex 本地 marketplace 入口
- `AGENTS.md` —— Codex 项目守则，指回本文件
- `docs/superpowers/specs/2026-06-07-clawd-mood-design.md` —— 原始设计说明书（**改架构前先读这个**）
- `docs/superpowers/specs/2026-06-07-codex-support-design.md` —— Codex 支持设计说明书
- `docs/superpowers/specs/2026-06-08-windows-linux-support-design.md` —— Windows + Linux 跨平台设计说明书
- `docs/superpowers/specs/2026-10-30-multi-session-squad-design.md` —— 多会话桌面小队设计说明书
- `docs/superpowers/plans/2026-06-07-clawd-mood-implementation.md` —— 原始 18 任务实施计划
- `docs/superpowers/plans/2026-06-07-codex-support-implementation.md` —— Codex 支持 8 任务实施计划
- `docs/superpowers/plans/2026-06-08-windows-linux-support-implementation.md` —— 跨平台实施计划

修改设计时同步更新 `docs/superpowers/specs/` 下的文档。
