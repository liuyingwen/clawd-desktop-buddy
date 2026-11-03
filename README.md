# clawd-mood

ESP32-C3 + ST7789 1.54" TFT 桌面摆件，USB 串口实时显示 **Claude Code** / **OpenAI Codex CLI** / **OpenCode** / **pi** / **WorkBuddy（CodeBuddy Code）** / **DeepSeek Harness** / **Cursor** 的运行状态。每个会话是一只像素小生物（每个工具一个由 logo 拟人的角色），同时显示会话标题和 7 种状态之一（idle / thinking / working / waiting / done / error / sleeping）。各端可独立挂、可同装、可同时跑。跨平台：macOS / Linux / Windows。

以 [Apache License 2.0](LICENSE) 开源。

**把下面这段话发给你的 agent，它会照着完成安装**（硬件要先按[接线表](#3-接线vcc-接-33v不要接-5v)接好并插上 USB）：

```text
请帮我安装 clawd-mood：https://github.com/liuyingwen/clawd-desktop-buddy
1. 克隆仓库，阅读 README.md，按其中的步骤操作。
2. 安装依赖和固件所需的板支持、库，找到 ESP32-C3 的串口，编译并烧录固件。
3. 判断你自己是哪个 CLI，只挂载对应的插件；有需要我手动完成的步骤（比如 Codex 的 /hooks 信任、重启应用）就告诉我。
4. 启动 daemon，灌一条 {"state":"working"} 测试消息，然后问我屏幕有没有变化。
需要 sudo 或修改系统配置时，先征求我的同意。
```

<p align="center">
  <img src="images/hero.png" width="48%" alt="clawd-mood on a coding desk next to a MacBook, its screen showing four sessions as a squad of pixel creatures" />
  <img src="images/with-claude-pandas.jpg" width="48%" alt="clawd-mood showing three sessions, flanked by two Claude panda figurines" />
</p>

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
arduino-cli lib install "Adafruit GFX Library" "Adafruit ST7735 and ST7789 Library" "ArduinoJson" "U8g2_for_Adafruit_GFX"
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
  Portfile: /var/folders/.../T/clawd-mood.port
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
claude plugin marketplace add ./
claude plugin install clawd-mood@clawd-mood
```

之后任意目录跑 `claude` 都自动挂上，无需 `--plugin-dir`。`/hooks` 确认 14 个事件全挂上。卸载：

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

**Codex 默认不执行未信任的 hook**：装完后在 codex 里运行 `/hooks`，审阅并信任 clawd-mood 的 12 个事件，否则屏幕不会有反应。Codex 对整个插件目录算哈希，插件里任何文件变更（包括升级版本）后都要重新信任。

之后任意目录 `codex` 即可。

卸载：

```bash
codex plugin remove clawd-mood@clawd-mood
codex plugin marketplace remove clawd-mood
```

> Codex 是把 plugin **拷贝**到 `~/.codex/plugins/cache/`，改了 `plugin/` 下的文件后需要 `codex plugin remove/add` 才生效，之后还要在 `/hooks` 重新信任。Claude Code 的 `--plugin-dir` 是 live path，无此问题。

#### OpenCode（≥ 2.0）

把插件软链到全局插件目录，然后重载：

```bash
mkdir -p ~/.config/opencode/plugins
ln -s /path/to/clawd-mood/plugin/adapters/opencode/clawd-mood.js ~/.config/opencode/plugins/clawd-mood.js
opencode reload
```

必须用软链（或插件能解析回仓库的方式），插件靠真实路径定位 `scripts/hook.py`。V1 插件接口在 OpenCode 2 不生效，所以不支持 1.x。卸载：删掉这个软链。

#### pi

```bash
pi install /path/to/clawd-mood/plugin/adapters/pi/clawd-mood.ts
```

卸载：`pi remove /path/to/clawd-mood/plugin/adapters/pi/clawd-mood.ts`。

#### WorkBuddy / CodeBuddy Code

```bash
cd /path/to/clawd-mood
codebuddy plugin marketplace add .
codebuddy plugin install clawd-mood@clawd-mood
```

WorkBuddy 桌面版使用自己的配置目录，命令前加 `CODEBUDDY_CONFIG_DIR=~/.workbuddy`，再重启应用。`codebuddy` 可执行文件在 `/Applications/WorkBuddy.app/Contents/Resources/app.asar.unpacked/cli/bin/codebuddy`。桌面版尚未端到端验证；从访达启动时 `PATH` 里可能没有 `uv`，屏幕没反应时先查这里。

#### DeepSeek Harness

有两种接法，**二选一，不要同装**，否则每个事件会推两次。

**原生插件（推荐，桌面版只能用这种）**：在进程内直接把事件映射成状态，能显示 waiting 和 error，会自动拉起 daemon，dsh 启动时恢复的历史会话不会上屏。先打包：

```bash
cd plugin/adapters/dsh && npm run pack:tarball   # 产出 dist/clawd-mood-dsh-<版本>.tgz，自带 hook.py / daemon.py
```

桌面版：在插件管理里填这个 `.tgz` 的绝对路径安装，装完即启用。命令行 / web 版：`dsh plugin --profile web add /绝对路径/clawd-mood-dsh-<版本>.tgz`，然后重启 dsh。`dsh` 命令行不能操作桌面版的 `desktop` profile。

**hook 桥接**：走官方的 Claude Code hook 桥接，只用于命令行 / web 版。先在要用的 profile 里装桥接包（版本与 `dsh --version` 一致）：

```bash
dsh plugin --profile web add @deepseek-ai/dsh-hooks-claude-code@<版本> @deepseek-ai/dsh-hook-protocol@<版本>
```

然后在 `~/.dsh/profiles/web/cordis.patch.yml` 末尾加一段（**两个路径都必须是绝对路径**），并重启 dsh：

```yaml
- insert:
    - id: clawd-mood-hooks
      name: '@deepseek-ai/dsh-hooks-claude-code'
      config:
        configPath: /path/to/clawd-mood/plugin/hooks/hooks-dsh.json
        pluginRoot: /path/to/clawd-mood/plugin
```

限制：桥接只支持 5 个事件，所以**不显示 waiting 和 error**，也没有压缩状态；会话从第一条 prompt 才上屏。hook 跑在 dsh 沙箱里，`uv` 无法运行，因此改用系统 `python3`，且**无法自动拉起 daemon**——请手动启动，或先让其他 agent 拉起。

#### Cursor

桌面版和命令行 `cursor-agent` 都读用户级的 `~/.cursor/hooks.json`。用脚本往里合并 8 个条目，文件里其他工具的条目不受影响：

```bash
cd /path/to/clawd-mood
uv run tools/cursor_hooks.py install
```

Cursor 会自动重载这个文件，新开一个对话就生效。条目指向仓库里 `plugin/scripts/hook.py` 的绝对路径，改了代码不用重装；仓库挪了位置要再跑一次 `install`。`hooks.json` 里有注释时脚本不会改写它，而是打印要手动加的内容。执行情况在 Customize 的 Hooks 页，或输出面板的 Hooks 频道。

限制：

- Cursor 没有审批事件，所以**不显示 waiting**；不读会话标题，标签是工作区目录名；子代理不显示小跟班（它的结束事件不带子代理 ID）
- hook 是同步执行的，每次工具调用多等约 50ms
- `cursor-agent -p`（非交互）不触发提交和结束事件，只显示 working，进程退出时移除
- 不用 Cursor 插件的形式，因为命令行只在用户级或项目级 hooks 里配了提交和结束事件时才会触发它们，只装插件就看不到 thinking 和 done
- Cursor 默认会导入已装的 Claude Code 插件；导入的那份 hook 在 Cursor 里会自动跳过，不会重复显示
- 桌面版尚未端到端验证（命令行已验证）

#### 同时挂

多端可同时跑，每个会话各占一格，左上角标来源：`CC`（Claude Code）、`CX`（Codex）、`OC`（OpenCode）、`PI`（pi）、`WB`（WorkBuddy / CodeBuddy Code）、`DS`（DeepSeek Harness）、`CU`（Cursor）。daemon 单实例、TCP server 多客户端，互不冲突。OpenCode 和 pi 按进程计，一个进程一只。

### 3. 屏幕说明

默认主题「经典」每页最多显示 4 个会话并撑满屏幕：1 个全屏，2 个左右拆分，3 个左大右二，4 个 2×2。超过 4 个时每 5 秒自动翻页；别的页有会话进入 Waiting 或 Error 时会立即翻过去，停留到状态解除，最多 10 秒；单次工具失败只显示 Error，不翻页，因为 agent 通常会继续干活。别的主题每页放几个、怎么排不一样（荧光终端每页 6 行，街机 12 个全在一屏，剧场每页一个），但最多记 12 个会话、紧急状态优先显示这些规则都一样，见[配置页](#5-配置页换主题调方向和亮度)。

每格下方显示会话标题，还没有标题时显示目录名。标题读自 CLI 的内部数据（目前支持 Claude Code 和 Codex），读不到时同样显示目录名。中文支持 GB2312 字符集，范围外的字符和 emoji 不显示。

不同工具显示不同的小人，造型来自各自的 logo。下面的造型和颜色是默认主题「经典」的，换主题后状态含义不变，外观以主题为准（见[配置页](#5-配置页换主题调方向和亮度)）。Claude Code 是橙色 Clawd，Codex 是头戴 OpenAI 结的白色小人，OpenCode 是黑色空心方框，pi 是独眼的像素 P，DeepSeek 是蓝鲸，WorkBuddy 是青绿色的猫，Cursor 是灰色的等距立方体。同一工具的多个会话长得一样，靠标题区分。

| 状态     | 地砖色 / 右上角图标 | 触发                                                                  |
| -------- | ------------- | --------------------------------------------------------------------- |
| Idle     | 灰 / 无 | 会话开始或结束 / Stop 后 3 秒回归 / 中断回合（Codex、OpenCode、pi、Cursor） |
| Thinking | 蓝 / 齿轮 | 提交 prompt / 压缩上下文中                                            |
| Working  | 绿 / 键盘 | 调用工具 / 压缩完恢复                                                 |
| Waiting  | 黄 / 问号，格子闪烁 | 等你批准或回答问题（DeepSeek Harness 桥接、Cursor 无）                    |
| Done     | 白 / 对勾 | 回答完成（3 秒瞬态）                                                  |
| Error    | 红 / 叉号，红框 | 工具失败 / API 报错（Claude、WorkBuddy、OpenCode、pi、Cursor）         |
| Sleeping | 深灰 / Z | 5 分钟无事件（Working 为 15 分钟，Waiting 不睡）；30 分钟无事件的会话会从屏幕移除 |

各端能显示的状态：

| 端               | 接入方式                  | waiting | error | 压缩 | 自动拉起 daemon |
| ---------------- | ------------------------- | ------- | ----- | ---- | --------------- |
| Claude Code      | 插件 hooks                | ✓       | ✓     | ✓    | ✓               |
| Codex CLI        | 插件 hooks                | ✓       | —     | ✓    | ✓               |
| OpenCode ≥ 2.0   | 进程内插件                | ✓       | ✓     | ✓    | ✓               |
| pi               | 进程内扩展                | 仅扩展弹窗 | ✓  | ✓    | ✓               |
| WorkBuddy        | 插件 hooks（同 Claude）   | ✓       | ✓     | ✓    | ✓               |
| DeepSeek Harness | 原生插件                  | ✓       | ✓     | —    | ✓               |
| DeepSeek Harness | 官方 Claude Code hook 桥接 | —      | —     | —    | —               |
| Cursor           | 用户级 hooks（同步）      | —       | ✓     | 仅开始 | ✓             |

### 4. 快速验证

挂载完，跑一个任意短任务（Claude Code 或 Codex 都行），观察屏幕上该会话的状态序列：

```
Idle → Thinking → Working → Done → Idle（Done 3 秒后自动回 Idle）
```

Codex 特有事件验证：

| 触发动作                  | 期望状态     | 对应事件             |
| ------------------------- | ------------ | -------------------- |
| codex 弹出命令批准提示    | Waiting      | `PermissionRequest`  |
| codex `/compact` 压缩中   | Thinking     | `PreCompact`         |
| codex 压缩完成            | Working      | `PostCompact`        |

不依赖 CLI 直接灌一个 state 看屏幕：

```bash
# mac/linux
printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat ${TMPDIR:-/tmp}/clawd-mood.port)
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
tail -f ${TMPDIR:-/tmp}/clawd-mood-daemon.log

# Windows PowerShell
Get-Content $env:TEMP\clawd-mood-daemon.log -Wait
```

每次事件应新增一行 `-> {"state":"...","event":"...","tool":"...","session":"...","name":"...","src":"CC"}`，有标题时还带 `title`。

### 5. 配置页：换主题、调方向和亮度

在仓库根目录运行，会打开浏览器：

```bash
uv run page/serve.py

# 只打印地址，不打开浏览器
uv run page/serve.py --no-open
```

```powershell
# Windows PowerShell（untested）
uv run page\serve.py
```

页面只监听本机，地址里带一次性令牌。10 分钟没有操作会自动关闭（最长 1 小时），重新运行上面的命令即可。daemon 没在跑时，页面发设置前会把它拉起来。

| 主题 | 风格 |
| ---- | ---- |
| 1 经典（默认） | 奶油色桌面，小队的原始模样 |
| 2 午夜 | 深夜霓虹，暗场里只亮状态 |
| 3 荧光终端 | 绿色荧光屏上的会话进程表 |
| 4 街机 | 小队变成入侵者阵列 |
| 5 剧场 | 聚光灯下只演一个主角 |

主题 3–5 不是分格子的：荧光终端每行一个会话，每页 6 行，翻页时整表上滚；街机把全部会话排成一个入侵者阵列，飞船和底部字幕轮流对准每个会话，有会话等你批准或出错时先对准它；剧场每页一个会话站在聚光灯下，其他会话坐在前排，翻页时拉幕。

**主题 3–5 要重新烧固件。** 它们比主题 1–2 晚加入，已经烧过 0.3.0 的板子也要重烧一次（命令见[编译 + 上传](#5-编译--上传)），烧完在配置页按「重新发送全部设置」。没重烧的固件不认识它们，收到后会退回「经典」，并且按「经典」存进闪存，所以重烧后要再发一次。页面不知道板子上是哪个固件，这几个卡带上都标着「需新版固件」。

- **选主题**：点卡带只切换页面上的预览，预览是真实固件画出来的帧。按「应用到屏幕」才发给设备，而且只发改过的项。
- **屏幕方向**：0° / 90° / 180° / 270°，默认 90°（`rotation` 1，当前面板的安装方向）。
- **背光亮度**：10–100，默认 100。
- **串口设备**：多块板子时在这里选，存到 `~/.clawd-mood/config.json`，保存后会重启 daemon。优先级低于环境变量 `CLAWD_MOOD_PORT`。Windows 上要手动重启 daemon 才生效。
- **发送测试小人**：屏幕上出现一个「测试小人」，工作、完成，几秒后离开。
- **诊断信息**：出问题时看这里，能看到 daemon、串口、日志。

链路是单向的，设备不回话，所以页面只能告诉你「已发送，请看屏幕确认」，表示 daemon 已把这行写进串口。屏幕上提示 3 秒（比如 `主题 午夜`）才算生效。看到「已保存，设备未连接，未发送」时，插上设备再按一次「应用到屏幕」；只写「已保存，未发送」时，问题在后台，下面一行写着断在哪。设置存在板子的闪存里。

不开页面也能设置，键都可选：

```bash
printf '{"cfg":{"theme":2,"rotation":1,"brightness":60}}\n' | nc 127.0.0.1 $(cat ${TMPDIR:-/tmp}/clawd-mood.port)
```

> ⚠️ **主题和设置还没在真实硬件上确认过。** 固件已烧进一块板子，但屏幕实际显示、方向、调光、断电保存、午夜主题的帧率都还没人检查过；主题 3–5 只在主机预览里看过，还没上板。已验证的是固件编译、主机上的帧输出和假串口上的完整链路。试过的话请开 issue 反馈结果。

想自己加主题，见[设计说明书](docs/superpowers/specs/2026-11-02-config-page-themes-design.md#加一个主题)。

### 6. 从旧版本更新到 0.3.0

- **所有人：重新烧一次固件才有主题**，命令见[编译 + 上传](#5-编译--上传)。旧固件会忽略设置行，状态显示不受影响，但页面仍会显示「已发送」。烧完后在配置页按「重新发送全部设置」。
- **Codex：要重新批准 hook。** Codex 对整个插件目录算哈希，0.3.0 改了插件文件。重装后在 codex 里运行 `/hooks`，重新信任 12 个事件，否则屏幕没反应：

  ```bash
  codex plugin remove clawd-mood@clawd-mood
  codex plugin add clawd-mood@clawd-mood
  ```

- **DeepSeek Harness 原生插件**：`.tgz` 里带着旧的 `daemon.py`，要重新 `npm run pack:tarball` 并安装新的包。
- **正在跑的 daemon** 还是旧版本，`pkill -f scripts/daemon.py` 后下次会话启动时会拉起新的（Windows 见[卸载](#卸载)里的停止命令）。

---

## 卸载

```bash
# Claude Code
claude plugin uninstall clawd-mood@clawd-mood
claude plugin marketplace remove clawd-mood

# Codex
codex plugin remove clawd-mood@clawd-mood
codex plugin marketplace remove clawd-mood

# OpenCode
rm ~/.config/opencode/plugins/clawd-mood.js

# pi
pi remove /path/to/clawd-mood/plugin/adapters/pi/clawd-mood.ts

# WorkBuddy / CodeBuddy Code
codebuddy plugin uninstall clawd-mood@clawd-mood
codebuddy plugin marketplace remove clawd-mood

# Cursor（只删 clawd-mood 的条目）
uv run tools/cursor_hooks.py uninstall

# DeepSeek Harness 原生插件：桌面版在插件管理里移除；命令行 / web 版
dsh plugin --profile web remove clawd-mood-dsh
# DeepSeek Harness 桥接：从 cordis.patch.yml 删掉 clawd-mood-hooks 这段（别把文件删空，空 patch 会导致启动失败）
```

停 daemon + 清理：

```bash
# mac/linux
pkill -f scripts/daemon.py; rm -f ${TMPDIR:-/tmp}/clawd-mood.port
```

```powershell
# Windows PowerShell
Get-Process python | Where-Object { $_.CommandLine -like '*daemon.py*' } | Stop-Process
Remove-Item $env:TEMP\clawd-mood.port -ErrorAction SilentlyContinue
```

---

## 故障排除

- **屏幕不亮**：检查接线，特别是 VCC 是不是接到 3V3 而不是 5V
- **屏幕方向不对**：在[配置页](#5-配置页换主题调方向和亮度)改「屏幕方向」。要改默认值，改 `firmware/clawd_mood/clawd_mood.ino` 里的 `DEFAULT_ROTATION`（0/2/3）重烧
- **配置页显示已发送，屏幕没变化**：固件可能还是旧的，重烧一次（见[更新到 0.3.0](#6-从旧版本更新到-030)），再按「重新发送全部设置」。新固件在设置确实变化时会提示 3 秒，值没变不提示
- **拔掉 USB 后屏幕不更新**：daemon 不会退出，拔掉期间的消息直接丢弃。插回后下一条消息触发重连（写失败时立刻试一次，之后最多每 3 秒试一次；打开串口约 3.5 秒，期间的消息排队），daemon 日志里会有 `is back,`。启动 daemon 时没插设备才会退出
- **`No ESP32-like USB CDC device found`**：板子没插好，或者 USB 数据线只能充电不能传输；Linux 检查是否加入 `dialout` 组；Windows 检查设备管理器有无未识别设备
- **多个 ESP32 同时插**：daemon 按「环境变量 → 配置页保存的串口 → 自动扫描」选端口，扫描时优先 ESP32 原生 USB，剩多个时取排序后的第一个并在日志里警告。可以在配置页选，或用环境变量指定端口
  - mac/linux: `CLAWD_MOOD_PORT=/dev/cu.usbmodemXXX ./plugin/scripts/daemon.py`
  - Windows PowerShell: `$env:CLAWD_MOOD_PORT='COM7'; uv run plugin\scripts\daemon.py`
- **默认 TCP 端口 48756 被占**：daemon 会自动回退到 OS 分配的空闲端口（写进 portfile），hook 读 portfile 自动跟随。要强制指定：`CLAWD_MOOD_PORT_TCP=49000 ./plugin/scripts/daemon.py`（显式指定时冲突会直接报错退出）
- **Codex 不识别插件**：检查 `codex plugin marketplace list` 看本地 marketplace 是否注册；若 marketplace 路径变了，先 `codex plugin marketplace remove clawd-mood` 再 `add` 一次
- **Codex 改完 hook.py 没生效**：codex 是拷贝缓存，要 `codex plugin remove clawd-mood@clawd-mood && codex plugin add clawd-mood@clawd-mood` 刷新
- **Codex 里屏幕完全没反应**：hook 还没被信任，或更新插件后信任失效了，在 codex 里运行 `/hooks` 审阅并信任
- **OpenCode 屏幕没反应**：确认版本 ≥ 2.0、软链有效，运行 `opencode reload` 或重启后台服务让插件重新加载
- **Cursor 屏幕没反应**：在输出面板的 Hooks 频道看有没有 `hook.py` 的执行记录。没有就确认 `~/.cursor/hooks.json` 里有 clawd-mood 的条目，路径指向现在的仓库；有记录但报错，多半是从程序坞启动的 Cursor 找不到 `uv`
- **DeepSeek Harness 屏幕没反应**：会话要提交过 prompt 才上屏。原生插件：确认装在了正在用的 profile（桌面版是 `desktop`）。桥接：daemon 不会被它拉起，先手动启动；再确认 profile 里装了桥接包、`configPath` 是绝对路径（路径错误时桥接只打警告，不注册 hook）

## 许可证

[Apache License 2.0](LICENSE)。Copyright 2026 刘英文。归属说明见 [NOTICE](NOTICE)。
