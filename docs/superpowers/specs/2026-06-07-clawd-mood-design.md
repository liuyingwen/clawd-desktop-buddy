# clawd-mood — Design Spec

**Date**: 2026-06-07
**Status**: Approved (pending user review)
**Authors**: liuyingwen + Claude

## 1. 目标

让 ESP32-C3 + ST7789 1.54" TFT 桌面摆件，通过 USB 串口接收 Claude Code 的运行状态，实时切换 7 种像素眼睛表情。

## 2. 范围

**做什么**：
- Claude Code 插件（hooks.json + hook.sh）把事件转成 state JSON
- macOS 守护进程（daemon.py + uv）保持串口长连，桥接 FIFO ↔ USB
- ESP32-C3 固件（Arduino IDE 单文件 .ino）渲染 7 表情 + 自动状态转移

**不做什么**：
- 不做 WiFi 网页控制（去掉 clawd-mochi 的 HTTP/Canvas 功能）
- 不做饥饿值连续变量（去掉 claudigotchi 的累积积分系统）
- 不做蜂鸣器声音
- 不做 OTA 升级
- 不做 launchd 自动启动 daemon
- 不做 Windows / Linux 兼容（macOS only）
- 不写自动化单元测试

## 3. 整体架构

```
Claude Code event
       │
       ▼
hook.sh  (Bash + jq；事件 → state)
       │
       ▼
/tmp/clawd-mood.fifo  (命名管道)
       │
       ▼
daemon.py  (uv 启动；常驻；写串口)
       │  USB CDC 115200 baud
       ▼
ESP32-C3 + ST7789  (Arduino sketch；状态机 + 像素眼)
```

## 4. 目录结构

```
clawd-mood/
├── README.md
├── .gitignore
├── firmware/
│   └── clawd_mood/
│       └── clawd_mood.ino           # Arduino 单文件
├── plugin/
│   ├── .claude-plugin/
│   │   └── plugin.json
│   ├── hooks/
│   │   └── hooks.json
│   └── scripts/
│       ├── daemon.py                # 顶部 PEP 723 内联依赖
│       └── hook.sh
└── docs/
    └── superpowers/specs/
        └── 2026-06-07-clawd-mood-design.md
```

## 5. 状态机

### 5.1 状态集

| 状态 | 触发事件 | 表情视觉 |
|---|---|---|
| `idle` | `SessionStart` / `Stop` 后 3 秒 / 上电 | 普通方块眼，慢眨眼，微微 wiggle |
| `thinking` | `UserPromptSubmit` | 眼球缓慢"上 → 左 → 右"循环 |
| `working` | `PreToolUse` / `PostToolUse` / `SubagentStart` / `SubagentStop` | 眼神聚焦+轻抖动，底部 `. .. ...` 循环 |
| `waiting` | `Notification` | 眼睛瞪大上下弹跳，底部 `?` |
| `done` | `Stop`（瞬态，3 秒后转 idle） | squish 笑眼 `> <` + 1 秒动画 |
| `error` | `PostToolUseFailure` | 眼睛斜歪、不对称、抖动 |
| `sleeping` | 固件本地计时：5 分钟无事件自动进入 | 横线闭眼 + `Z` 漂浮 |

### 5.2 自动转移

- **Done → Idle**：进入 `done` 后 3 秒倒计时，自动转 `idle`
- **任意状态 → Sleeping**：固件用 `millis()` 计 `lastEventMs`，超过 5 分钟无串口消息自动进 `sleeping`
- **Sleeping → 任意状态**：任何串口消息立即唤醒

### 5.3 关键设计原则

- **事件驱动直接映射**：不引入饥饿值积分；hook 来什么状态，固件立即切什么状态
- **多事件收敛**：`PreToolUse` / `PostToolUse` / `SubagentStart` / `SubagentStop` 全部映射到 `working`，避免表情乱跳
- **Done 必须是瞬态**：否则会卡在笑脸不真实

## 6. 串口协议

- **方向**：单向 Mac → ESP32，ESP32 不回包
- **格式**：NDJSON（换行分隔），UTF-8，115200 baud
- **最小消息**：`{"state":"working"}`
- **可选调试字段**：`{"state":"working","event":"PreToolUse","tool":"Bash"}`
- **state 枚举**（小写）：`idle | thinking | working | waiting | done | error | sleeping`

## 7. 组件设计

### 7.1 plugin/.claude-plugin/plugin.json

```json
{
  "name": "clawd-mood",
  "description": "Drives a clawd-mood ESP32-C3 desk mascot via USB serial, reflecting Claude Code state on a TFT display.",
  "version": "0.1.0",
  "author": { "name": "liuyingwen" }
}
```

### 7.2 plugin/hooks/hooks.json

注册 9 个事件，全部 `async: true`，命令统一指向 `"$CLAUDE_PLUGIN_ROOT"/scripts/hook.sh`：

```
SessionStart, UserPromptSubmit, PreToolUse, PostToolUse,
PostToolUseFailure, Notification, Stop, SubagentStart, SubagentStop
```

### 7.3 plugin/scripts/hook.sh

```bash
#!/bin/bash
# clawd-mood hook — maps Claude Code events to ESP32 states via FIFO.

FIFO="/tmp/clawd-mood.fifo"

# 守护进程没启动则静默退出，不阻塞 Claude Code
[ -p "$FIFO" ] || exit 0
# jq 缺失也兜底
command -v jq >/dev/null || exit 0

INPUT=$(cat)
EVENT=$(echo "$INPUT" | jq -r '.hook_event_name // empty')
TOOL=$(echo "$INPUT" | jq -r '.tool_name // empty')

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
  *)                  exit 0 ;;
esac

# 1 秒超时兜底，防止 FIFO 写卡死
timeout 1 bash -c "echo '{\"state\":\"$STATE\",\"event\":\"$EVENT\",\"tool\":\"$TOOL\"}' > '$FIFO'" || true
exit 0
```

### 7.4 plugin/scripts/daemon.py

**关键特性**：

- PEP 723 内联依赖头，`uv run` 自动建临时 venv
- 启动时 glob 自动探测 `/dev/cu.usbmodem*`，`CLAWD_MOOD_PORT` 环境变量可覆盖
- 创建 FIFO `/tmp/clawd-mood.fifo`
- DTR/RTS 禁用 + 启动排空 1000 字节 ×2，避免 ESP32 复位
- 循环：读 FIFO → JSON 校验 → 写串口
- `SIGINT/SIGTERM` 清理 FIFO 退出

**脚本头**：

```python
#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["pyserial>=3.5"]
# ///
```

**端口探测**：

```python
ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("No /dev/cu.usbmodem* device found. Is the ESP32 plugged in?")
port = os.environ.get("CLAWD_MOOD_PORT") or ports[0]
if len(ports) > 1 and not os.environ.get("CLAWD_MOOD_PORT"):
    print(f"  warning: multiple devices found {ports}, using {port}")
```

### 7.5 firmware/clawd_mood/clawd_mood.ino

**砍掉 clawd-mochi 的**：WiFi、WebServer、DNSServer、所有 HTTP 路由 handler、网页 HTML 字符串、Canvas WebSocket 逻辑

**保留 clawd-mochi 的**：ST7789 引脚配置、Adafruit_GFX/ST7789 初始化、`drawEye()` / `drawSquishEye()` 等绘制函数、`EYE_W/EYE_H/EYE_GAP/EYE_OX/EYE_OY` 几何参数、启动 logo（可选）

**新增**：

```cpp
#include <ArduinoJson.h>

enum Mood { MOOD_IDLE, MOOD_THINKING, MOOD_WORKING, MOOD_WAITING,
            MOOD_DONE, MOOD_ERROR, MOOD_SLEEPING, MOOD_UNKNOWN };

Mood currentMood = MOOD_IDLE;
unsigned long lastEventMs = 0;
unsigned long doneEnteredMs = 0;
String serialBuf;

void pollSerial();          // 非阻塞读串口拼行
void handleLine(const String& line);   // JSON 解析 → setMood
Mood parseMood(const char* s);
void setMood(Mood m);       // 切换+重置 doneEnteredMs
void tickMoodMachine();     // Done→Idle (3s) / *→Sleeping (5min)
void renderMood();          // 调度 7 个 draw 函数

void drawIdle(); void drawThinking(); void drawWorking();
void drawWaiting(); void drawDone(); void drawError(); void drawSleeping();
```

**主循环**：

```cpp
void loop() {
  pollSerial();
  tickMoodMachine();
  renderMood();
  delay(30);   // ~33 fps
}
```

**新增库依赖**：ArduinoJson（Library Manager 安装）

**板配置（保持 clawd-mochi 一致）**：

- Board: ESP32C3 Dev Module
- **USB CDC On Boot: Enabled**（关键）
- CPU Frequency: 160 MHz
- Upload Speed: 921600

**引脚（保持 clawd-mochi 一致）**：

| Display | ESP32-C3 GPIO |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO10 |
| SCL | GPIO8 |
| RES | GPIO2 |
| DC | GPIO1 |
| CS | GPIO4 |
| BL | GPIO3 |

## 8. 错误处理

| 故障 | 处理 |
|---|---|
| 用户没装 uv / jq | hook.sh 兜底 `command -v` 检测，daemon README 写明 |
| daemon 没启动 | hook.sh 检测 FIFO 不存在 → 静默 exit 0 |
| ESP32 没插 | daemon glob 找不到 → 打印提示后退出 |
| 多个 USB CDC 设备 | daemon 打印列表，提示 `CLAWD_MOOD_PORT=...` 指定 |
| 运行中拔 USB | `SerialException` → 退出，用户手动重启 |
| FIFO 写卡死 | hook.sh 用 `timeout 1` 兜底 |
| 未知事件 / 未知 state | 静默忽略，串口 `[warn]` 提示 |
| 坏 JSON | 跳过，串口 `[warn]` 提示 |

## 9. 测试策略

### 9.1 固件本地（无 Mac 端依赖）

Arduino 串口监视器手输 7 种 JSON 验证 7 表情：

```
{"state":"idle"} / thinking / working / waiting / done / error / sleeping
```

时间规则验证：

- 发 `done` 后数 3 秒，应自动回 idle
- 发任意状态后停 5 分钟，应自动进 sleeping

### 9.2 守护进程联调（不依赖 Claude Code）

```bash
echo '{"state":"working"}' > /tmp/clawd-mood.fifo
```

预期 daemon 日志 + ESP32 屏幕切换。

### 9.3 端到端（连 Claude Code）

1. 启 daemon
2. `claude --plugin-dir ./plugin`
3. `/hooks` 命令确认 9 个 hook 注册
4. 跑真实任务："列出当前目录"
5. 预期：Idle → Thinking → Working → Done → (3s) Idle
6. 触发 Notification 场景 → Waiting
7. 故意触发 Bash 失败 → Error

### 9.4 不写自动化单元测试

理由：IO 重、硬件相关、逻辑链短，手测 + 集成测足够。

## 10. README 内容清单

1. 项目简介 + 截图位（先留 TODO）
2. Parts list（抄 clawd-mochi）
3. Wiring 表（抄 clawd-mochi）
4. Software setup（Arduino IDE + uv + jq）
5. 烧固件步骤
6. 启动 daemon 步骤
7. Claude Code 插件挂载（`--plugin-dir`）
8. 7 状态对照表
9. 串口协议
10. 调试技巧（手灌 FIFO + 串口监视器）
11. License

## 11. 后续可扩展点（明确 out-of-scope）

- 蜂鸣器音效
- 多设备支持
- WiFi 网页备份控制
- launchd 自启
- 自定义表情主题
