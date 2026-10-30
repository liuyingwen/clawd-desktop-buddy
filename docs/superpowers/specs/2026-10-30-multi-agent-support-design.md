# 多 agent 支持设计（OpenCode / pi / WorkBuddy / DeepSeek Harness）

## 目标

在 Claude Code、Codex CLI 之外，让摆件也能显示 OpenCode、pi、WorkBuddy、DeepSeek Harness 的状态。daemon、串口协议和固件都不改。

> 后续：多会话桌面小队（`2026-10-30-multi-session-squad-design.md`）给串口协议加了会话字段。适配器按进程发 `session`，`src` 分别为 `OC`、`PI`；DeepSeek Harness 的 hook 传 `dsh`，显示 `DS`；WorkBuddy 共用 hooks.json，显示 `CC`。

## 接入方式

按 agent 提供的扩展机制分两类：

| 类别 | agent | 进程模型 | 状态映射位置 |
| ---- | ----- | -------- | ------------ |
| 命令式 hook | Claude Code、Codex、WorkBuddy、DeepSeek Harness | 每个事件起一个进程 | `hook.py` 的 `EVENT_TO_STATE` |
| 进程内扩展 | OpenCode、pi | 事件在 agent 进程内回调 | 适配器内，直接走 TCP |

数据流：

```
WorkBuddy / DeepSeek Harness ─ hook ─→ hook.py ─┐
                                                ├─ TCP → daemon → 串口
OpenCode / pi ─ 进程内适配器 ───────────────────┘
                  └─ 仅启动时调 hook.py 拉起 daemon
```

## 关键决策

**进程内适配器直接写 TCP，不经 hook.py。** 最初的实现是把事件翻译成 hook 事件名，再串行 spawn `uv run hook.py`。实测 `opencode run` 的服务进程在回合结束约 8ms 后退出，而一次 uv 启动约 100ms，最后的 `done` / `error` 全部丢失，屏幕停在 `working`。直接写 socket 约 1ms，还保证了顺序。代价是适配器里多一份映射，但都收敛到同一个 state 枚举。

**适配器拉起 daemon 时用 `source: compact`。** 这让 hook.py 只探活不推状态。否则 uv 的启动延迟会让 `idle` 晚于第一个真实状态到达，把它盖掉。

**命令式 hook 必须以 `|| true` 结尾。** `uv` 自身出错返回退出码 2，而同步 hook 的 2 表示拦截。这在 DeepSeek Harness 上实际发生过：沙箱禁止 uv 初始化缓存，结果每条 prompt 都被 `blocked`。Windows cmd 没有 `true`，但此时退出码是 1，同样不拦截。

**DeepSeek Harness 走官方桥接，不写原生插件。** 原生 Cordis 插件能拿到审批等更多事件，但要发 npm 包并跟随 rc 版本的内部 API。桥接的代价是只有 7 个事件。

**OpenCode 只支持 2.0 及以上。** V1 插件接口在 2.0 不运行，本机也只有 2.0 可测，不保留无法验证的 V1 代码。

## 事件映射

OpenCode（子会话的 execution 事件被过滤，靠 `session.created` 的 `parentID` 识别）：

| 事件 | state |
| ---- | ----- |
| 插件加载 | idle |
| `session.execution.started` | thinking |
| `session.tool.called` / `session.tool.success` | working |
| `session.tool.failed` | error |
| `permission.asked` / `form.created` | waiting |
| `permission.replied` / `form.replied` / `form.cancelled` | working |
| `session.compaction.started` | thinking |
| `session.compaction.ended` / `session.compaction.failed` | 回合中 working，否则 idle |
| `session.execution.succeeded` | done |
| `session.execution.failed` | error |
| `session.execution.interrupted` | idle |

pi：

| 事件 | state |
| ---- | ----- |
| `session_start`（`reload` 除外） | idle |
| `before_agent_start` | thinking |
| `tool_execution_start` | working |
| `tool_execution_end` | `isError` 时 error，否则 working |
| `ui_prompt_start` / `ui_prompt_end`（回合中） | waiting / working |
| `session_before_compact` | thinking |
| `session_compact` / `session_compact_failed` | 回合中 working，否则 idle |
| `agent_settled` | `aborted` 时 idle，最后一条消息 `stopReason: error` 时 error，否则 done |
| `session_shutdown`（`quit`） | idle |

## 已知限制

- DeepSeek Harness：没有 waiting、error、压缩状态；不能自动拉起 daemon（沙箱内 uv 不可用）。
- pi：本身没有工具审批，waiting 只在扩展弹窗时出现。
- OpenCode 的工具事件不带工具名，`tool` 字段为空（仅调试用）。

## 验证

用真实 CLI 对接本地 mock 模型（OpenAI 兼容，支持工具调用和 400 报错）和假 daemon，通过隔离的 `TMPDIR` 避开真实串口：

| agent | 版本 | 已验证 | 未验证 |
| ----- | ---- | ------ | ------ |
| OpenCode | 2.0.32 | 工具、权限、子会话、报错、自动拉起 daemon | 交互式 TUI、手动 compact、中断、question 表单 |
| pi | 1.4.0 | 工具、报错、`pi install`、自动拉起 daemon | 交互式 TUI、compact、中断、弹窗 |
| CodeBuddy 引擎 | 2.137.1 | marketplace 安装；未登录下 SessionStart / UserPromptSubmit / Stop | 登录后的工具事件、WorkBuddy 桌面版 |
| DeepSeek Harness | 0.2.1-rc.2 | 桥接 + 工具全流程（headless） | 桌面版、web 版 |

全部未接真实硬件。Linux、Windows 未验证。
