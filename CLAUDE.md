# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概要

`clawd-mood` 是一个 ESP32-C3 + ST7789 1.54" TFT 桌面摆件，通过 USB 串口接收 **Claude Code / OpenAI Codex CLI / OpenCode / pi / WorkBuddy / DeepSeek Harness** 的运行状态，以「桌面小队」形式实时显示多个会话：每个会话是一只像素小生物，同时显示会话标题或目录名和 7 种状态之一（idle / thinking / working / waiting / done / error / sleeping）。外观是编译进固件的主题（经典 / 午夜），方向和亮度也可调，都通过本地配置页 `page/serve.py` 设置，见 `docs/superpowers/specs/2026-11-02-config-page-themes-design.md`。

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
firmware/clawd_mood/*.ino     (Arduino 状态机 + 主题渲染)
```

配置页 `page/serve.py` 是 TCP 这一层的另一个客户端，发 `cfg` 行，不走 hook。

三个组件**互相独立**，可单独测试：
- 不启动 daemon 时，hook.py 读 portfile 拿不到端口（或连不上）直接静默退出（不阻塞 Claude Code / Codex）
- 不启动 CLI 时，可直接 `printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat ${TMPDIR:-/tmp}/clawd-mood.port)` 测 daemon + 固件（Windows 见下文"手动测试"）
- 不接 ESP32 时，daemon 启动时找不到 USB CDC 设备直接退出；运行中拔掉不退出（见 §6）
- 不挂 Claude Code 时，可只挂 Codex（`codex plugin add clawd-mood@clawd-mood`），或反之；两端可独立、可同装

## 关键约束（容易踩坑）

### 1. 显示器 VCC 接 3.3V，**不要接 5V**
ST7789 的 VCC 接到 ESP32-C3 的 3V3 引脚。接 5V 会烧屏。README 烧录段已写明，改文档时不要弄错。

### 2. `USB CDC On Boot` 必须 Enabled
板子在 Arduino IDE 配置里必须开 USB CDC On Boot。关掉了 daemon 看不到串口、屏幕也不会更新。`-b esp32:esp32:esp32c3:CDCOnBoot=cdc,...` 这串参数里的 `CDCOnBoot=cdc` 就是这意思。

### 3. SPI 引脚需要手动 remap
ESP32-C3 用 `SPI.begin(8, -1, 10, TFT_CS)` 显式指定 SCK=8 / MOSI=10，不要用默认值。

### 4. 屏幕方向
默认 `rotation` 1 是当前用户面板的安装方向（`DEFAULT_ROTATION`）。0/2/3 都是合法值，用户可以用 `cfg` 改；默认值**只在用户报告方向不对时**才改。参考 commit `e8f6e6e`（之前从 2 改到 1）。

### 5. 串口选择与多 USB CDC 设备
daemon 选串口的优先级：`CLAWD_MOOD_PORT` → 配置文件的 `serial_port`（已枚举到才用，否则警告并继续扫描）→ 扫描。扫描时优先 Espressif 原生 USB（VID `0x303A`），避免把 JSON 发给别的板子；剩下多个时警告并拿排序后的第一个。扫描规则跨平台：
- macOS: `/dev/cu.usbmodem*`
- Linux: `/dev/ttyACM*`（ESP32-C3 原生 CDC）或 `/dev/ttyUSB*`（CH340/CP2102 桥）
- Windows: `COM*`（仅当 USB VID 不为空，过滤蓝牙 / 调制解调器虚拟口）

指定其他端口用环境变量：`CLAWD_MOOD_PORT=/dev/cu.usbmodemXXX`（mac/linux）或 `CLAWD_MOOD_PORT=COM7`（Windows），或在配置页选（写入配置文件）。

配置文件是 `~/.clawd-mood/config.json`，来自真实用户目录（POSIX 上用 `pwd.getpwuid`，因为 DeepSeek Harness 会替换 `$HOME`），可用 `CLAWD_MOOD_CONFIG` 覆盖。形如 `{"serial_port":null,"last_sent":{...},"unsent":[]}`，由页面原子写入。只有 `daemon.py`（只读 `serial_port`）和页面读它，`hook.py` 和适配器**不读**。文件缺失时静默用默认值，损坏时记一次日志，都不会让 daemon 退出。

### 6. 插件级 daemon 自启 ≠ 系统级自启

`hook.py` 在 `SessionStart` 事件里会先 `probe_daemon()` 探活，没活就用 `subprocess.Popen` 拉脱离的后台进程：
- mac/linux: `start_new_session=True`
- Windows: `creationflags=DETACHED_PROCESS | CREATE_NO_WINDOW`

这是**插件级 / 会话触发**的自启：只在 `SessionStart` 拉起 daemon（配置页发设置时也会拉起），不依赖系统服务。启动时没设备的 daemon 会退出，要等下一个 `SessionStart` 或配置页操作才再启动。"明确不做：launchd/systemd 系统级自启"指的仍是写 LaunchAgents / `.service` 让 daemon 跟随开机/登录启动 —— 两者不冲突。

实现上的取舍：
- **Singleton 用 portfile + connect() 探活而非 PID 文件**：避免脏 PID、避免 `pgrep -f` 在 Windows 上不存在的问题。代价是 daemon 异常崩了 portfile 可能残留，但下次启动会自动覆盖（`check_singleton()` 探不到活就 take over）。
- **首条 SessionStart 状态消息会丢**：daemon 冷启动约 2–3 秒（uv 首次装 pyserial），此时 portfile 还没出现，`hook.py` 走 `port is None` 路径静默退出。下一条 `UserPromptSubmit` 会把状态补推上去，所以表情滞后但不会卡。
- **daemon 永驻**：不在 `Stop` 事件里关 daemon，避免多个 CLI 会话相互踩踏。需要重启 daemon 就 `pkill -f scripts/daemon.py`（mac/linux）；Windows 上更稳的做法是按 PID 杀（避免 `taskkill /F /IM python.exe` 误杀其他 python 进程）。
- **运行中拔插不退出**：写串口失败时关掉串口，TCP 继续收，消息丢弃。断开只记一次日志，恢复时记丢了多少条。之后的**消息**触发重新探测和打开，每 3 秒最多一次（`RECONNECT_INTERVAL_S`），没有消息就不会重连。连过 Espressif 板后，重连只等 Espressif 板。所以 TCP 发送成功不代表写进了串口，要看日志的 `-> line`
- **板子卡死但没断开时会阻塞**：`flush()`（tcdrain）没有写超时，串行循环会卡住。用 pty 测试时要持续读 master，否则同样会卡，连带 `nc` 也不退出
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
- 会话 5 分钟（`SLEEP_IDLE_MS`）没有消息时进入 `Sleeping`；`Working` 放宽到 15 分钟（`SLEEP_WORKING_MS`），长工具调用期间没有事件；`Waiting` 不睡，避免漏掉待批准
- 该会话的任何消息立即唤醒 `Sleeping`
- 会话 30 分钟（`SESSION_EXPIRE_MS`）没有消息时从屏幕移除，兜底没发 `SessionEnd` 就退出的 CLI

## 多会话屏幕（桌面小队）

- 每页最多 4 个并撑满屏幕：1 个全屏，2 个左右拆分，3 个左大右二，4 个 2×2；固件最多记 12 个（`MAX_SESSIONS`），满了淘汰最久没动静的非紧急会话
- 超过 4 个时每 5 秒自动向左翻页（硬件没有按键），底部 4 像素页码条
- 别的页有会话**进入** `waiting` / `error` 时立即翻过去，停留到解除或最多 10 秒；该页页码块闪状态色。带 `quiet` 的消息例外
- 会话位置稳定，新会话追加到末尾，状态变化不换位；有会话退出时后面的依次前移
- 不同工具是不同角色，按 `src` 查当前主题的 `SKINS` 表（每个主题各有一份，6 个 agent 顺序相同），造型来自各自 logo。主题 1 的值：CC 橙色 Clawd、CX 戴 OpenAI 结帽子的白色小人（现图标，不是旧的云朵）、OC 黑色空心框、PI 像素 P（独眼）、DS 蓝鲸、WB 青绿猫；主题 2 的 CX / OC / PI 改成亮色，否则融进深色背景。同一工具的多个会话长得一样，靠标签区分；未知 `src` 用 Clawd 造型 + 轮换颜色
- 状态靠形状 + 颜色区分（地砖色 + 右上角图标）。主题 1 的值：idle 灰、thinking 蓝齿轮、working 绿键盘、waiting 黄问号、done 白对勾、error 红叉、sleeping 深灰 Z；主题 2 见 `theme_noir.cpp`
- 改颜色要改具体主题，不要在 sketch 里写死：`theme.h` 的 `Palette` 按角色分字段，不借用状态色。主题 1 的画面被 `tools/preview/baseline.txt` 锁住
- 标签优先显示会话标题，还没有标题时显示目录名；标题一旦拿到就保留，目录名随消息更新。按单元格宽度在 UTF-8 边界截断
- 标题来源是 CLI 内部数据，可能随版本失效，读不到时静默退回目录名：Claude Code 读 `transcript_path` 里的 `ai-title` 记录，Codex 读 `~/.codex/session_index.jsonl` 的 `thread_name`
- 中文用 `U8g2_for_Adafruit_GFX` 的 `u8g2_font_wqy12_t_gb2312`（GB2312 外的字符不显示，字库约占 208 KB 闪存）
- 子代理是主小人头顶的一排小跟班（同款造型 12×9px，单会话全屏时翻倍），放不下显示 `+N`。动效：出现时落下、启动时看左右、工作时错拍跳动敲键盘、等待时变黄高跳（主小人同时显示 waiting 并翻页）、`SubagentStop` 后撒彩屑 1.5 秒再消失；收不到结束事件时，工具执行中 15 分钟（`FOLLOWER_EXPIRE_MS`）、工具之间（最后一条是 `PostToolUse` / `PostToolUseFailure`）2 分钟（`FOLLOWER_PAUSE_MS`）后移除，模型想得更久的会在下个工具事件时重新出现。所有 agent 都支持，同一只主小人跟自己工具的同款跟班。标签只由主代理更新
- 用 `GFXcanvas16` 整屏帧缓冲（115 KB）一次刷新，不要回到 `fillScreen` 直接画屏，会闪

hook 端的事件→状态映射是**多对一收敛**：`PreToolUse` / `PostToolUse` 都映射到 `working`，避免表情在工具调用过程中乱跳。**子代理不驱动主小人**：带 `agent_id` 的事件只更新头顶的小跟班；`SubagentStop` 常在主代理 `Stop` 之后才到（实测 13 次里 10 次），所以它只能移除跟班，不能改主状态，否则会盖掉 `done` 并卡到睡眠。改 `hook.py` 的 `EVENT_TO_STATE` 表时保持这个收敛行为。

**两端共有的事件映射**：

- `PermissionRequest` → `waiting`（等用户批准）
- `PreCompact` → `thinking`（正在压缩上下文）
- `PostCompact` → `working`（压缩完成回到工作流）。压缩也会在回合之外发生（实测回合结束约 54 分钟后触发 `trigger: auto`，之后没有 `Stop`），`trigger` 区分不了，所以固件记住 `PreCompact` 前的状态：原来是 working / thinking 才回 working，否则回 idle
- `SessionEnd` → `idle`（会话退出，避免停在 `working`）

`SessionStart` 的 `source` 为 `compact` 时只探活、不推状态：它在回合中途触发，推 `idle` 会打断工作表情。DeepSeek Harness 的 `SessionStart` 也一律不推：dsh 启动时会为每个恢复的历史会话各发一次，又没有 `SessionEnd`，屏幕上会多出一堆假会话，所以它的会话从第一条 `UserPromptSubmit` 才上屏。

**单端独有的事件映射**，另一端不会触发，互不干扰：

- Claude Code：`PostToolUseFailure` → `working`（`diff` 退出码 1 这类很常见，agent 还在继续，不算错误；其他 agent 的工具失败同样处理）、`StopFailure` → `error`（API 报错结束回合）、`Notification` → `waiting`（只认 `notification_type` 为 `permission_prompt` / `elicitation_dialog` 或缺失的情况，`idle_prompt` 这类不是待处理事项）
- Codex：`Interrupt` → `idle`（用户中断回合）

state 枚举（小写、严格匹配）：`idle | thinking | working | waiting | done | error | sleeping`。固件里 `parseMood` 用 `strcmp`，**大小写敏感**。

## 双 CLI 并发

两端可同时挂载、同时运行，**无冲突**：

- TCP server 接受多客户端：两个 CLI 的 hook 进程各自短连接写 `127.0.0.1:<port>`，daemon `accept()` 循环串行消费
- daemon singleton 不变：`SessionStart` 拉 daemon 时 hook 走 `probe_daemon()`（portfile + 试连）防重复
- 串口由 daemon 独占：两个 CLI 共享同一个 daemon → 共享同一个串口
- 表情序列按事件到达顺序交错驱动，**不做合并/去重**——这意味着两端长任务同时跑时表情会快速切换，是预期行为

如果觉得"鬼畜"，停掉其中一端的会话即可（不需要卸载插件）。

## 其他 agent 的接入方式

新 agent 优先复用 `hook.py`；只有进程内扩展才自己映射状态。设计见 `docs/superpowers/specs/2026-10-30-multi-agent-support-design.md`。

- **WorkBuddy / CodeBuddy Code**：复用 `hooks/hooks.json`（引擎同时注入 `CLAUDE_PLUGIN_ROOT` 和 `CODEBUDDY_PLUGIN_ROOT`），只多一份 `plugin/.codebuddy-plugin/plugin.json` 和根目录 `.codebuddy-plugin/marketplace.json`
- **DeepSeek Harness**：两条路线，二选一。原生插件 `plugin/adapters/dsh/`（推荐，桌面版唯一可用：`dsh` 命令行拒绝操作 `desktop` profile，只能在桌面版插件管理里装 `npm run pack:tarball` 打出的 tgz），订阅 Cordis 事件直接走 TCP，按 `session.header.id` 取 8 位 sha1 分会话，`origin: subagent` 显示为小跟班（嵌套的归到顶层会话；子代理审批策略固定为 never，不会 waiting），不监听 `agent/created`（它也为启动时恢复的历史会话触发）。备选是官方 `dsh-hooks-claude-code` 桥接读 `hooks/hooks-dsh.json`，仅 5 个事件，只用于命令行 / web 版。占位符只认 `${CLAUDE_PLUGIN_ROOT}`（带花括号），且它不在环境变量里，所以不能共用 `hooks.json`。沙箱内 `uv` 跑不了，这里用 `python3`（因此 `hook.py` 要兼容 macOS 自带的 3.9）
- **OpenCode ≥ 2.0 / pi**：`plugin/adapters/` 下的进程内适配器，事件直接映射成 state 走 TCP，`hook.py` 只用来拉起 daemon（`source: compact`，不推状态）。**不要改成每个事件起一个 uv**：`opencode run` 在回合结束约 8ms 后就退出，最后的 done 会丢

**容易踩坑**：
- `hooks.json` / `hooks-codex.json` / `hooks-dsh.json` 的命令结尾必须带 `|| true`。`uv` 自身出错退出码是 2，而同步 hook 的 2 表示拦截，会把用户的 prompt 全部挡掉
- OpenCode 1.x 的 `event` / `tool.execute.*` 接口在 2.0 不运行；2.0 的事件名是 `session.execution.*`、`session.tool.*`，且 payload 在 `data` 上
- pi 的 `agent_end` 不代表结束（可能重试、压缩），完成信号是 `agent_settled`；pi 的 subagent 是子进程，靠 `CLAWD_MOOD_PI_OWNER` 环境变量让顶层进程驱动主小人，子进程只发小跟班消息
- Claude Code 的 Workflow 子代理有工具事件，但不发 `SubagentStart` / `SubagentStop`（实测 61 个全无）。`hook.py` 每次运行时读 `<transcript 去后缀>/subagents/workflows/*/journal.jsonl` 的 `result` 记录，给新结束的 agent 补发 `gone`（读到的位置记在 `<tempdir>/clawd-mood-journal-<session>.json`，`SessionEnd` 时删除）。这是 CLI 内部文件，读不到时静默退回固件的超时
- 各家子代理的 `session_id`：Claude Code / WorkBuddy / Codex 都是父会话的（Codex 的 `agent_id` 是子线程 ID）。WorkBuddy 的 `agent_id` 来自 session 的 `storeId`，实测主会话带 `agent_type: cli` 但没有 `agent_id`，`SubagentStart` 有 `agent_id` 但没有 `agent_type`，所以 `hook.py` 对工具事件要求两者都有，对 `Subagent*` 只要 `agent_id`
- `codex exec` 回合结束后立刻退出，异步的 `Stop` / 紧贴结尾的 `SubagentStop` 会丢（实测），靠同步的 `SessionEnd` 兜底移除会话，所以验证 Codex 结束信号时要让主代理在子代理结束后再多跑几秒
- CodeBuddy 的 `--plugin-dir` 下事件会触发两次（marketplace 安装不会），状态相同，无害

## 串口协议

- 方向：**单向** Mac → ESP32，ESP32 不回包
- 格式：NDJSON（换行分隔），UTF-8，115200 baud
- 必需字段：`state`（设置行例外，见下）
- 会话字段：`session`（hook 对 session_id 取 8 位 hash，缺失时用 cwd）、`name`（cwd 目录名，最多 16 字符）、`title`（可选，会话标题，最多 16 字符）、`src`（`CC` / `CX` / `OC` / `PI` / `DS` / `WB`，WorkBuddy 与 Claude Code 共用 hooks.json，靠 `CODEBUDDY_PLUGIN_ROOT` 环境变量区分）、`end`（`SessionEnd` 时为 `true`，移除该会话）
- 不带 `session` 的旧格式消息归入 `default` 会话，手动测试仍可用
- 子代理字段：`agent`（`agent_id` 后 6 位，有它时只更新小跟班，不改主状态）、`gone`（`SubagentStop` 时为 `true`）
- 可选字段：`quiet`（`true` 时 waiting / error 只显示不翻页；固件保留支持，目前上游不发送）
- 可选字段：`event`（固件只用它判断小跟班是否在工具之间，缺失时按工具执行中算）、`tool`（调试用，截断到 32 字符）
- 单行上限 320 字节（`LINE_MAX`）；daemon 只转发合法 JSON，不解析会话，也不区分状态行和设置行
- daemon 的连接过滤：第一个非空白字节不是 `{` 的连接直接丢弃（网页也能连这个端口，HTTP 请求体里可能带合法行）；`recv` 超时 0.3 秒（`CLIENT_TIMEOUT_S`），所以客户端连上后要立刻发，等太久这条就丢了
- 固件只在两帧之间读串口，接收缓冲设为 4096 字节（`SERIAL_RX_BYTES`）。默认 256 字节时连发几条就丢行，已在真机复现
- 未知 `state` 或坏 JSON：固件打 `[warn]` 到串口，不切换、不死
- 设置行：`{"cfg":{"theme":N,"rotation":R,"brightness":B}}`，键都可选，没有 `state`，走 daemon 现有 TCP 端口。在任何会话逻辑之前处理并返回，不创建也不碰会话。未知主题回退主题 1；`rotation` 不在 0–3 忽略；`brightness` 限制到 10–100。默认 1 / 1 / 100，没收到过 `cfg` 的设备行为不变。Preferences 持久化（`setup` 里首帧前加载，防抖 1.5 秒，只写变化的键）。有变化时屏幕提示 3 秒，这是单向链路上唯一的确认；成功时**不打串口**：没人读，TX 缓冲满了会卡主循环

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

测帧时间：编译时加 `--build-property compiler.cpp.extra_flags=-DCLAWD_PROFILE`，固件每 5 秒向串口打印 render / push 微秒数和超时帧数（默认不编入）。

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

在 codex 里运行 `/hooks` 审阅并**信任** 12 个事件（比 Claude Code 少 `PostToolUseFailure` / `Notification` / `StopFailure`，多 `Interrupt`）：未信任的 hook 不会执行。**Codex 对整个插件目录算 hash，不只是 hook 命令**：`plugin/` 下任何文件变了（`hook.py`、`daemon.py`、清单都算），都要在 `/hooks` 重新批准。这就是 `page/`、`tools/` 放在 `plugin/` 外的原因，别把新工具放进去。之后任意目录 `codex` 就能用。卸载：`codex plugin remove clawd-mood@clawd-mood`。

**Codex 改 plugin 后必须重装**：codex 是把 plugin 内容**拷贝**到 `~/.codex/plugins/cache/clawd-mood/clawd-mood/<version>/`，不是 symlink。修改了源仓库 `plugin/` 下的文件后，codex 仍跑 cache 里的旧版本。开发时改完执行，然后在 `/hooks` 重新批准：

```bash
codex plugin remove clawd-mood@clawd-mood
codex plugin add clawd-mood@clawd-mood
```

Claude Code 走 `--plugin-dir` 是 live path，无此问题。

**WorkBuddy 改 plugin 后要清缓存再重装**：它同样拷贝到 `~/.workbuddy/plugins/cache/clawd-mood/clawd-mood/<version>/`，而且 `plugin uninstall` 不删这个目录，版本号没变时重装会继续用旧文件。改完先 uninstall，删掉该目录（或升版本号），再 install。

### 配置页

```bash
uv run page/serve.py            # 打开浏览器；--no-open 只打印地址，--port N 指定端口
```

只绑 `127.0.0.1`，地址里带一次性 token，10 分钟无操作退出，最长 1 小时。页面只能说「已发送」，不能说「已生效」：协议单向，设备不回话。测试时设 `TMPDIR` 和 `CLAWD_MOOD_CONFIG` 到临时目录，不要碰真实的 portfile 和配置文件。

### 预览工具（改固件绘制或主题时用）

```bash
uv run tools/preview/render.py --check                          # 主题 1 对得上 baseline、预览是最新的
uv run tools/preview/render.py                                  # 重新生成 page/previews/
uv run tools/preview/render.py --contact /tmp/contact.png       # 总览图 + 对比度报告
uv run tools/preview/render.py --compare tools/preview/baseline.txt   # 列出和 baseline 不同的帧
uv run tools/preview/render.py --baseline                       # 确认变化后重录
uv run tools/preview/headline.py                                # 改了页面标题文字时重新生成 assets/headline.png
```

要先按 README 装好 Arduino 库，它用的是同一份 GFX / U8g2 / ArduinoJson，再加一个主机 C++ 编译器。加主题的完整步骤见 `docs/superpowers/specs/2026-11-02-config-page-themes-design.md`。

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

# 发设置行（不开配置页）
printf '{"cfg":{"theme":2,"brightness":60}}\n' | nc 127.0.0.1 $(cat ${TMPDIR:-/tmp}/clawd-mood.port)
```

JSON 校验：`python3 -m json.tool plugin/hooks/hooks.json`

## 测试策略

**没有自动化测试**——硬件依赖重、IO 逻辑短，靠手测 + 集成测覆盖。这个约定不变，没有 CI，也不要加：

- **帧对比（主题作者手动跑的工具，不是 CI）**：`uv run tools/preview/render.py --check` 在主机上编译真实固件（真实 GFX / U8g2 / ArduinoJson，虚拟时钟），跑 `scenes/*.ndjson`，主题 1 的逐帧 hash 必须等于 `baseline.txt`，`page/previews/` 必须是最新的。改了绘制要先 `--compare tools/preview/baseline.txt` 看清哪些帧变了，确认后才 `--baseline` 重录。改了主题要跑 `render.py` 重新生成预览。它验证的是帧缓冲内容，不是屏幕、背光、NVS 或真机帧时间
- **固件单独**：Arduino 串口监视器手输 7 种 JSON，肉眼确认 7 种状态；带不同 `session` 字段可测 1–5+ 个会话的布局和翻页
- **daemon + 固件**：`printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat ${TMPDIR:-/tmp}/clawd-mood.port)`（mac/linux），Windows PowerShell 见上"手动测试"段
- **端到端**：跑真实任务（"列出当前目录"），观察 Idle → Thinking → Working → Done → Idle 序列
- **Done 3 秒回 Idle、Sleeping 5 分钟进入**：临时把 `SLEEP_IDLE_MS` 改成 `15000UL` 缩短验证
- **Linux / Windows**：未在本地端到端验证；代码层 review 确认无 macOS-isms。社区验证 welcome

完整测试清单见 `docs/superpowers/plans/2026-06-07-clawd-mood-implementation.md` Task 13 / 17。

## 仓库结构要点

- `firmware/clawd_mood/theme.h`、`theme_classic.cpp`、`theme_noir.cpp` —— 主题。加主题只要一个 `theme_*.cpp` 加 `THEME_TABLE` 一项；id 只增不复用，`THEMES[0]` 是主题 1
- `tools/preview/` —— 主机预览工具（`harness.cpp`、`shim/`、`scenes/`、`render.py`、`baseline.txt`；`headline.py` / `headline.cpp` 生成页面标题图）；`page/previews/` 是它的产物。`page/` 和 `tools/` 故意放在 `plugin/` 外：Codex 对整个插件目录算 hash，放进去用户要重新批准 12 个 hook
- `page/` —— 配置页：`serve.py`（本地服务，自带 CSP / token）、`index.html`、`page.css`、`page.js`、`assets/`（`state-colors.json` 是手抄的各主题状态色，改调色板要同步）、`previews/`（`render.py` 的产物，别手改）
- `firmware/clawd_mood/clawd_mood.ino` —— Arduino sketch；会话表 + `tickSessions()` 计时器 + `tickPaging()` 翻页 + `drawCell()` / `drawCreature()` 渲染 + `pollSerial()` 拼行 + `handleLine()` JSON 解析
- `plugin/.claude-plugin/plugin.json` —— Claude Code 插件清单
- `plugin/.codex-plugin/plugin.json` —— Codex CLI 插件清单（指向 `hooks-codex.json`）
- `plugin/hooks/hooks.json` —— Claude Code 14 个事件全 `async: true`，命令统一指 `uv run "$CLAUDE_PLUGIN_ROOT"/scripts/hook.py`
- `plugin/hooks/hooks-codex.json` —— Codex 12 个事件（含 Interrupt），除 `SessionEnd`（Codex 强制同步）外全 `async: true`，同一个 hook.py，命令多传 `codex` 参数标记来源
- `plugin/adapters/opencode/clawd-mood.js`、`plugin/adapters/pi/clawd-mood.ts` —— 进程内适配器，直接走 TCP，自带 `session`（按进程）/ `name` / `src`
- `plugin/adapters/dsh/` —— DeepSeek Harness 原生插件包（`index.js` + `cordis.patch.yml`），`prepack` 把 hook.py / daemon.py 拷进包里，tgz 脱离仓库也能拉起 daemon
- `plugin/hooks/hooks-dsh.json` —— DeepSeek Harness 桥接用，5 个事件，`python3` 启动，传 `dsh` 参数标记来源
- `plugin/.codebuddy-plugin/plugin.json`、`.codebuddy-plugin/marketplace.json` —— WorkBuddy / CodeBuddy Code 清单
- `plugin/scripts/hook.py` —— event → state 映射，TCP 短连接写 daemon（PEP 723，标准库 only，共享给两端）
- `plugin/scripts/daemon.py` —— PEP 723 内联依赖，TCP server ↔ 串口桥；portfile 服务发现；拔插恢复；读配置文件的 `serial_port`。要兼容 Python 3.9
- `.agents/plugins/marketplace.json` —— Codex 本地 marketplace 入口
- `AGENTS.md` —— Codex 项目守则，指回本文件
- `docs/superpowers/specs/2026-06-07-clawd-mood-design.md` —— 原始设计说明书（**改架构前先读这个**）
- `docs/superpowers/specs/2026-06-07-codex-support-design.md` —— Codex 支持设计说明书
- `docs/superpowers/specs/2026-06-08-windows-linux-support-design.md` —— Windows + Linux 跨平台设计说明书
- `docs/superpowers/specs/2026-10-30-multi-session-squad-design.md` —— 多会话桌面小队设计说明书
- `docs/superpowers/specs/2026-11-02-config-page-themes-design.md` —— 配置页与主题设计说明书（含加主题步骤、硬件未验证清单）
- `docs/superpowers/plans/2026-06-07-clawd-mood-implementation.md` —— 原始 18 任务实施计划
- `docs/superpowers/plans/2026-06-07-codex-support-implementation.md` —— Codex 支持 8 任务实施计划
- `docs/superpowers/plans/2026-06-08-windows-linux-support-implementation.md` —— 跨平台实施计划

修改设计时同步更新 `docs/superpowers/specs/` 下的文档。
