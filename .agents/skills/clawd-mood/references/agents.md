# 各 agent 的接入、更新和卸载

命令都在仓库根目录执行，`<repo>` 指仓库的绝对路径。多个 agent 可以同时装、同时跑。

| agent | 接入方式 | waiting | error | 压缩 | 自动拉起 daemon |
| --- | --- | --- | --- | --- | --- |
| Claude Code | 插件 hooks | ✓ | ✓ | ✓ | ✓ |
| Codex CLI ≥ 0.133.0 | 插件 hooks | ✓ | — | ✓ | ✓ |
| OpenCode ≥ 2.0 | 进程内插件 | ✓ | ✓ | ✓ | ✓ |
| pi | 进程内扩展 | 仅扩展弹窗 | ✓ | ✓ | ✓ |
| WorkBuddy / CodeBuddy Code | 插件 hooks（同 Claude Code） | ✓ | ✓ | ✓ | ✓ |
| DeepSeek Harness | 原生插件 | ✓ | ✓ | — | ✓ |
| DeepSeek Harness | 官方 hook 桥接 | — | — | — | — |
| Cursor | 用户级 hooks | — | ✓ | 仅开始 | ✓ |

## Claude Code

```bash
claude plugin marketplace add ./
claude plugin install clawd-mood@clawd-mood
```

之后任意目录跑 `claude` 都会挂上。请用户在 claude 里运行 `/hooks`，确认 14 个事件都在。只想在当前会话临时用：`claude --plugin-dir <repo>/plugin`。

- 更新：`claude plugin marketplace update clawd-mood`，再 uninstall + install（插件版本号不一定变，`claude plugin update` 可能认为已是最新），然后请用户重启 claude。`--plugin-dir` 方式不用更新
- 卸载：`claude plugin uninstall clawd-mood@clawd-mood && claude plugin marketplace remove clawd-mood`

## Codex CLI（≥ 0.133.0）

```bash
codex plugin marketplace add .
codex plugin add clawd-mood@clawd-mood
```

**必须让用户在 codex 里运行 `/hooks`，审阅并信任 clawd-mood 的 12 个事件**，未信任的 hook 不会执行。Codex 对整个插件目录算哈希，插件里任何文件变了都要重新信任。

- 更新：`codex plugin remove clawd-mood@clawd-mood && codex plugin add clawd-mood@clawd-mood`，再让用户在 `/hooks` 重新信任（Codex 用的是拷贝，不重装就还是旧版）
- 卸载：`codex plugin remove clawd-mood@clawd-mood && codex plugin marketplace remove clawd-mood`

## OpenCode（≥ 2.0）

```bash
mkdir -p ~/.config/opencode/plugins
ln -s <repo>/plugin/adapters/opencode/clawd-mood.js ~/.config/opencode/plugins/clawd-mood.js
opencode reload
```

必须是软链，插件靠真实路径找到 `scripts/hook.py`。1.x 不支持。

- 更新：软链指向仓库，`git pull` 后 `opencode reload` 即可
- 卸载：`rm ~/.config/opencode/plugins/clawd-mood.js`

## pi

```bash
pi install <repo>/plugin/adapters/pi/clawd-mood.ts
```

- 卸载：`pi remove <repo>/plugin/adapters/pi/clawd-mood.ts`

## WorkBuddy / CodeBuddy Code

```bash
codebuddy plugin marketplace add .
codebuddy plugin install clawd-mood@clawd-mood
```

WorkBuddy 桌面版：命令前加 `CODEBUDDY_CONFIG_DIR=~/.workbuddy`，装完重启应用。`codebuddy` 在 `/Applications/WorkBuddy.app/Contents/Resources/app.asar.unpacked/cli/bin/codebuddy`。从访达启动时 PATH 里可能没有 `uv`。

- 更新：uninstall，删掉 `~/.workbuddy/plugins/cache/clawd-mood/clawd-mood/<版本>/`（uninstall 不删它，版本号没变时重装会继续用旧文件），再 install
- 卸载：`codebuddy plugin uninstall clawd-mood@clawd-mood && codebuddy plugin marketplace remove clawd-mood`

## DeepSeek Harness

两种接法二选一，不要同装，否则每个事件推两次。

**原生插件（推荐，桌面版只能用这种）**：打好的包在仓库的 `dist/` 里，按版本命名，不用自己打包，也不用克隆仓库。把最新版本的 GitHub 地址给用户：

```
https://raw.githubusercontent.com/liuyingwen/clawd-desktop-buddy/main/dist/clawd-mood-dsh-0.3.0.tgz
```

版本以仓库 `dist/` 里最新的 `clawd-mood-dsh-<版本>.tgz` 为准（本地有仓库时 `ls <repo>/dist`）。

- 桌面版：请用户在插件管理里粘贴这个地址安装。`dsh` 命令行不能操作桌面版的 `desktop` profile，只能用户自己点
- 命令行 / web 版：`dsh plugin --profile web add <上面的地址>`，然后重启 dsh
- 没网或想用本地文件：填 `<repo>/dist/clawd-mood-dsh-<版本>.tgz` 的绝对路径也行

- 更新：用新版本的地址再装一次，会替换旧版。不要指望同一个地址自己更新：每个版本的文件发布后不再改，pnpm 也会把同一地址的新内容当成被篡改而拒绝
- 卸载：桌面版在插件管理里移除；命令行 `dsh plugin --profile web remove clawd-mood-dsh`

**hook 桥接（只用于命令行 / web 版）**：

```bash
dsh plugin --profile web add @deepseek-ai/dsh-hooks-claude-code@<版本> @deepseek-ai/dsh-hook-protocol@<版本>   # 版本与 dsh --version 一致
```

在 `~/.dsh/profiles/web/cordis.patch.yml` 末尾加（两个路径都必须是绝对路径），然后重启 dsh：

```yaml
- insert:
    - id: clawd-mood-hooks
      name: '@deepseek-ai/dsh-hooks-claude-code'
      config:
        configPath: <repo>/plugin/hooks/hooks-dsh.json
        pluginRoot: <repo>/plugin
```

桥接拉不起 daemon，要用户手动启动或先让别的 agent 拉起；会话从第一条 prompt 才上屏。

- 卸载：从 `cordis.patch.yml` 删掉 `clawd-mood-hooks` 这段（别把文件删空，空 patch 会启动失败）

## Cursor

```bash
uv run tools/cursor_hooks.py install
```

往 `~/.cursor/hooks.json` 合并 8 个条目，别的工具的条目不动；文件里有注释时脚本不改写，会打印要手动加的内容。Cursor 自动重载，新开对话生效。条目指向仓库的绝对路径，仓库挪了位置要再 `install` 一次。

`cursor-agent -p`（非交互）只显示 working。没有 waiting，标签是工作区目录名，不显示子代理。

- 更新：再跑一次 `uv run tools/cursor_hooks.py install`（可重复执行，事件列表变了也能补上）
- 卸载：`uv run tools/cursor_hooks.py uninstall`

全部卸载后，按 [troubleshooting.md](troubleshooting.md#停掉-daemon) 停掉 daemon。本技能也要删的话：`npx skills remove --global clawd-mood`。
