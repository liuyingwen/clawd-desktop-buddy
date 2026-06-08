# Windows + Linux 支持 — Design Spec

**Date**: 2026-06-08
**Status**: Draft (pending user review)
**Authors**: liuyingwen + Claude
**Depends on**: [2026-06-07-clawd-mood-design.md](./2026-06-07-clawd-mood-design.md), [2026-06-07-codex-support-design.md](./2026-06-07-codex-support-design.md)

## 1. 目标

让 clawd-mood 从 macOS-only 扩展到 **macOS + Linux + Windows 三平台**。固件不变，仅改 Mac 侧管道（hook + daemon + IPC）以消除 POSIX 假设。

完成标准：
- **macOS**：端到端完整回归通过（§9 全部 8 个回归用例）
- **Linux**：代码完整 + 文档完整，标注 `should work on Linux`（POSIX 路径与 macOS 高度相似，回归风险低），等待社区验证
- **Windows**：代码完整 + 文档完整，标注 `untested on Windows`，等待社区验证

## 2. 范围

**做什么**：
- 用 `hook.py` 替换 `hook.sh`（PEP 723 + uv，纯标准库，三平台同一份）
- 修改 `daemon.py`：串口枚举改 `pyserial.list_ports`，FIFO 改 TCP socket，新增 portfile 服务发现，跨平台后台启动与信号处理
- 修改 `hooks.json` / `hooks-codex.json` 的 `command` 字段指向 `uv run … /hook.py`
- 删除 `hook.sh`
- README 重组成"平台共通段 + 三平台并列子段"
- 更新 CLAUDE.md（删 macOS-only 限制、改 IPC 描述、补 portfile 段）
- 新增 0.2.0 breaking 公告

**不做什么**：
- 不改固件（USB CDC 协议本身跨平台）
- 不改三段式架构（仍是 hook → IPC → daemon → 串口 → ESP32 单向）
- 不引入 launchd / systemd / Windows Service 系统级自启（仍只做插件级 SessionStart 自启）
- 不做自动化测试（继承现状策略）
- 不在 Windows 上端到端测试（无环境）
- 不维护 macOS 向后兼容的 hook.sh（A 方案 = 完全替换）
- 不引入 `psutil` 等第三方进程检测库（portfile + connect 探活替代）

## 3. 整体架构

三段式结构不变，仅替换 IPC 与 hook 实现：

```
┌─────────────────────────────────────────┐
│ Claude Code / Codex CLI                 │
│         event                           │
└──────────────┬──────────────────────────┘
               │
               ▼
   hook.py（uv run, 标准库 only）
   - JSON 解析事件 → 12 项 state 映射表
   - SessionStart 时若 daemon 不在 → spawn detached
   - 读 portfile → TCP 短连接 send NDJSON
               │
               ▼
   TCP 127.0.0.1:<port>   ← portfile: <tempdir>/clawd-mood.port
               │
               ▼
   daemon.py（uv run, pyserial）
   - bind preferred 48756；冲突回退 bind(0)
   - 写 portfile（实际端口）
   - accept 循环串行消费 NDJSON
   - 写串口（独占）
               │ USB CDC NDJSON 115200
               ▼
   firmware/clawd_mood/*.ino   （完全不变）
```

### 并发与单例

- **TCP 短连接多写者单读者**：与旧 FIFO 语义等价。两个 CLI 的 hook 进程各自 connect → send → close；daemon 单线程 accept 串行消费
- **daemon singleton**：portfile 探活替代 `pgrep -f`。启动时读 portfile，try `connect()` 那个端口（100ms 超时），能连上就退出
- **串口独占**：仍由 daemon 独占
- **表情序列**：照旧不做合并/去重

## 4. 文件结构（diff）

```
clawd-mood/
├── plugin/
│   ├── hooks/
│   │   ├── hooks.json                   (修改：command 字段)
│   │   └── hooks-codex.json             (修改：command 字段)
│   └── scripts/
│       ├── hook.sh                      ← 删除
│       ├── hook.py                      ← 新增
│       └── daemon.py                    (修改：见 §5.2)
├── docs/superpowers/
│   ├── specs/
│   │   └── 2026-06-08-windows-linux-support-design.md   ← 本文档
│   └── plans/
│       └── 2026-06-08-windows-linux-support-implementation.md   ← 下一阶段产物
├── README.md                            (重组)
├── CLAUDE.md                            (修改：见 §6)
└── AGENTS.md                            (无需大改，指回 CLAUDE.md 即可)

firmware/clawd_mood/clawd_mood.ino       (完全不变)
```

## 5. 关键组件设计

### 5.1 hook.py

**头部 / 依赖**：

```python
#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
```

无第三方依赖。uv 在无依赖时启动开销约 80–200ms（cached），可接受。

**事件 → state 映射**（集中成一个常量 dict）：

| 事件 | state | 来源 CLI |
|---|---|---|
| `SessionStart` | `idle` | both |
| `UserPromptSubmit` | `thinking` | both |
| `PreToolUse` / `PostToolUse` | `working` | both |
| `PostToolUseFailure` | `error` | Claude Code only |
| `Notification` | `waiting` | Claude Code only |
| `Stop` | `done` | both |
| `SubagentStart` / `SubagentStop` | `working` | both |
| `PermissionRequest` | `waiting` | Codex only |
| `PreCompact` | `thinking` | Codex only |
| `PostCompact` | `working` | Codex only |

未知事件 → 静默退出（与旧 `*) exit 0 ;;` 行为一致）。

**主流程**：

```python
def main():
    payload = json.loads(sys.stdin.read())
    event = payload.get("hook_event_name", "")
    tool  = payload.get("tool_name", "")
    state = EVENT_TO_STATE.get(event)
    if state is None:
        return

    if event == "SessionStart":
        ensure_daemon_running()

    port = read_portfile()
    if port is None:
        return  # daemon 还没起，下次事件再补
    line = json.dumps({"state": state, "event": event, "tool": tool}) + "\n"
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.3) as s:
            s.sendall(line.encode())
    except (OSError, socket.timeout):
        pass  # daemon 死了或拒连，静默退出
```

**跨平台 spawn daemon**：

```python
def ensure_daemon_running():
    if read_portfile() and probe_daemon():
        return
    daemon_path = Path(__file__).parent / "daemon.py"
    log = Path(tempfile.gettempdir()) / "clawd-mood-daemon.log"
    log_fh = open(log, "a")

    if sys.platform == "win32":
        CREATE_NO_WINDOW = 0x08000000
        DETACHED_PROCESS = 0x00000008
        subprocess.Popen(
            ["uv", "run", str(daemon_path)],
            stdout=log_fh, stderr=log_fh, stdin=subprocess.DEVNULL,
            creationflags=DETACHED_PROCESS | CREATE_NO_WINDOW,
            close_fds=True,
        )
    else:
        subprocess.Popen(
            [str(daemon_path)],
            stdout=log_fh, stderr=log_fh, stdin=subprocess.DEVNULL,
            start_new_session=True,
            close_fds=True,
        )
```

**关键设计点**：
- `timeout=0.3` 秒——hook 不阻塞 Claude Code / Codex
- 第一次 SessionStart 仍可能丢消息（daemon 冷启约 2-3 秒，portfile 还没出现）—— UserPromptSubmit 补，与现状一致
- Windows 上必须显式 `uv run daemon.py`（shebang 不工作），`CREATE_NO_WINDOW | DETACHED_PROCESS` 防止闪 cmd 窗
- Mac/Linux 上 `start_new_session=True` 替代 `nohup ... & disown`，标准库原生

### 5.2 daemon.py

**改动一览**：

| 现状 | 改成 |
|---|---|
| `glob("/dev/cu.usbmodem*")` | `serial.tools.list_ports.comports()` + 平台路径前缀过滤 |
| `os.mkfifo(FIFO_PATH)` + `open(fifo, "r")` for loop | `socket(AF_INET, SOCK_STREAM).listen()` + `accept()` 循环 |
| 进程退出 `os.unlink(FIFO_PATH)` | `atexit` 清 portfile（best-effort） |
| `signal.signal(SIGINT/SIGTERM, cleanup)` | 保留 SIGINT；SIGTERM 在 Windows 上不可用，try/except 静默 |

**串口枚举**：

```python
def detect_port() -> str:
    override = os.environ.get("CLAWD_MOOD_PORT")
    if override:
        return override
    candidates = []
    for p in list_ports.comports():
        if sys.platform == "darwin" and p.device.startswith("/dev/cu.usbmodem"):
            candidates.append(p.device)
        elif sys.platform == "linux" and (
            p.device.startswith("/dev/ttyACM") or p.device.startswith("/dev/ttyUSB")
        ):
            candidates.append(p.device)
        elif sys.platform == "win32" and p.device.upper().startswith("COM") and p.vid is not None:
            candidates.append(p.device)
    candidates.sort()
    if not candidates:
        sys.exit("No ESP32-like USB CDC device found. Plug in or set CLAWD_MOOD_PORT.")
    if len(candidates) > 1:
        print(f"  warning: multiple devices {candidates}, using {candidates[0]}", file=sys.stderr)
    return candidates[0]
```

Linux 同时扫 `ttyACM`（ESP32-C3 原生 USB CDC）+ `ttyUSB`（CH340/CP2102 桥）。
Windows 过滤 `p.vid is not None`，避开蓝牙串口、调制解调器虚拟 COM。

**TCP server + portfile**：

```python
def bind_listen(preferred, env_forced):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", preferred))
    except OSError:
        if env_forced:
            raise  # 显式 env 覆盖时冲突，let user know
        s.bind(("127.0.0.1", 0))  # OS 自动分配
    s.listen(8)
    return s, s.getsockname()[1]


def main():
    install_signal_handlers()
    port_path = Path(tempfile.gettempdir()) / "clawd-mood.port"
    forced = os.environ.get("CLAWD_MOOD_PORT_TCP")
    preferred = int(forced) if forced else 48756

    # Singleton check
    if port_path.exists():
        try:
            with socket.create_connection(
                ("127.0.0.1", int(port_path.read_text().strip())), timeout=0.1
            ):
                sys.exit("Another daemon is already running.")
        except (OSError, ValueError):
            pass  # 脏 portfile，接管

    server, actual_port = bind_listen(preferred, forced is not None)
    port_path.write_text(str(actual_port))
    atexit.register(lambda: port_path.unlink(missing_ok=True))

    serial_port = detect_port()
    ser = open_serial(serial_port)
    print(f"clawd-mood daemon — listening 127.0.0.1:{actual_port}, serial {serial_port}")

    while True:
        conn, _ = server.accept()
        with conn:
            data = conn.recv(4096)
            for line in data.decode("utf-8", errors="replace").splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    json.loads(line)
                except json.JSONDecodeError:
                    print(f"  !! bad JSON: {line}", file=sys.stderr)
                    continue
                try:
                    ser.write((line + "\n").encode())
                    ser.flush()
                    print(f"  -> {line}")
                except serial.SerialException as e:
                    print(f"  !! serial error: {e}", file=sys.stderr)
                    sys.exit(1)
```

**信号处理跨平台**：

```python
def install_signal_handlers():
    signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
    try:
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    except (AttributeError, ValueError):
        pass  # Windows 没有真正的 SIGTERM
```

Windows 上 `Ctrl+C` 仍触发 SIGINT；`atexit` 兜底清 portfile。`taskkill /F` 会留下脏 portfile，但下次启动自动覆盖。

### 5.3 hooks.json / hooks-codex.json

`command` 字段从：

```json
"command": "\"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.sh"
```

改成：

```json
"command": "uv run \"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.py"
```

三平台一致。不再依赖 shebang、不依赖 `.py` Windows 文件关联。

## 6. 文档改动

### 6.1 CLAUDE.md

| 段落 | 改动 |
|---|---|
| 「项目概要」 | 「仅支持 macOS。明确不做：…Windows/Linux 兼容…」→ 「跨平台桌面摆件（macOS / Linux / Windows）。明确不做：WiFi 控制、OTA、launchd/systemd 系统级自启、自动化单元测试、蜂鸣器、多设备。Windows 端代码完成但 **untested**。」 |
| 「三段式架构」图 | `/tmp/clawd-mood.fifo` → `TCP 127.0.0.1:<port> (portfile 服务发现)`；`hook.sh (Bash + jq)` → `hook.py (uv run, 标准库)` |
| 关键约束 #1 | 删「macOS 没有 timeout 命令」（不再相关） |
| 关键约束 #6 | 「多 USB CDC 设备」补 Windows COM 口、Linux ttyACM 示例 |
| 关键约束 #7 | 「插件级 daemon 自启」改用 portfile + connect 探活；`nohup` → `subprocess.Popen(start_new_session=True)` 或 Windows `DETACHED_PROCESS` |
| 新增约束 #8 | 「portfile 服务发现」——位置、被占用回退、脏 portfile 自愈 |
| 「常用命令 / 烧固件」 | 拆三平台示例（COM / ttyACM / cu.usbmodem） |
| 「手动测试」 | 替换 `echo … > FIFO`：Mac/Linux 用 `printf '{"state":"working"}\n' \| nc 127.0.0.1 $(cat /tmp/clawd-mood.port)`；Windows PowerShell 用 `$p = Get-Content $env:TEMP\clawd-mood.port; (New-Object Net.Sockets.TcpClient '127.0.0.1', $p).GetStream().Write([Text.Encoding]::UTF8.GetBytes('{"state":"working"}`n'), 0, 21)`（实施阶段在 Windows 真机验证并微调） |
| 「仓库结构要点」 | 删 hook.sh 行，加 hook.py 行 |

### 6.2 README.md

重组为「平台共通段 + 三平台并列子段」：

```
## 准备
### macOS    brew install arduino-cli uv
### Linux    apt/pacman/dnf install arduino-cli uv + dialout 组提示
### Windows  winget install arduino-cli astral-sh.uv  (⚠️ untested 横幅)

## 烧固件
### macOS    -p /dev/cu.usbmodem101
### Linux    -p /dev/ttyACM0
### Windows  -p COM3

## 挂插件
### Claude Code    三平台命令相同
### Codex CLI      三平台命令相同

## 卸载
（三平台分别给出删 portfile + kill daemon 命令）
```

**Windows 段顶部横幅**：

> ⚠️ **Untested on Windows.** Code paths are written and reviewed but not end-to-end verified on real Windows + ESP32 hardware. If you try it, please open an issue with your result.

### 6.3 兼容性公告（0.2.0）

README 顶部 + commit message 中声明：

- `/tmp/clawd-mood.fifo` 不再使用，改 TCP `127.0.0.1:<port>`（端口写在 `<tempdir>/clawd-mood.port`）
- `plugin/scripts/hook.sh` 删除，统一 `hook.py`
- 仍是单一 daemon、单 USB 串口、无 WiFi/OTA
- 升级方式：Claude Code 用户重新 `claude --plugin-dir`（实时）；Codex 用户 `codex plugin remove/add`（拷贝缓存）

## 7. 依赖矩阵（新旧对比）

| 组件 | 旧（macOS） | 新（三平台） |
|---|---|---|
| hook | `bash` + `jq` | `uv`（标准库 only，无包安装） |
| daemon | `uv` + `pyserial`（PEP 723） | 同 |
| 串口枚举 | shell `glob` | `pyserial.tools.list_ports` |
| 进程检测 | `pgrep -f` | portfile + `connect()` 探活（无系统依赖） |
| 后台启动 | `nohup ... & disown` | `subprocess.Popen(start_new_session=True)` / Windows `DETACHED_PROCESS` |
| 日志位置 | `/tmp/clawd-mood-daemon.log` | `<tempfile.gettempdir()>/clawd-mood-daemon.log` |

## 8. 错误处理

| 场景 | 行为 |
|---|---|
| daemon 没起，hook 触发 | `read_portfile()` 返回 None，静默退出。不阻塞 CLI |
| portfile 存在但 daemon 已死 | hook 端 `connect()` 抛 OSError，静默退出。下次 SessionStart 自启 |
| daemon 启动时 portfile 已存在且能 connect 通 | "Another daemon is already running" 退出（singleton） |
| daemon 启动时 portfile 是脏的 | 当前进程接管，覆盖 portfile |
| 默认端口 48756 被占 | daemon 自动 `bind(0)` 让 OS 分配，写新端口到 portfile |
| `CLAWD_MOOD_PORT_TCP=N` 显式指定且冲突 | daemon 直接报错退出（用户既然显式指定，应让其知晓） |
| 未识别 ESP32 设备 | daemon 启动失败、报错退出 |
| 串口被占（permission denied on Linux） | daemon 抛 SerialException，建议 README 给 `usermod -aG dialout` 提示 |
| daemon 收到 bad JSON | 打 stderr 警告 + 跳过这条，不退出 |
| daemon 写串口失败 | 退出（让 SessionStart 重启） |

## 9. 测试策略

| 平台 | 验证方式 | 覆盖深度 |
|---|---|---|
| macOS | 本机端到端：插 ESP32 → 跑真实 Claude Code 任务 → 看 7 表情序列 | 完整回归 |
| Linux | **不端到端跑**（无 Linux 机）。POSIX 路径与 macOS 高度相似（同样 `start_new_session=True` / `tempfile.gettempdir()` / pyserial），macOS 通过即视为 Linux 大概率通过。代码 review 重点看 `sys.platform == "linux"` 分支与 ttyACM/ttyUSB 扫描 | 仅代码层 |
| Windows | **不端到端跑**。代码 review、grep 关键平台调用（`creationflags=`、`list_ports`、`tempfile.gettempdir()`、`sys.platform == "win32"`）确认无 macOS-isms 残留。PR 模板请求社区验证 | 仅代码层 |

**回归用例**（macOS 端到端必跑）：

1. 冷启动：删 portfile + kill 已有 daemon → 跑 Claude Code 任务 → 第一条事件可能丢、第二条起表情正常切换
2. 端口被占：手动 `nc -l 48756` 占住默认端口 → 启 daemon → 应自动回退到 OS 分配端口 → portfile 反映实际端口 → hook 仍能连通
3. 显式端口冲突：`CLAWD_MOOD_PORT_TCP=48756` 启 daemon 时 48756 被占 → 应报错退出
4. Singleton：连开两个 daemon → 第二个应检测到 portfile 探活成功并退出
5. 脏 portfile：手动 `kill -9` 杀 daemon → portfile 还在 → 重启 daemon → 应能接管
6. 双 CLI 并发：Claude Code + Codex 同时跑任务 → 表情按事件到达顺序交错
7. Done → Idle 3 秒回退（固件层不变，验证不受 IPC 改动影响）
8. Sleeping → 5 分钟无消息（同上）

## 10. 风险与未决事项

| 项 | 评估 |
|---|---|
| Windows 上 `uv run` 在 Claude Code/Codex hook spawn 时能否找到 PATH | 高概率 OK（uv 在 Windows first-class）。实施阶段实测验证 |
| Windows 上 `"$CLAUDE_PLUGIN_ROOT"` 变量展开机制 | Claude Code/Codex 应自行处理跨平台变量展开。若不支持，回退到 hook.py 内部用 `__file__` 自定位 daemon.py（已经如此设计） |
| Linux 上 `start_new_session=True` 与 systemd-logind 的交互 | 用户登出可能杀掉脱离会话进程（depends on `KillUserProcesses` 配置）。文档提示 |
| portfile 多用户共享 tempdir 冲突（如多用户同时登录） | 罕见场景，0.2.0 不处理。若需要可改成 `<tempdir>/clawd-mood-<uid>.port` |
| 端口 48756 是否常被占用 | 该端口不属于知名服务，冲突极少。回退机制覆盖罕见情况 |

## 11. 时间线（设计阶段）

- 2026-06-08：本 spec 落盘 + 用户审稿
- 待 spec 审稿通过后：进 writing-plans，写实施计划
- 实施计划批准后：进 executing-plans 实施
