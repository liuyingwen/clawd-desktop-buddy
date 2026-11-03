# Cursor 支持设计

## 目标

让摆件显示 Cursor agent（桌面版 Agent 聊天和 `cursor-agent` 命令行）的状态。daemon 和串口协议不改；固件只多一个 `CU` 造型。

## 接入方式

Cursor 有命令式 hook（`hooks.json`，每个事件起一个进程，stdin 传 JSON），归入「命令式 hook」一类，复用 `hook.py`，传 `cursor` 参数。

安装由 `tools/cursor_hooks.py install` 完成：往用户级 `~/.cursor/hooks.json` 的 8 个事件各追加一条 `uv run '<仓库>/plugin/scripts/hook.py' cursor || true`（`timeout: 5`）。重复安装先删掉旧条目再加，仓库挪了位置重跑即可；`uninstall` 只删命令里带 `scripts/hook.py ... cursor` 的条目。文件里有注释（Cursor 接受 JSONC）时不改写，打印要手动加的内容。配置页的「插件痕迹」按同样的规则在这个文件里找 clawd-mood 的条目。

## 关键决策

**用户级 hooks，不做 Cursor 插件。** 最初做的是 Cursor Plugin（`.cursor-plugin/plugin.json` + 插件内 hooks 文件），实测 `cursor-agent` 2026.10.14 有个限制：`beforeSubmitPrompt`、`stop`、`afterAgentResponse` 只有在用户级或项目级 hooks 配了同名事件时才执行。一旦配了，插件里的同名 hook 也会一起执行（日志里每个事件出现两次）；只装插件时这三个事件一次都不触发。代码里交互界面执行 `beforeSubmitPrompt` 前检查的正是 `userHooks || projectHooks`。只靠插件，命令行上就没有 thinking 和 done，回合结束后一直停在 working。用户级 hooks 在桌面版和命令行都是一等公民，而且指向仓库的绝对路径，改代码立即生效；插件在桌面版要拷贝到 `~/.cursor/plugins/local/`（不认指向外面的软链）。代价是要改用户的 `hooks.json`，所以用脚本合并，不碰别的工具的条目。

**挂 `preToolUse`，但 `hook.py` 绝不写 stdout。** Cursor 没有 `async` 选项，所有 hook 都同步执行。`preToolUse` 是权限 hook：读到不合法的 JSON 会拦截工具调用，读到空输出则记一条 "produced no output" 后放行（`failClosed` 默认 false；Cursor 3.27 的代码和 CLI 实测都确认了）。只挂 `postToolUse` 的话，第一个工具执行完之前一直是 thinking，长命令期间看不出在干活。每个 hook 约多 50ms（`uv run` 启动），可以接受。

**`|| true` 和 `timeout: 5`。** `uv` 自身出错退出码是 2，Cursor 和 Claude Code 一样把 2 当作拦截。超时（Cursor 记为 124）和其他非零退出码都放行。

**跳过导入的 Claude Code hook。** Cursor 默认开着 Third-Party Imports，从 3.27 的代码看，它会读 `~/.claude/plugins/installed_plugins.json` 加载已装的 Claude Code 插件，并把 Claude 格式的 hook 转成自己的格式同步执行（转换时只保留 command / timeout / matcher，`async` 被丢掉）。装过 Claude Code 版 clawd-mood 的用户，在 Cursor 里同一事件会多跑一次 `hook.py`。Claude Code 的 payload 不会有 `cursor_version`，所以 `hook.py` 见到它却没带 `cursor` 参数时直接退出。不靠环境变量判断：在 Cursor 内置终端里跑 `claude` 时，环境变量可能来自 Cursor。

**会话身份用 `conversation_id` 和 `workspace_roots[0]`。** 每个事件都带 `conversation_id`（`sessionStart` / `sessionEnd` 的 `session_id` 与它相同）。`cwd` 只有工具事件才带，而且可能是子目录，用它当标签会来回跳；用户级 hook 的工作目录是 `~/.cursor/`，也不能用。

**不读标题。** Cursor 的聊天标题存在它自己的数据库里，`transcript_path` 指向的 jsonl 只有 role / message 记录。读 transcript 还会给每个同步 hook 增加延迟。

## 事件映射

`hook.py` 先把 Cursor 的事件名换成 `EVENT_TO_STATE` 里的名字：

| Cursor 事件 | 换成 | state |
| ----------- | ---- | ----- |
| `sessionStart` | `SessionStart` | idle（并拉起 daemon） |
| `beforeSubmitPrompt` | `UserPromptSubmit` | thinking |
| `preToolUse` / `postToolUse` | `PreToolUse` / `PostToolUse` | working |
| `postToolUseFailure` | `PostToolUseFailure` | working（和其他 agent 一样，工具失败不算错误）；`is_interrupt` 时丢弃 |
| `preCompact` | `PreCompact` | thinking |
| `stop`，`status: completed` | `Stop` | done |
| `stop`，`status: aborted` | `Interrupt` | idle |
| `stop`，`status: error` | `StopFailure` | error |
| `sessionEnd` | `SessionEnd` | idle，`end: true` |

不挂的事件：

- `subagentStart` / `subagentStop`：没有小跟班。`subagentStart` 带 `subagent_id`，但 `subagentStop` 不带，对不上该移除哪个跟班；只能靠固件超时移除的跟班不如不显示
- `beforeShellExecution` / `beforeMCPExecution` / `beforeReadFile`：权限 hook，且被 `preToolUse` 覆盖
- `afterShellExecution` / `afterMCPExecution` / `afterFileEdit`：被 `postToolUse` 覆盖，多挂只会多等
- `afterAgentResponse` / `afterAgentThought`：不对应状态变化

## 验证

`cursor-agent` 2026.10.14，把脚本生成的条目装进临时工作区的 `.cursor/hooks.json`（项目级，和用户级走同一个触发条件），`TMPDIR` 指到临时目录并放一个假 daemon：

- 交互模式，读一个文件后回答：`SessionStart` idle → `UserPromptSubmit` thinking → `PreToolUse` / `PostToolUse` working（Read 没被拦） → `Stop` done → 退出时 `SessionEnd`
- `-p` 模式：只有 `sessionStart`、工具事件、`sessionEnd`，项目级 hooks 也一样
- 实测 payload：hook 继承 agent 进程的环境（包括 `TMPDIR`），`workspace_roots` 是 `--workspace` 指的目录，命令行的 `cursor_version` 形如 `2026.10.14-941492e`

桌面版没有端到端验证。

## 已知限制

- 没有 waiting：Cursor 没有审批或提问事件，`beforeShellExecution` 发生在审批之前，但看不出这次是否需要审批
- 压缩没有结束事件，压缩完靠下一个工具事件回到 working；回合外的压缩会停在 thinking，5 分钟后睡眠
- `cursor-agent -p` 没有 thinking 和 done，进程退出时会话被移除
- Windows 未验证：Cursor 在 Windows 上用 PowerShell 执行 hook 命令，脚本在 Windows 上生成不带 `|| true` 的命令（PowerShell 5.1 里是语法错误）
- 子代理的工具事件带哪个 `conversation_id` 未验证；如果是子代理自己的，会多出一只临时小人，30 分钟后移除
- 云端 agent 不跑 `sessionStart` / `sessionEnd`、不读用户级 hooks，也连不到本机 daemon，不考虑

## 固件

`SKIN_COUNT` 从 6 改成 7，每个主题的 `SKINS` 末尾加一项 `CU`：等距立方体，顶面是 accent（A），左面是 body（B），右面是 face（F），方眼分在左右两个面上。轮廓和眼睛位置五个主题相同，颜色按各主题的规矩：

| 主题 | 顶 / 左 / 右 | 眼 | 说明 |
| ---- | ------------ | -- | ---- |
| 经典 | 灰 / 深灰 / 近黑 | 白 | 顶面压到 136 灰，在奶油底上对比度过 3 |
| 午夜 | PAPER / SLATE / 靛蓝 | INK | 纸白眼睛在 SLATE 上不够 3，改用深色眼睛 |
| 荧光终端 | MID / HI / HI | BG | 只用 HI、MID，睡眠变暗不引入新颜色；两侧同色和 CX 一样会报 face on body |
| 街机 | WHITE / SILVER / GREY | INDIGO | |
| 剧场 | CREAM / GOLD / PLANK | BACK | 只用调色板里的颜色，像一只道具箱 |

荧光终端单会话视图的命令名是 `cursor`。街机开场的角色队列原来按 6 个角色、每步 6 px 刚好 9.6 秒一圈，现在每步 `SKIN_COUNT` px，一圈仍是 9.6 秒，预览照样无缝循环。

预览：加了 `scenes/one-cu.ndjson`（同 `one-wb`），总览图的矩阵加了 `CU`。主题 1 原有 2791 帧不变，baseline 只追加了新场景；页面预览里只有街机的 empty（开场队列）变了。
