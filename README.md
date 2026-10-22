# clawd-mood

ESP32-C3 + ST7789 1.54"（240×240）桌面摆件。电脑通过 USB 串口把 **Claude Code** 或 **OpenAI Codex CLI** 的运行状态推到板上，屏幕用 7 种像素眼睛实时换表情。

以 [MIT License](LICENSE) 开源。Copyright (c) 2026 liuyingwen。

<p align="center">
  <img src="images/hero.png" width="48%" alt="桌上的 clawd-mood，屏幕是工作中的像素眼，旁边是一台 MacBook" />
  <img src="images/with-claude-pandas.jpg" width="48%" alt="休眠表情的 clawd-mood，两侧各有一只 Claude 熊猫摆件" />
</p>

## 功能

数据只从电脑流向板子：CLI hook 把事件收成一行 JSON，本机 daemon 经 USB CDC 写到固件，固件画眼睛。

- **7 种表情**：`idle`、`thinking`、`working`、`waiting`、`done`、`error`、`sleeping`。大小写敏感。
- **Claude Code 插件**：挂 9 个 hook（`SessionStart`、`UserPromptSubmit`、`PreToolUse`、`PostToolUse`、`PostToolUseFailure`、`Notification`、`Stop`、`SubagentStart`、`SubagentStop`），命令为 `uv run hook.py`，`async: true`。
- **Codex CLI 插件**（≥ 0.133.0）：挂 10 个 hook，在上面的公共事件之外还有 `PermissionRequest`、`PreCompact`、`PostCompact`。Codex 这份清单不设 `async`。
- **两端可单独装，也可一起跑**。同一个 daemon 独占串口，按事件到达顺序换表情。
- **会话开始时拉起 daemon**。`SessionStart` 发现本机没有在听的 daemon，就在后台启动它。这是插件触发的进程，不是开机用的 launchd / systemd 服务。
- **本机 TCP 发现端口**。daemon 默认听 `127.0.0.1:48756`，被占用就改用系统分配的端口，并把端口写进临时目录的 `clawd-mood.port`。hook 读这个文件，不写死端口。
- **固件自己计时**：进入 `done` 约 3 秒后回到 `idle`；当前表情不是 `sleeping` 时，5 分钟没有新的合法串口消息就进入 `sleeping`。一条 `state` 与当前不同的合法消息会换表情，因此也可以把屏幕从 `sleeping` 唤醒。
- **跨平台 daemon**：macOS、Linux、Windows。Windows 的代码路径已写好，尚未在真实 Windows + ESP32 上端到端验证。Linux 与 macOS 共用 POSIX 路径，仓库里没有对应的 CI。

### 表情

| 状态 | 屏幕 | 什么时候出现 |
| --- | --- | --- |
| Idle | 黑色像素眼，慢慢左右晃，大约每 5–7.5 秒眨一次 | 上电默认；`SessionStart`；`done` 满 3 秒 |
| Thinking | 双眼在上、左、右、中之间移动 | `UserPromptSubmit`；Codex `PreCompact` |
| Working | 双眼轻微错位抖动，底部循环 `.` / `..` / `...` | `PreToolUse`、`PostToolUse`、`SubagentStart`、`SubagentStop`；Codex `PostCompact` |
| Waiting | 略高的眼睛上下跳，底部一个 `?` | Claude Code `Notification`；Codex `PermissionRequest` |
| Done | 先交替眯眼，再停在 `> <` 形笑容，约 3 秒后回 Idle | `Stop` |
| Error | 左右眼高低不对称、快速抖动 | Claude Code `PostToolUseFailure` |
| Sleeping | 闭成一条线，旁边有一个上浮的 `Z` | 5 分钟没有串口消息 |

`working` 把一轮工具调用收成同一种表情，避免每个 tool 事件都换脸。

### 当前范围

已经做了：USB 串口状态摆件、上述两种 CLI 插件、单 daemon、Wokwi 仿真目录（见下文）。

还没有做，也欢迎不要当成缺陷来报：Wi-Fi 控制、OTA、蜂鸣器、开机自启的 launchd / systemd、同时驱动多块屏幕、自动化单元测试。插着多块板时，daemon 使用排序后的第一个串口；要指定另一块，设置 `CLAWD_MOOD_PORT`。

## 硬件

固件按这块组合编写：

| 零件 | 说明 |
| --- | --- |
| ESP32-C3 Super Mini | 带 USB-C，走原生 USB CDC |
| ST7789 1.54" TFT | 240×240，SPI |
| 杜邦线 | 下表 8 根 |
| USB 数据线 | 需要能传数据，不能只充电 |

### 接线

VCC 接 **3.3V**。接到 5V 会烧屏。

| 屏幕 | ESP32-C3 |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO 10（MOSI） |
| SCL | GPIO 8（SCK） |
| RES | GPIO 2 |
| DC | GPIO 1 |
| CS | GPIO 4 |
| BL | GPIO 3 |

固件里背光脚上电拉高，SPI 用 `SPI.begin(8, -1, 10, TFT_CS)`，方向是 `tft.setRotation(1)`。

## 环境

- [uv](https://docs.astral.sh/uv/)。`hook.py` / `daemon.py` 声明 Python ≥ 3.12，用 `uv run` 即可，不必自己先建虚拟环境。
- [arduino-cli](https://arduino.github.io/arduino-cli/)，用于编译和上传。
- ESP32 Arduino 核心，以及库：`Adafruit GFX Library`、`Adafruit ST7735 and ST7789 Library`、`ArduinoJson`。
- Claude Code 和 Codex CLI 按你实际要挂的那一端安装。Codex 插件按 ≥ 0.133.0 编写。

### 安装 uv 和 arduino-cli

macOS：

```bash
brew install arduino-cli uv
```

Linux（发行版里没有 `arduino-cli` 时，用上游安装脚本）：

```bash
# Debian/Ubuntu
sudo apt install arduino-cli
curl -LsSf https://astral.sh/uv/install.sh | sh

# Arch
sudo pacman -S arduino-cli
curl -LsSf https://astral.sh/uv/install.sh | sh

# 串口权限：加入 dialout 后重新登录
sudo usermod -aG dialout "$USER"
```

Windows（代码已覆盖，真机未验证）：

```powershell
winget install ArduinoSA.CLI
winget install astral-sh.uv
```

## 烧录

### 1. ESP32 核心

还没有 arduino-cli 配置文件时先执行 `arduino-cli config init`。配置已经存在就跳过这一步：`config init --overwrite` 会盖掉你现有的板管理器地址。

```bash
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

FQBN 里的 `CDCOnBoot=cdc` 就是 **USB CDC On Boot = Enabled**。关掉它，daemon 看不到串口，屏幕也不会跟 CLI 走。

### 2. 库

```bash
arduino-cli lib install "Adafruit GFX Library" "Adafruit ST7735 and ST7789 Library" "ArduinoJson"
```

### 3. 找到端口

- macOS：`ls /dev/cu.usbmodem*`
- Linux：`ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null`
- Windows PowerShell：`Get-CimInstance Win32_SerialPort | Select Name,DeviceID`

### 4. 编译并上传

把 `<PORT>` 换成上面看到的端口（macOS 如 `/dev/cu.usbmodem101`，Linux 如 `/dev/ttyACM0`，Windows 如 `COM3`）。

```bash
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
arduino-cli upload -p <PORT> -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

也可以用 Arduino IDE：板子选 ESP32C3 Dev Module，打开 USB CDC On Boot，CPU 160 MHz，上传速率 921600，打开 `firmware/clawd_mood/clawd_mood.ino`。

## 使用

克隆：

```bash
git clone https://github.com/liuyingwen/clawd-desktop-buddy.git
cd clawd-desktop-buddy
```

### 1. 挂到 CLI

插件会在第一次 `SessionStart` 时自己拉起 daemon。你也可以先手动启动 daemon，确认串口通了再挂插件。

Claude Code，写入本机已启用插件：

```bash
claude plugin marketplace add .
claude plugin install clawd-mood@clawd-mood
```

之后任意目录的 `claude` 都会加载。在会话里执行 `/hooks`，应看到上面那 9 个事件。只想对当前进程生效、不写设置文件时：

```bash
claude --plugin-dir "$PWD/plugin"
```

Codex CLI：

```bash
codex plugin marketplace add .
codex plugin add clawd-mood@clawd-mood
```

`/hooks` 应看到 10 个事件。Codex 把插件**复制**到 `~/.codex/plugins/cache/`。改了 `plugin/scripts/hook.py` 或 hook 清单后要重新安装：

```bash
codex plugin remove clawd-mood@clawd-mood
codex plugin add clawd-mood@clawd-mood
```

卸载：

```bash
claude plugin uninstall clawd-mood@clawd-mood
claude plugin marketplace remove clawd-mood

codex plugin remove clawd-mood@clawd-mood
codex plugin marketplace remove clawd-mood
```

两端同时开着时，表情按各自事件的到达顺序切换。觉得跳得太勤，停掉其中一个会话即可。

### 2. 手动启动 daemon

macOS / Linux：

```bash
./plugin/scripts/daemon.py
```

Windows PowerShell：

```powershell
uv run plugin\scripts\daemon.py
```

第一次运行时 uv 会安装 `pyserial`。就绪时打印类似：

```
clawd-mood daemon started
  TCP:    127.0.0.1:48756
  Portfile: /tmp/clawd-mood.port
  Serial: /dev/cu.usbmodem101
  Ready!
```

Windows 的 portfile 在 `%TEMP%\clawd-mood.port`。再启动一次会因为已经能连上该端口而退出，并提示已有 daemon。

指定串口或 TCP 端口：

```bash
# macOS / Linux
CLAWD_MOOD_PORT=/dev/cu.usbmodem101 ./plugin/scripts/daemon.py
CLAWD_MOOD_PORT_TCP=49000 ./plugin/scripts/daemon.py
```

```powershell
$env:CLAWD_MOOD_PORT = 'COM3'
uv run plugin\scripts\daemon.py
```

`CLAWD_MOOD_PORT_TCP` 指定的端口如果被占用，daemon 直接退出。不设置这个变量时，48756 被占用则会自动换端口。

日志：macOS / Linux 为 `/tmp/clawd-mood-daemon.log`，Windows 为 `%TEMP%\clawd-mood-daemon.log`。需要边跑边看时用 `PYTHONUNBUFFERED=1`。

停掉 daemon（它不会在任务 `Stop` 时自己退出）：

```bash
# macOS / Linux
pkill -f scripts/daemon.py
rm -f /tmp/clawd-mood.port
```

Windows 上按 PID 结束对应的 Python 进程，避免 `taskkill /IM python.exe` 误伤别的程序，然后删除 `%TEMP%\clawd-mood.port`。

### 3. 看一次完整序列

挂好插件后跑一个短任务，例如列出当前目录。屏幕大致是：

```
Idle → Thinking → Working → Done → Idle
```

`Done` 大约 3 秒后自己回到 `Idle`。

不经过 CLI，直接喂 daemon（daemon 已在跑）：

```bash
# macOS / Linux
printf '{"state":"working"}\n' | nc 127.0.0.1 "$(cat /tmp/clawd-mood.port)"
```

```powershell
# Windows PowerShell（尚未在真机验证）
$p = [int](Get-Content $env:TEMP\clawd-mood.port)
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', $p)
$s = $c.GetStream()
$b = [Text.Encoding]::UTF8.GetBytes('{"state":"working"}' + "`n")
$s.Write($b, 0, $b.Length)
$c.Close()
```

只测事件映射、不要求屏幕变化时：

```bash
echo '{"hook_event_name":"PreToolUse","tool_name":"Bash"}' | ./plugin/scripts/hook.py
```

daemon 日志里应出现一行 `-> {"state":"working","event":"PreToolUse","tool":"Bash"}`。没有 daemon 时，hook 发现不了 portfile 会直接退出，不会卡住 CLI。

冷启动时，第一条 `SessionStart` 可能赶在 portfile 出现之前发出，这一条会被丢掉。下一条事件（通常是 `UserPromptSubmit`）会补上状态。

也可以在串口监视器里直接输入一行 JSON，波特率 115200，例如 `{"state":"idle"}`。

### 串口协议

单向，电脑到 ESP32，NDJSON，UTF-8，115200。必填字段是 `state`。`event` 和 `tool` 可有可无，固件只读 `state`。

```json
{"state":"working","event":"PreToolUse","tool":"Bash"}
```

未知 `state` 或坏 JSON：固件向串口打 `[warn]`，保持当前表情。

## 无硬件时看屏幕

`sim/` 用 Wokwi 跑同一份固件，**不**走上面的 TCP daemon。`sim/daemon-sim.py` 从 `/tmp/clawd-mood.fifo` 读 JSON，经 socat 的虚拟串口送给模拟器。真 USB CDC 上用来避免复位的 DTR/RTS 设置在 pty 上会失败，所以仿真用这份精简桥，不改 `plugin/scripts/daemon.py`。

准备（脚本和说明按 macOS 书写）：`arduino-cli`、`socat`、[wokwi-cli](https://wokwi.com/ci/install.sh)，以及环境变量 `WOKWI_CLI_TOKEN`（在 [Wokwi CI](https://wokwi.com/dashboard/ci) 申请）。

```bash
./sim/sim.sh
echo '{"state":"working"}' > /tmp/clawd-mood.fifo
```

`hook.py` 写的是 TCP，不会进入这条 FIFO。要用眼睛看 CLI 事件，先自己把事件对应的 `state` 写进 FIFO，或看 [sim/README.md](sim/README.md)。Wokwi 没有 ST7789 零件，仿真图用 ILI9341 顶替，颜色和原点可能偏，不能当像素级对照。

## 故障排除

- **屏幕不亮**：核对 VCC 是否在 3V3，BL 是否接到 GPIO 3。
- **方向反了**：只在安装方向不同时，把 `firmware/clawd_mood/clawd_mood.ino` 的 `tft.setRotation(1)` 改成 0、2 或 3，重新烧录。
- **`No ESP32-like USB CDC device found`**：板子没枚举成串口，或线只能充电。Linux 确认用户在 `dialout`。Windows 看设备管理器里有没有未知设备。烧录 FQBN 需要带 `CDCOnBoot=cdc`。
- **多块 ESP32**：`CLAWD_MOOD_PORT` 指向要驱动的那一个。macOS / Linux 给设备路径，Windows 给 `COM7` 这种名字。
- **日志是空的**：`PYTHONUNBUFFERED=1 ./plugin/scripts/daemon.py`。
- **Codex 改完没变化**：插件跑的是 cache 副本，按上面的 `remove` / `add` 重装。
- **默认端口 48756 被占用**：不设置 `CLAWD_MOOD_PORT_TCP` 时会自动换端口；hook 跟 portfile 走。

## 项目结构

```
firmware/clawd_mood/clawd_mood.ino   状态机、3 秒 done、5 分钟睡眠、7 个 draw 函数
plugin/scripts/hook.py               事件名 → state，经 TCP 发给 daemon
plugin/scripts/daemon.py             127.0.0.1 TCP → USB CDC 115200
plugin/hooks/hooks.json              Claude Code 的 9 个事件
plugin/hooks/hooks-codex.json        Codex 的 10 个事件
plugin/.claude-plugin/plugin.json    Claude Code 插件清单（0.1.0）
plugin/.codex-plugin/plugin.json     Codex 插件清单
.claude-plugin/marketplace.json      Claude Code 本地 marketplace
.agents/plugins/marketplace.json     Codex 本地 marketplace
sim/                                 Wokwi + FIFO 仿真，和真机 daemon 分开
docs/superpowers/                    设计说明和实现记录
```

## 参与贡献

看 [CONTRIBUTING.md](CONTRIBUTING.md)。Windows 和 Linux 真机结果尤其有用。问题请用 GitHub Issue 模板。

## 许可证

[MIT License](LICENSE)。Copyright (c) 2026 liuyingwen。见 [NOTICE](NOTICE)。

---

# English

clawd-mood is a desk display: an ESP32-C3 Super Mini and a 1.54" ST7789 240×240 TFT. A small local daemon reads Claude Code or OpenAI Codex CLI hook events and forwards one JSON line per event over USB serial. The firmware draws one of seven pixel-eye faces.

Licensed under the [MIT License](LICENSE). Copyright (c) 2026 liuyingwen.

## What it does

Flow is one way: CLI hook → TCP `127.0.0.1` → daemon → USB CDC 115200 → firmware.

- Seven states, lowercase and exact: `idle`, `thinking`, `working`, `waiting`, `done`, `error`, `sleeping`.
- Claude Code plugin: nine async hooks (`SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `PostToolUseFailure`, `Notification`, `Stop`, `SubagentStart`, `SubagentStop`).
- Codex CLI plugin (≥ 0.133.0): those shared events plus `PermissionRequest`, `PreCompact`, and `PostCompact`. The Codex hook file does not set `async`.
- Either CLI can be installed alone. Both can run at once and share one daemon and one serial port. Faces follow event arrival order.
- `SessionStart` starts the daemon in the background when nothing is listening. That is session startup, not a launchd or systemd boot service.
- The daemon binds `127.0.0.1:48756`, or an ephemeral port if that one is taken, and writes the port to `clawd-mood.port` in the system temp directory.
- On the board, `done` returns to `idle` after 3 seconds. If the face is not already `sleeping`, 5 minutes without a valid serial message enters `sleeping`. A valid `state` different from the current one changes the face, which is also how the display leaves `sleeping`.
- Host code targets macOS, Linux, and Windows. The Windows path is implemented and has not been verified on real Windows hardware. Linux shares the POSIX path with macOS; this repo does not CI either platform.

| State | On screen | When |
| --- | --- | --- |
| Idle | Wiggling eyes, blink about every 5–7.5 s | Boot, `SessionStart`, 3 s after `done` |
| Thinking | Eyes move up, left, right, center | `UserPromptSubmit`, Codex `PreCompact` |
| Working | Slight jitter and `.` / `..` / `...` | Tool and subagent events, Codex `PostCompact` |
| Waiting | Bouncing taller eyes and `?` | Claude Code `Notification`, Codex `PermissionRequest` |
| Done | Squish smile, then Idle | `Stop` |
| Error | Asymmetric jitter | Claude Code `PostToolUseFailure` |
| Sleeping | Closed eyes and a floating `Z` | 5 minutes with no serial message |

Tool and subagent events all map to `working`, so the face stays put during a tool burst.

In scope: the USB mood display, both CLI plugins, one daemon, and the Wokwi helper under `sim/`. Out of scope: Wi-Fi, OTA, a buzzer, boot-time launchd/systemd units, driving more than one display, and automated unit tests. With several boards plugged in, the daemon opens the first sorted port unless `CLAWD_MOOD_PORT` is set.

## Hardware

| Part | Notes |
| --- | --- |
| ESP32-C3 Super Mini | USB-C, native USB CDC |
| ST7789 1.54" TFT | 240×240 SPI |
| Jumper wires | Eight, per the table |
| USB cable | Data, not charge-only |

Connect display VCC to **3.3V**. 5V can destroy the panel.

| Display | ESP32-C3 |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO 10 (MOSI) |
| SCL | GPIO 8 (SCK) |
| RES | GPIO 2 |
| DC | GPIO 1 |
| CS | GPIO 4 |
| BL | GPIO 3 |

The sketch enables the backlight, calls `SPI.begin(8, -1, 10, TFT_CS)`, and uses `tft.setRotation(1)`.

## Requirements

- [uv](https://docs.astral.sh/uv/). The scripts request Python ≥ 3.12 and declare their dependencies inline.
- [arduino-cli](https://arduino.github.io/arduino-cli/).
- The ESP32 Arduino core, plus `Adafruit GFX Library`, `Adafruit ST7735 and ST7789 Library`, and `ArduinoJson`.
- Claude Code, Codex CLI ≥ 0.133.0, or both.

macOS: `brew install arduino-cli uv`.

Linux: install `arduino-cli` from the distro (or upstream), install uv with `curl -LsSf https://astral.sh/uv/install.sh | sh`, and add your user to `dialout`, then log in again.

Windows (unverified on hardware): `winget install ArduinoSA.CLI` and `winget install astral-sh.uv`.

## Flash

Run `arduino-cli config init` only when you do not already have a config. `--overwrite` replaces board-manager URLs you already added.

```bash
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit GFX Library" "Adafruit ST7735 and ST7789 Library" "ArduinoJson"
```

`CDCOnBoot=cdc` in the FQBN is **USB CDC On Boot: Enabled**.

Find the port with `ls /dev/cu.usbmodem*` (macOS), `ls /dev/ttyACM* /dev/ttyUSB*` (Linux), or `Get-CimInstance Win32_SerialPort` (Windows). Then:

```bash
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
arduino-cli upload -p <PORT> -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

Example ports: `/dev/cu.usbmodem101`, `/dev/ttyACM0`, `COM3`. Arduino IDE works too: ESP32C3 Dev Module, USB CDC On Boot enabled, 160 MHz, upload speed 921600, sketch `firmware/clawd_mood/clawd_mood.ino`.

## Run

```bash
git clone https://github.com/liuyingwen/clawd-desktop-buddy.git
cd clawd-desktop-buddy
```

Claude Code:

```bash
claude plugin marketplace add .
claude plugin install clawd-mood@clawd-mood
```

One-shot, without writing user settings:

```bash
claude --plugin-dir "$PWD/plugin"
```

Codex CLI:

```bash
codex plugin marketplace add .
codex plugin add clawd-mood@clawd-mood
```

Codex copies the plugin into `~/.codex/plugins/cache/`. After editing `plugin/`, remove and add the plugin again. Confirm hooks inside the CLI with `/hooks` (9 events for Claude Code, 10 for Codex).

Start the daemon yourself:

```bash
./plugin/scripts/daemon.py                 # macOS / Linux
uv run plugin\scripts\daemon.py            # Windows PowerShell
```

Override the serial device with `CLAWD_MOOD_PORT` (`/dev/cu...`, `/dev/ttyACM0`, or `COM3`). Override the TCP port with `CLAWD_MOOD_PORT_TCP`; an explicit port that is already taken is a hard error. The log file is `/tmp/clawd-mood-daemon.log` or `%TEMP%\clawd-mood-daemon.log`.

Push one state without a CLI:

```bash
printf '{"state":"working"}\n' | nc 127.0.0.1 "$(cat /tmp/clawd-mood.port)"
```

Or feed the hook directly:

```bash
echo '{"hook_event_name":"PreToolUse","tool_name":"Bash"}' | ./plugin/scripts/hook.py
```

A short CLI task should move the display through Idle → Thinking → Working → Done → Idle. The first `SessionStart` can be dropped while the daemon is still writing the port file; the next event fills in the face. The daemon stays up after `Stop`. On macOS and Linux, `pkill -f scripts/daemon.py` stops it. On Windows, stop that Python process by PID.

Serial messages are newline-delimited UTF-8 JSON at 115200 baud. `state` is required. `event` and `tool` are optional and ignored by the firmware. Bad JSON or an unknown state prints `[warn]` on serial and leaves the current face.

```json
{"state":"working","event":"PreToolUse","tool":"Bash"}
```

## Simulator

`./sim/sim.sh` compiles the sketch and opens it in Wokwi. `sim/daemon-sim.py` reads `/tmp/clawd-mood.fifo` and writes a virtual serial port. It does not replace the TCP daemon, and `hook.py` does not write that FIFO. Push `{"state":"working"}` into the FIFO to change the simulated face. You need `arduino-cli`, `socat`, `wokwi-cli`, and `WOKWI_CLI_TOKEN`. The Wokwi diagram uses an ILI9341 stand-in because Wokwi has no ST7789 part, so color and origin can be off. Details: [sim/README.md](sim/README.md).

## Layout

`firmware/clawd_mood/clawd_mood.ino` is the renderer and timers. `plugin/scripts/hook.py` maps events. `plugin/scripts/daemon.py` bridges TCP to serial. Hook manifests live in `plugin/hooks/`. Marketplace files are `.claude-plugin/marketplace.json` and `.agents/plugins/marketplace.json`. Design notes are under `docs/superpowers/`.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Reports from real Windows and Linux hardware are useful. Please use the issue templates.

## License

[MIT License](LICENSE). Copyright (c) 2026 liuyingwen. See [NOTICE](NOTICE).
