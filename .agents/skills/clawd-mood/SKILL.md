---
name: clawd-mood
description: 安装、配置、排查和卸载 MiqiPet（clawd-mood，ESP32-C3 + ST7789 桌面小屏，显示 AI 编程 agent 的会话状态）。用户要安装或烧固件、把 Claude Code / Codex / OpenCode / pi / WorkBuddy / DeepSeek Harness / Cursor 接上屏幕、换主题样式、调屏幕方向或亮度、选串口、更新版本、屏幕没反应、或卸载时使用。Use to install, flash, hook up, restyle (theme, rotation, brightness), update, troubleshoot or uninstall the MiqiPet desk screen.
---

# MiqiPet

下面的命令都在 clawd-mood 仓库的根目录执行。按顺序找仓库：

1. 本文件往上三级（`.agents/skills/clawd-mood/SKILL.md`）有 `plugin/scripts/daemon.py`，那里就是
2. `~/clawd-mood`
3. 问用户是否克隆在别处；没有就 `git clone https://github.com/liuyingwen/clawd-desktop-buddy ~/clawd-mood`

插件和 hook 都记着仓库的绝对路径，装好后不要挪动仓库。

需要 sudo 或修改系统配置（比如 Linux 加 dialout 组）时，先征求用户同意。你在沙箱里运行（比如 Codex）时，安装、烧录和改设置要用到网络、写用户目录、访问串口设备，按你的方式申请权限。

| 用户想要 | 去哪 |
| --- | --- |
| 安装 | [安装](#安装) |
| 换样式、调方向或亮度 | [改设置](#改设置) |
| 屏幕没反应、报错 | [references/troubleshooting.md](references/troubleshooting.md) |
| 更新到新版本 | [更新](#更新) |
| 卸载 | [references/agents.md](references/agents.md) 里对应 agent 的「卸载」 |

## 安装

### 1. 装 uv

先检查 `uv` 是否已在 PATH 里，没有就装：

- macOS：`brew install uv`（没有 Homebrew 就请用户先按 https://brew.sh 装好）
- Linux：`curl -LsSf https://astral.sh/uv/install.sh | sh`；串口权限 `sudo usermod -aG dialout $USER`，重新登录后生效
- Windows：`winget install astral-sh.uv`。告诉用户 Windows 还没在真机上验证过

只有[从源码编译](#从源码编译)才需要 arduino-cli。

### 2. 确认接线

你看不到硬件，问用户屏幕是否已按下表接好并插上 USB。没接就把表给用户，并强调 **VCC 接 3V3，接 5V 会烧屏**：

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

### 3. 烧固件

找串口：macOS `ls /dev/cu.usbmodem*`，Linux `ls /dev/ttyACM* /dev/ttyUSB*`，Windows `Get-CimInstance Win32_SerialPort | Select Name,DeviceID`。有多个时 `uv run page/serve.py --status` 里 `serial.candidates` 标了 `likely` 的是 ESP32（VID `0x303A`），还分不清就问用户。

默认烧 GitHub Releases 里预编译好的固件，不用装编译环境。先停掉占着串口的 daemon（`pkill -f scripts/daemon.py`，Windows 见[停掉 daemon](references/troubleshooting.md#停掉-daemon)），然后下载和当前代码对应的那一版，紧接着烧录：

```bash
FW=$(git log -1 --format=%H -- firmware/ | cut -c1-7)
curl -fLO https://github.com/liuyingwen/clawd-desktop-buddy/releases/download/fw-$FW/clawd-mood-esp32c3.bin
uvx --no-build-package cryptography esptool --chip esp32c3 --port <串口> write-flash 0x0 clawd-mood-esp32c3.bin
```

- Windows PowerShell：`$FW = (git log -1 --format=%H -- firmware/).Substring(0,7)`，下载用 `curl.exe`
- `--no-build-package cryptography` 不能省：cryptography 新版没有 Intel Mac 的预编译包，省掉会去从源码编译，然后失败
- 下载 404：固件改动推送到 main 后，CI 要几分钟才发布，过几分钟再试。仍然 404，或者用户改过固件（`git status --porcelain firmware/` 有输出）、接线和上表不同，就[从源码编译](#从源码编译)
- 烧录失败：先关掉串口监视器，再停一次 daemon 后马上重烧（别的 agent 会话一开始又会把它拉起来）；还不行就请用户按住板子上的 BOOT 键再插 USB，进入下载模式后重试
- 这个镜像会清掉板子上保存的设置。重烧或更新时，烧完运行 `uv run page/serve.py --resend` 恢复

#### 从源码编译

先装 arduino-cli（macOS `brew install arduino-cli`，Linux `sudo apt install arduino-cli` 或 `sudo pacman -S arduino-cli`，Windows `winget install ArduinoSA.CLI`）。ESP32 板支持包要下载几个 GB，先告诉用户。接线不同的话，先改 `firmware/clawd_mood/clawd_mood.ino` 开头的 `TFT_*` 引脚和 `SPI.begin(8, -1, 10, TFT_CS)`（SCK、MOSI）。

```bash
arduino-cli core update-index --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core install esp32:esp32 --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli lib install "Adafruit GFX Library" "Adafruit ST7735 and ST7789 Library" "ArduinoJson" "U8g2_for_Adafruit_GFX"
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
arduino-cli upload -p <串口> -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

上传前同样要停掉 daemon，失败时的处理同上。编译出了问题，可以对照 `.github/workflows/firmware.yml` 里 CI 用的板支持包和库版本。这种方式不清设置，不用 `--resend`。

### 4. 接上 agent

判断你自己是哪个 CLI，只装这一个，命令见 [references/agents.md](references/agents.md)。用户要求时才装别的。需要用户亲手做的步骤（比如 Codex 要在 `/hooks` 里信任、桌面应用要重启）要明确告诉用户。

### 5. 验证

先请用户盯着屏幕，再运行：

```bash
uv run page/serve.py --test
```

屏幕上会出现「测试小人」：先工作、再完成、几秒后离开，命令要等它离开才输出结果。daemon 没在跑时会自动拉起（第一次要装 pyserial，多等几秒）。输出 `"result": "sent"` 后问用户看到没有；不是 `sent` 就看 `explain`，再查 [troubleshooting](references/troubleshooting.md)。

正在运行的 agent 会话不会加载刚装的 hook。请用户新开一个会话发一句话，屏幕上这个会话应依次显示 Thinking → Working → Done，3 秒后回到 Idle。

### 6. 收尾

- 问用户要不要换样式，可选的见[改设置](#改设置)，样子见 README 的「屏幕样式」。
- 本技能是你从仓库里读到的（而不是已经装在用户级技能目录里），就问用户要不要装到全局，以后在任何目录都能让 agent 改设置。同意的话运行（需要 Node.js ≥ 22.20，`-a` 填你自己：`claude-code`、`codex`、`cursor`、`opencode`、`pi`）：

  ```bash
  npx skills add liuyingwen/clawd-desktop-buddy -g -a claude-code -y
  ```

## 改设置

```bash
uv run page/serve.py --set theme=2                   # 换主题
uv run page/serve.py --set rotation=2 brightness=60  # 可以一次改几项
uv run page/serve.py --status                        # 当前保存的值在 last_sent，还有诊断信息
```

| 键 | 取值 |
| --- | --- |
| `theme` | 1 经典（默认）、2 午夜、3 荧光终端、4 街机、5 剧场、6 地铁线路、7 深夜公寓、8 水族箱 |
| `rotation` | 0 / 1 / 2 / 3 对应 0° / 90° / 180° / 270°，默认 1 |
| `brightness` | 10–100，默认 100 |

- 设备不回话。`sent` 只说明已写进串口，要请用户看屏幕：设置有变化时屏幕会提示 3 秒，比如 `主题 午夜`
- `no_change`：和上次发的一样，没发。确实要重发就用 `uv run page/serve.py --resend`（重发全部已保存的设置）
- `saved_not_sent`：已保存但没发出去，按 `explain` 修好后再 `--resend`
- 方向不对：先 `--status` 看 `last_sent.rotation`。倒过来了就加 2，横着就加 1（还不对再加 2），都对 4 取余，发完问用户
- 主题 3–8 需要较新的固件，6–8 要的比 3–5 更新。选了之后屏幕没变化或提示 `主题 经典`，说明固件太旧：按[烧固件](#3-烧固件)重烧，再 `--resend`
- 用户想先看预览再挑：`uv run page/serve.py` 会打开配置页，10 分钟无操作自动关闭
- 不要自己往 daemon 的 TCP 端口发 `cfg` 行，配置页记录的「上次发送」会对不上

## 更新

1. 记下当前版本 `OLD=$(git rev-parse HEAD)`，再 `git pull`
2. 停掉 daemon（`pkill -f scripts/daemon.py`）。`git diff --quiet $OLD HEAD -- firmware/` 不为 0（固件有改动）时按[烧固件](#3-烧固件)重烧。然后 `uv run page/serve.py --resend`，会拉起新版 daemon 并重发设置
3. 按 [references/agents.md](references/agents.md) 里各 agent 的「更新」刷新插件。Codex 改了任何插件文件都要重新信任 hook，否则屏幕没反应
4. 本技能装在用户级目录的话，`npx skills update clawd-mood -g -y` 更新技能本身
5. 最后请用户重启各 agent（你自己的会话也一样，所以这步交给用户）
