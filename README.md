# clawd-mood

## 烧录

### 1. 安装工具

```bash
brew install arduino-cli uv jq
```

### 2. 安装 ESP32 板支持

```bash
arduino-cli config init --overwrite
arduino-cli config add board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

### 3. 安装 Arduino 库

```bash
arduino-cli lib install "Adafruit GFX Library" "Adafruit ST7735 and ST7789 Library" "ArduinoJson"
```

### 4. 接线（VCC 接 3.3V，**不要接 5V**）

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

### 5. 编译 + 烧录

把板子用 USB-C 插到 Mac 上，确认串口存在：

```bash
ls /dev/cu.usbmodem*
```

进入项目目录，编译 + 烧录：

```bash
cd /path/to/clawd-mood
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
arduino-cli upload -p /dev/cu.usbmodem101 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

如果串口名不是 `usbmodem101`，把 `-p` 后面换成实际的。

---

## 使用

### 1. 启动 daemon

```bash
cd /path/to/clawd-mood
chmod +x plugin/scripts/daemon.py
./plugin/scripts/daemon.py
```

首次启动 uv 会自动装 pyserial。看到下面就绪：

```
clawd-mood daemon started
  FIFO:   /tmp/clawd-mood.fifo
  Serial: /dev/cu.usbmodem101
  Ready!
```

让它一直挂着。

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

### 3. 表情对照

| 状态     | 触发                        |
| -------- | --------------------------- |
| Idle     | 会话开始 / Stop 后 3 秒回归 |
| Thinking | 提交 prompt                 |
| Working  | 调用工具                    |
| Waiting  | Claude 等你输入             |
| Done     | 回答完成（3 秒瞬态）        |
| Error    | 工具失败                    |
| Sleeping | 5 分钟无事件                |

### 4. 手动测试（不依赖 Claude Code）

Daemon 跑着，往 FIFO 灌 JSON：

```bash
echo '{"state":"working"}' > /tmp/clawd-mood.fifo
echo '{"state":"done"}'    > /tmp/clawd-mood.fifo
```

---

## 故障排除

- **屏幕不亮**：检查接线，特别是 VCC 是不是接到 3V3 而不是 5V
- **屏幕方向不对**：改 `firmware/clawd_mood/clawd_mood.ino` 里 `tft.setRotation(1)` 换成 0/2/3 重烧
- **`No /dev/cu.usbmodem*`**：板子没插好，或者 USB 数据线只能充电不能传输
- **Daemon 日志不刷新**：用 `PYTHONUNBUFFERED=1 ./plugin/scripts/daemon.py` 启动
- **多个 ESP32 同时插**：`CLAWD_MOOD_PORT=/dev/cu.usbmodemXXX ./plugin/scripts/daemon.py` 指定端口
