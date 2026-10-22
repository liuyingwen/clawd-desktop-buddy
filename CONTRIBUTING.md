# 参与贡献

感谢你愿意改进 clawd-mood。改动前先看 [README](README.md) 里的架构和范围；设计背景在 `docs/superpowers/specs/`。

## 怎么改

1. Fork 本仓库，从 `main` 拉分支。
2. 改动尽量小，并写清它对应的行为（固件表情、hook 事件映射，或 daemon 串口桥）。
3. 开 Pull Request。说明你怎么验证的：真机、Wokwi，或只做了代码审查。

仓库没有自动化测试。硬件和串口路径靠手测。PR 里写上你实际跑过的命令和看到的表情序列。

## 改代码时别踩的约定

- 屏幕 **VCC 接 3.3V**。文档和注释里不要写成 5V。
- 烧录参数保持 `CDCOnBoot=cdc`。关掉 USB CDC On Boot 后，daemon 找不到串口。
- SPI 用 `SPI.begin(8, -1, 10, TFT_CS)`（SCK=8，MOSI=10）。
- `tft.setRotation(1)` 对应当前面板安装方向。只有使用者反馈方向不对时才改。
- 状态字符串小写且严格匹配：`idle`、`thinking`、`working`、`waiting`、`done`、`error`、`sleeping`。固件 `parseMood` 用 `strcmp`，大小写敏感。
- `plugin/scripts/hook.py` 里 `PreToolUse`、`PostToolUse`、`SubagentStart`、`SubagentStop` 都映射到 `working`。改 `EVENT_TO_STATE` 时保持这个收敛，避免工具调用过程中表情来回跳。
- `done` 在固件里 3 秒后回到 `idle`，5 分钟没有串口消息进入 `sleeping`。这两处是固件本地计时，不要只在 hook 里模拟。
- daemon 在 `Stop` 时不退出，避免多个 CLI 会话互相关掉对方的进程。
- Claude Code 的 hook 带 `"async": true`。Codex 的 `hooks-codex.json` 不带 async（Codex CLI 0.133.0 尚不支持）。两端共用同一个 `hook.py`。
- Codex 会把插件拷贝到 `~/.codex/plugins/cache/`。改完 `plugin/` 后需要 `codex plugin remove` 再 `add`，缓存才会更新。Claude Code 的 `--plugin-dir` 直接读仓库。

## 欢迎的反馈

- Windows 或 Linux 上的真机结果（成功或失败都有用）。
- 接线、烧录、daemon 日志对不上时的复现步骤。请用 [Bug 模板](.github/ISSUE_TEMPLATE/bug_report.yml)，并附上 daemon 日志（macOS / Linux：`/tmp/clawd-mood-daemon.log`，Windows：`%TEMP%\clawd-mood-daemon.log`）。

## 许可证

提交到本仓库的贡献以 [MIT License](LICENSE) 授权。Copyright (c) 2026 liuyingwen。

---

# Contributing

Thanks for improving clawd-mood. Read the architecture in the [README](README.md) first. Design notes live under `docs/superpowers/specs/`.

1. Fork and branch from `main`.
2. Keep the change focused on firmware expressions, the hook event map, or the serial daemon.
3. Open a pull request and say how you checked it: real hardware, Wokwi, or review only.

There is no automated test suite. Describe the commands you ran and the expression sequence you saw.

Please keep these behaviors:

- Display VCC stays on 3.3V.
- Firmware uploads keep `CDCOnBoot=cdc`, and SPI stays `SPI.begin(8, -1, 10, TFT_CS)`.
- Leave `tft.setRotation(1)` unless a user reports the panel is mounted the other way.
- State names stay lowercase: `idle`, `thinking`, `working`, `waiting`, `done`, `error`, `sleeping`.
- Tool and subagent events in `EVENT_TO_STATE` stay collapsed to `working`.
- The 3-second Done timeout and the 5-minute sleep timeout stay in the firmware.
- The daemon keeps running across `Stop`, so one CLI session does not kill another session's bridge.
- Claude Code hooks set `"async": true`. Codex hooks in `hooks-codex.json` do not (Codex CLI 0.133.0 has no async hooks). Both call `hook.py`.
- After editing `plugin/`, reinstall the Codex plugin so its cache copy updates.

Hardware reports from Windows and Linux are welcome. File bugs with the issue template and attach the daemon log.

Contributions are accepted under the [MIT License](LICENSE). Copyright (c) 2026 liuyingwen.
