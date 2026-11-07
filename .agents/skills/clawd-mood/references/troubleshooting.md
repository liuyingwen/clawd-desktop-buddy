# 排查

链路是 agent 的 hook → daemon（TCP 转串口）→ ESP32 固件，单向，设备不回话。先跑：

```bash
uv run page/serve.py --status
```

`first_break` 是第一处断开的环节（`explain` 是写给配置页的说明，按钮名不适用于你）：

| `first_break.link` | 处理 |
| --- | --- |
| `uv` | 装 uv |
| `usb` | 请用户插好板子，或指定串口（见下表「多块板子」） |
| `daemon` | `uv run page/serve.py --test` 会拉起它 |
| 整个是 `null` | 后台和 USB 都正常，问题在 agent 的 hook：`plugins` 里看插件痕迹，再按下表查对应 agent |

细节在 `daemon.alive`、`serial.usb_ok`、`log.errors`。再看 daemon 日志，每个事件应有一行 `-> {"state":...}`：

```bash
tail -n 50 ${TMPDIR:-/tmp}/clawd-mood-daemon.log      # mac/linux
Get-Content $env:TEMP\clawd-mood-daemon.log -Tail 50  # Windows
```

日志里有 `->` 行但屏幕没变，问题在设备；没有 `->` 行，问题在 hook 或 daemon。

## 常见情况

| 现象 | 处理 |
| --- | --- |
| 屏幕不亮 | 请用户检查接线，尤其 VCC 是否接 3V3 |
| 方向不对 | 见 SKILL.md「改设置」里方向的算法 |
| `No ESP32-like USB CDC device found` | 板子没插好，或数据线只能充电；Linux 检查 dialout 组；Windows 看设备管理器 |
| 多块板子，daemon 选错了 | 用户在配置页（`uv run page/serve.py`）的「串口设备」里选；或启动 daemon 时设 `CLAWD_MOOD_PORT=/dev/cu.usbmodemXXX`（Windows `COM7`） |
| 拔插 USB 后不更新 | daemon 不退出，下一条消息会触发重连，日志里有 `is back,`。启动时没插设备才会退出 |
| 设置发了屏幕没变 | 固件可能太旧，重烧后 `uv run page/serve.py --resend`。值没变化时屏幕不提示 |
| 端口 48756 被占 | daemon 自动换端口并写进 portfile，不用管；要固定端口设 `CLAWD_MOOD_PORT_TCP` |
| 改了插件文件不生效 | Codex、WorkBuddy、DeepSeek 原生插件用的是拷贝，按 [agents.md](agents.md) 的「更新」重装 |
| Codex 完全没反应 | hook 没信任或更新后信任失效，请用户在 codex 里 `/hooks` 信任；`codex plugin marketplace list` 看 marketplace 是否注册 |
| OpenCode 没反应 | 版本 ≥ 2.0、软链有效，`opencode reload` |
| Cursor 没反应 | 先看 `--status` 的 `plugins.cursor`（或 `~/.cursor/hooks.json` 里有没有带 `scripts/hook.sh ... cursor` 的条目），不是 `found` 就跑 `uv run tools/cursor_hooks.py install`（`hooks.json` 有注释时它不写入，按它打印的内容手动加）；要新开对话才生效。还不行请用户看输出面板的 Hooks 频道：没有 `hook.sh` 的记录，检查条目里的路径；有记录但报错，多半是从程序坞启动的 Cursor 既找不到 `uv` 也没有可用的 `python3`（macOS 要装 Command Line Tools：`xcode-select --install`），或者从终端用 `cursor` 命令启动。`cursor-agent -p` 本来就只显示 working |
| DeepSeek Harness 没反应 | 会话要提交过 prompt 才上屏。原生插件确认装在正在用的 profile（桌面版是 `desktop`）；桥接拉不起 daemon，先手动启动，再确认 `configPath` 是绝对路径 |

## 手动启动 daemon

```bash
uv run plugin/scripts/daemon.py     # 前台运行，看到 Ready! 即就绪
```

## 停掉 daemon

```bash
pkill -f scripts/daemon.py; rm -f ${TMPDIR:-/tmp}/clawd-mood.port     # mac/linux
```

```powershell
Get-Process python | Where-Object { $_.CommandLine -like '*daemon.py*' } | Stop-Process
Remove-Item $env:TEMP\clawd-mood.port -ErrorAction SilentlyContinue
```

下一个 agent 会话开始时，或 `uv run page/serve.py --set/--resend/--test` 时会自动拉起。
