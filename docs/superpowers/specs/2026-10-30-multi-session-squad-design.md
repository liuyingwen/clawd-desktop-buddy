# 多会话屏幕：桌面小队

目标：一块 240×240 屏同时显示多个 CLI 会话，每个会话都能看到项目名和状态。取代原来的单会话全屏眼睛表情。

## 视觉

每个会话是一只固定位置的像素小生物（造型按 `src` 取自对应工具的 logo，未知来源用 Clawd 造型 + 轮换颜色），脚下地砖颜色和右上角图标表示状态，最下方写会话标题（没有时写目录名），左上角写来源（CC / CX）。

| 状态 | 地砖色 / 图标 | 动作 |
|---|---|---|
| idle | 灰 / 无 | 左右轻移，偶尔眨眼 |
| thinking | 蓝 / 转动齿轮 | 眼睛左右看 |
| working | 绿 / 键盘 | 手臂交替敲击 |
| waiting | 黄 / 问号 | 原地跳动，格子黄底闪烁 |
| done | 白 / 对勾 | 跳跃撒彩屑，3 秒后回 idle |
| error | 红 / 叉号 | X 眼、冒烟、红色内框 |
| sleeping | 深灰 / Z | 压扁，闭眼，颜色变暗 |

## 布局与翻页

- 每页最多 4 个并撑满：1 全屏；2 左右拆分；3 左大右二；4 为 2×2。
- 超过 4 个时每 5 秒自动翻页（硬件无按键），底部 4 像素页码条。滑动 6 帧、共 300ms。
- 别的页有会话进入 waiting / error：立即翻过去，停留到解除或最多 10 秒，页码块闪状态色。带 `quiet` 的消息只显示不翻页（目前上游不发送）；单次工具失败显示为 working，不算 error。
- 位置稳定：新会话追加，状态变化不换位。

## 取舍

- **会话表放固件，daemon 不变**：daemon 继续只转发，重启不丢状态语义；固件重启后由下一条事件恢复。
- **整屏帧缓冲**：`GFXcanvas16` 占 115 KB，ESP32-C3 可用内存足够，换来多格无闪烁、翻页实现简单。
- **Waiting 不进入 sleeping**：否则离开 5 分钟后待批准会被隐藏。
- **Working 15 分钟才进入 sleeping**：长工具调用期间没有事件，5 分钟会把正在跑的构建显示成睡着。
- **30 分钟过期**：CLI 崩溃不会发 SessionEnd。
- **标签优先显示会话标题**：读 CLI 内部数据，读不到时退回目录名；固件保留最近的标题，避免偶发读取失败造成跳变。
- **中文字体**：wqy 12 像素 GB2312，约占 208 KB 闪存；分屏单元格一行约放 9 个汉字。

## 子代理：头顶小跟班

子代理不占格子，也不改主小人状态（等待批准除外）。hook 用 `agent_id` 识别，串口消息多带 `agent` 字段，`SubagentStop` 再带 `gone`。

- 位置：主小人头顶一排居中，按出现先后排列，放不下显示 `+N`。选它而不是两侧或脚边：不用缩小主小人，不挡身体，各种布局表现一致。
- 造型：主小人的缩小版，12×9px，单会话全屏时翻倍。

| 时机 | 动效 |
|---|---|
| 出现（`SubagentStart`，thinking） | 从上方落下 400ms，眼睛左右看 |
| 工作（working） | 跳动、交替敲键盘，每只错开 110ms 形成波浪 |
| 等待批准（waiting） | 全身变黄、举手高跳；主小人同时显示 waiting 并翻页 |
| 结束（`SubagentStop`） | `^` 眼跳跃撒彩屑 1.5 秒后消失，后面的依次补位 |

没收到 `SubagentStop` 时，15 分钟无事件后移除。工具失败仍显示 working。

各 agent 的子代理来源：

| agent | 识别方式 | 结束信号 |
|---|---|---|
| Claude Code / WorkBuddy / Codex | hook 的 `agent_id` + `agent_type`，`session_id` 是父会话 | `SubagentStop` |
| OpenCode | `session.created` 带 `parentID` 的子会话 | `session.execution.*` 结束或 `session.deleted` |
| pi | 环境变量 `CLAWD_MOOD_PI_OWNER` 不是自己的子进程 | `agent_settled` / `session_shutdown` |
| DeepSeek Harness | `header.origin === "subagent"`，嵌套的归到顶层 | `turn-stopping` / `agent/error` / `agent/disposed` |
