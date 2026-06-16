# clawd-mood

ESP32-C3 + ST7789 1.54" TFT 桌面摆件，USB 串口实时显示 **Claude Code** / **OpenAI Codex CLI** 的运行状态，7 种像素眼睛表情（idle / thinking / working / waiting / done / error / sleeping）。两端可独立挂、可同装、可同时跑。

> **0.2.0 BREAKING**：`/tmp/clawd-mood.fifo` 不再使用，改 TCP `127.0.0.1:<port>`（端口写在 `<tempdir>/clawd-mood.port`）。`plugin/scripts/hook.sh` 已删除，统一用 `plugin/scripts/hook.py`。升级方式：Claude Code 用户重新 `claude --plugin-dir`；Codex 用户 `codex plugin remove/add` 刷新缓存。
>
> 跨平台支持：macOS / Linux / Windows。Windows 端代码完成但 **untested on real Windows hardware**，欢迎社区验证。

## 准备

### macOS

```bash
brew install arduino-cli uv
```

### Linux

```bash
# Debian/Ubuntu
sudo apt install arduino-cli
curl -LsSf https://astral.sh/uv/install.sh | sh

# Arch
sudo pacman -S arduino-cli
curl -LsSf https://astral.sh/uv/install.sh | sh

# 串口权限：把用户加进 dialout 组后重新登录
sudo usermod -aG dialout $USER
```

### Windows

> ⚠️ **Untested on Windows.** 代码路径已 review 但未在真实 Windows + ESP32 硬件上端到端验证。试过的话请开 issue 反馈结果。

```powershell
winget install ArduinoSA.CLI
winget install astral-sh.uv
```

## 烧固件

### 1. 装 ESP32 板支持

```bash
arduino-cli config init --overwrite
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

### 2. 装 Arduino 库

```bash
arduino-cli lib install "Adafruit GFX Library" "Adafruit ST7735 and ST7789 Library" "ArduinoJson"
```

### 3. 接线（VCC 接 3.3V，**不要接 5V**）

| Display | ESP32-C3 |
| ------- | -------- |
| VCC     | 3V3      |
| GND     | GND      |
| SDA     | GPIO 10  |
| SCL     | GPIO 8   |
| RES     | GPIO 2   |
| DC      | GPIO 1   |
| CS      | GPIO 4   |
| BL      | GPIO 3   |

### 4. 查 ESP32 端口

- macOS: `ls /dev/cu.usbmodem*`
- Linux: `ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null`
- Windows PowerShell: `Get-CimInstance Win32_SerialPort | Select Name,DeviceID`

### 5. 编译 + 上传

编译（三平台相同）：

```bash
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

上传时把 `-p` 后面的端口名换成实际的：

| 平台 | 示例端口 |
|---|---|
| macOS | `/dev/cu.usbmodem101` |
| Linux | `/dev/ttyACM0` |
| Windows | `COM3` |

```bash
arduino-cli upload -p <PORT> -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

## 使用

### 1. 启动 daemon

```bash
# mac/linux
./plugin/scripts/daemon.py

# Windows PowerShell
uv run plugin\scripts\daemon.py
```

首次启动 uv 会自动装 pyserial。看到下面就绪：

```
clawd-mood daemon started
  TCP:    127.0.0.1:48756
  Portfile: /tmp/clawd-mood.port
  Serial: /dev/cu.usbmodem101
  Ready!
```

让它一直挂着。

### 2. 挂载到 CLI

两端可独立挂、可同装。

#### Claude Code

推荐：**marketplace install**（一次注册，全局生效，写入 `~/.claude/settings.json` 的 `enabledPlugins`）：

```bash
cd /path/to/clawd-mood
claude plugin marketplace add .
claude plugin install clawd-mood@clawd-mood
```

之后任意目录跑 `claude` 都自动挂上，无需 `--plugin-dir`。`/hooks` 确认 9 个事件全挂上。卸载：

```bash
claude plugin uninstall clawd-mood@clawd-mood
claude plugin marketplace remove clawd-mood
```

临时/开发用 `--plugin-dir`（不写 settings.json，只对当前会话生效）：

```bash
claude --plugin-dir /path/to/clawd-mood/plugin
```

#### Codex CLI（≥ 0.133.0）

首次：

```bash
cd /path/to/clawd-mood
codex plugin marketplace add .
codex plugin add clawd-mood@clawd-mood
```

之后任意目录 `codex` 即可。在 codex 里 `/hooks` 确认 10 个事件全挂上。

卸载：

```bash
codex plugin remove clawd-mood@clawd-mood
codex plugin marketplace remove clawd-mood
```

> Codex 是把 plugin **拷贝**到 `~/.codex/plugins/cache/`，改了 `plugin/scripts/hook.py` 等源文件后需要 `codex plugin remove/add` 才生效。Claude Code 的 `--plugin-dir` 是 live path，无此问题。

#### 同时挂

两端可同时跑，表情会按事件到达顺序交错切换。daemon 单实例、TCP server 多客户端，互不冲突。

### 3. 表情对照

| 状态     | 触发                                                                  |
| -------- | --------------------------------------------------------------------- |
| Idle     | 会话开始 / Stop 后 3 秒回归                                           |
| Thinking | 提交 prompt / Codex 压缩上下文中（PreCompact）                        |
| Working  | 调用工具 / Codex 压缩完恢复（PostCompact）                            |
| Waiting  | Claude 等你输入（Notification）/ Codex 等你批准（PermissionRequest）  |
| Done     | 回答完成（3 秒瞬态）                                                  |
| Error    | 工具失败（Claude Code 独有 PostToolUseFailure）                       |
| Sleeping | 5 分钟无事件                                                          |

### 4. 快速验证

挂载完，跑一个任意短任务（Claude Code 或 Codex 都行），观察屏幕表情序列：

```
Idle → Thinking → Working → Done → Idle（Done 3 秒后自动回 Idle）
```

Codex 特有事件验证：

| 触发动作                  | 期望表情     | 对应事件             |
| ------------------------- | ------------ | -------------------- |
| codex 弹出命令批准提示    | Waiting      | `PermissionRequest`  |
| codex `/compact` 压缩中   | Thinking     | `PreCompact`         |
| codex 压缩完成            | Working      | `PostCompact`        |

不依赖 CLI 直接灌一个 state 看屏幕：

```bash
# mac/linux
printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat /tmp/clawd-mood.port)
```

```powershell
# Windows PowerShell（untested）
$p = Get-Content $env:TEMP\clawd-mood.port
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', $p)
$s = $c.GetStream()
$b = [Text.Encoding]::UTF8.GetBytes('{"state":"working"}' + "`n")
$s.Write($b, 0, $b.Length); $c.Close()
```

屏幕没动？看 daemon log：

```bash
# mac/linux
tail -f /tmp/clawd-mood-daemon.log

# Windows PowerShell
Get-Content $env:TEMP\clawd-mood-daemon.log -Wait
```

每次事件应新增一行 `-> {"state":"...","event":"...","tool":"..."}`。

---

## 卸载

```bash
# Claude Code
claude plugin uninstall clawd-mood@clawd-mood
claude plugin marketplace remove clawd-mood

# Codex
codex plugin remove clawd-mood@clawd-mood
codex plugin marketplace remove clawd-mood
```

停 daemon + 清理：

```bash
# mac/linux
pkill -f scripts/daemon.py; rm -f /tmp/clawd-mood.port
```

```powershell
# Windows PowerShell
Get-Process python | Where-Object { $_.CommandLine -like '*daemon.py*' } | Stop-Process
Remove-Item $env:TEMP\clawd-mood.port -ErrorAction SilentlyContinue
```

---

## 故障排除

- **屏幕不亮**：检查接线，特别是 VCC 是不是接到 3V3 而不是 5V
- **屏幕方向不对**：改 `firmware/clawd_mood/clawd_mood.ino` 里 `tft.setRotation(1)` 换成 0/2/3 重烧
- **`No ESP32-like USB CDC device found`**：板子没插好，或者 USB 数据线只能充电不能传输；Linux 检查是否加入 `dialout` 组；Windows 检查设备管理器有无未识别设备
- **Daemon 日志不刷新**：用 `PYTHONUNBUFFERED=1 ./plugin/scripts/daemon.py` 启动
- **多个 ESP32 同时插**：用环境变量指定端口
  - mac/linux: `CLAWD_MOOD_PORT=/dev/cu.usbmodemXXX ./plugin/scripts/daemon.py`
  - Windows PowerShell: `$env:CLAWD_MOOD_PORT='COM7'; uv run plugin\scripts\daemon.py`
- **默认 TCP 端口 48756 被占**：daemon 会自动回退到 OS 分配的空闲端口（写进 portfile），hook 读 portfile 自动跟随。要强制指定：`CLAWD_MOOD_PORT_TCP=49000 ./plugin/scripts/daemon.py`（显式指定时冲突会直接报错退出）
- **Codex 不识别插件**：检查 `codex plugin marketplace list` 看本地 marketplace 是否注册；若 marketplace 路径变了，先 `codex plugin marketplace remove clawd-mood` 再 `add` 一次
- **Codex 改完 hook.py 没生效**：codex 是拷贝缓存，要 `codex plugin remove clawd-mood@clawd-mood && codex plugin add clawd-mood@clawd-mood` 刷新
