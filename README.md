# clawd-mood

ESP32-C3 + ST7789 1.54" TFT 桌面摆件，实时显示 **Claude Code** / **Codex CLI** / **OpenCode** / **pi** / **WorkBuddy** / **DeepSeek Harness** / **Cursor** 的运行状态。每个会话是一只像素小生物，显示会话标题和当前状态；多个 agent 可以同时用。支持 macOS / Linux / Windows。

<p align="center">
  <img src="images/hero.png" width="48%" alt="clawd-mood on a coding desk next to a MacBook, its screen showing four sessions as a squad of pixel creatures" />
  <img src="images/with-claude-pandas.jpg" width="48%" alt="clawd-mood showing three sessions, flanked by two Claude panda figurines" />
</p>

## 屏幕样式

<table>
  <tr>
    <td align="center" width="33%"><img src="images/themes/classic.gif" width="240" alt="经典" /><br /><b>01 经典</b>（默认）</td>
    <td align="center" width="33%"><img src="images/themes/noir.gif" width="240" alt="午夜" /><br /><b>02 午夜</b></td>
    <td align="center" width="33%"><img src="images/themes/phosphor.gif" width="240" alt="荧光终端" /><br /><b>03 荧光终端</b></td>
  </tr>
  <tr>
    <td align="center"><img src="images/themes/arcade.gif" width="240" alt="街机" /><br /><b>04 街机</b></td>
    <td align="center"><img src="images/themes/stage.gif" width="240" alt="剧场" /><br /><b>05 剧场</b></td>
  </tr>
</table>

## 安装

需要一块 ESP32-C3 和一块 ST7789 1.54" 240×240 SPI 屏，按下表接线后用数据线插到电脑上。

### 接线（VCC 接 3.3V，**不要接 5V**）

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

### 让 agent 安装

先装上 clawd-mood 技能（需要 Node.js ≥ 22.20，会问你装给哪些 agent）：

```bash
npx skills add liuyingwen/clawd-desktop-buddy -g
```

再跟 agent 说「帮我安装 clawd-mood」。它会烧录固件、接上插件，最后在屏幕上放一个测试小人让你确认。之后换样式、排查、更新、卸载也都直接跟它说。

不想装技能，就把这段话发给 agent：

```text
请帮我安装 clawd-mood：https://github.com/liuyingwen/clawd-desktop-buddy
克隆到 ~/clawd-mood，按 .agents/skills/clawd-mood/SKILL.md 的步骤安装，只接你自己这个 CLI。
需要我动手的步骤告诉我；需要 sudo 或修改系统配置时先问我。
```

### 手动安装

先装好 [uv](https://docs.astral.sh/uv/getting-started/installation/)，再克隆仓库，插件要从这里加载：

```bash
git clone https://github.com/liuyingwen/clawd-desktop-buddy ~/clawd-mood
cd ~/clawd-mood
```

**1. 烧固件**，下面两种方式选一种。`<串口>` 换成板子的串口：macOS 用 `ls /dev/cu.usbmodem*` 查，Linux 用 `ls /dev/ttyACM*`（没权限就 `sudo usermod -aG dialout $USER` 后重新登录），Windows 在设备管理器里看 COM 号。

**方式一：直接烧录**（推荐）。下载预编译好的固件烧进去，不用装编译环境：

```bash
curl -LO https://github.com/liuyingwen/clawd-desktop-buddy/releases/latest/download/clawd-mood-esp32c3.bin
uvx --no-build-package cryptography esptool --chip esp32c3 --port <串口> write-flash 0x0 clawd-mood-esp32c3.bin
```

Windows PowerShell 里把 `curl` 换成 `curl.exe`，也可以到 [Releases](https://github.com/liuyingwen/clawd-desktop-buddy/releases/latest) 页面下载。以后重烧会清掉保存的样式，烧完运行 `uv run page/serve.py --resend` 恢复。

**方式二：从源码编译**。改了固件或接线不一样时用，要先装 [arduino-cli](https://arduino.github.io/arduino-cli/latest/installation/)，ESP32 板支持包要下载几个 GB。接线不一样就先改 `firmware/clawd_mood/clawd_mood.ino` 里的引脚。

```bash
arduino-cli core update-index --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core install esp32:esp32 --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli lib install "Adafruit GFX Library" "Adafruit ST7735 and ST7789 Library" "ArduinoJson" "U8g2_for_Adafruit_GFX"
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
arduino-cli upload -p <串口> -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

烧不进去：先关掉串口监视器，用过 clawd-mood 的话再停掉后台的 daemon（`pkill -f scripts/daemon.py`）；还不行就按住板子上的 BOOT 键再插 USB，然后重试。

**2. 接上 agent**：在 [agents.md](.agents/skills/clawd-mood/references/agents.md) 里找到你用的 agent，在仓库目录运行它下面的安装命令。

**3. 测试**：在仓库目录运行 `uv run page/serve.py --test`，屏幕上会出现一个测试小人，干一会儿活再离开。然后新开一个 agent 会话发句话，屏幕上就会出现它。

## 换样式

跟 agent 说「把 clawd-mood 换成午夜主题」「屏幕亮度调到 60」「屏幕倒过来了」就行。也可以打开配置页，看着预览自己挑：

```bash
uv run page/serve.py
```

![配置页](images/config-page.png)

主题 3–5 需要较新的固件，选了没反应就让 agent 重烧一次。

## 屏幕上显示什么

| 状态 | 经典主题里 | 什么时候 |
| --- | --- | --- |
| Idle | 灰 | 空闲 |
| Thinking | 蓝，齿轮 | 你发了消息，模型在想 |
| Working | 绿，键盘 | 调用工具 |
| Waiting | 黄，问号，闪烁 | 等你批准或回答（Cursor 没有） |
| Done | 白，对勾 | 回答完成，3 秒后回到 Idle |
| Error | 红，叉号 | 回合出错，比如 API 报错 |
| Sleeping | 深灰，Z | 5 分钟没动静 |

每个工具一个由 logo 拟人的小人，子代理是它头顶的小跟班。会话多了自动翻页，有会话等你批准或出错时会先翻到它。

屏幕没反应、想更新或卸载，也直接跟 agent 说。

## 已知限制

- Windows 和 Linux 还没在真机上完整跑过，试过的话欢迎开 issue。
- 主题、方向和亮度设置还没在真机上确认过，主题 3–5 只在预览里看过。

## 许可证

[Apache License 2.0](LICENSE)。Copyright 2026 刘英文。归属说明见 [NOTICE](NOTICE)。
