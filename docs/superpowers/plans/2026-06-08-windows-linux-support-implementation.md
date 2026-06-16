# Windows + Linux Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 clawd-mood 从 macOS-only 扩展到 macOS / Linux / Windows 三平台。固件不变，重写 Mac 侧管道（hook + daemon + IPC）消除 POSIX 假设。

**Architecture:** `/tmp/clawd-mood.fifo` 替换为 TCP `127.0.0.1:<port>` + portfile 服务发现；`hook.sh` (bash+jq) 替换为 `hook.py` (uv run, stdlib only)；串口 glob 替换为 `pyserial.list_ports`；进程检测/启动用 portfile probe 和 `subprocess.Popen` 跨平台替代 `pgrep`/`nohup`。

**Tech Stack:** Python 3.12+ via `uv`, `pyserial`, Python stdlib (`socket`, `subprocess`, `tempfile`, `atexit`, `signal`).

**Spec:** [2026-06-08-windows-linux-support-design.md](../specs/2026-06-08-windows-linux-support-design.md)

**Testing strategy (per CLAUDE.md)：项目无自动化测试**。每个 task 用 `write code → manual smoke check → verify expected output → commit` 模板。macOS 端到端集成测试集中在 Task 6。Linux / Windows 仅代码 review + 文档完整，标注 untested。

**Branch:** `feature/windows-linux-support`（已建并 commit spec）。所有 task commit 都进这个分支。

---

## File Structure

| 文件 | 操作 | 责任 |
|---|---|---|
| `plugin/scripts/daemon.py` | **Rewrite** | 跨平台串口枚举 / TCP server / portfile / 跨平台信号 |
| `plugin/scripts/hook.py` | **Create** | 事件→state 映射 / portfile 读 / TCP 短连接 / 跨平台 spawn daemon |
| `plugin/scripts/hook.sh` | **Delete** | bash 版完全替换 |
| `plugin/hooks/hooks.json` | **Modify** | `command` 字段：`hook.sh` → `uv run … hook.py` |
| `plugin/hooks/hooks-codex.json` | **Modify** | 同上 |
| `CLAUDE.md` | **Modify** | 8 段：项目概要 / 架构图 / 关键约束 / 烧固件 / 手测命令 / 仓库结构 |
| `README.md` | **Restructure** | 三平台并列段 + Windows untested 横幅 + 0.2.0 breaking 公告 |

---

## Task 1: 重写 daemon.py（跨平台 IPC + 串口 + 信号）

**Files:**
- Modify (full rewrite): `plugin/scripts/daemon.py`

**Why this is one atomic task：** daemon.py 改动量大但耦合紧——串口枚举、TCP server、portfile、信号处理必须一起到位才能让 daemon 启动。拆开会出现"半工作"状态。

- [ ] **Step 1: Rewrite `plugin/scripts/daemon.py`** with full cross-platform implementation

```python
#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["pyserial>=3.5"]
# ///
"""clawd-mood daemon — TCP server forwarding state to ESP32 via USB CDC.

Replaces the POSIX FIFO (/tmp/clawd-mood.fifo) used in 0.1.x with a TCP
localhost socket. The actual listening port is written to a portfile at
<tempdir>/clawd-mood.port so the hook can discover it. Cross-platform:
macOS / Linux / Windows.
"""

import atexit
import json
import os
import signal
import socket
import sys
import tempfile
import time
from pathlib import Path

import serial
from serial.tools import list_ports

BAUD_RATE = 115200
DEFAULT_TCP_PORT = 48756
PORTFILE = Path(tempfile.gettempdir()) / "clawd-mood.port"


def detect_port() -> str:
    override = os.environ.get("CLAWD_MOOD_PORT")
    if override:
        return override
    candidates: list[str] = []
    for p in list_ports.comports():
        if sys.platform == "darwin" and p.device.startswith("/dev/cu.usbmodem"):
            candidates.append(p.device)
        elif sys.platform == "linux" and (
            p.device.startswith("/dev/ttyACM") or p.device.startswith("/dev/ttyUSB")
        ):
            candidates.append(p.device)
        elif (
            sys.platform == "win32"
            and p.device.upper().startswith("COM")
            and p.vid is not None
        ):
            candidates.append(p.device)
    candidates.sort()
    if not candidates:
        sys.exit(
            "No ESP32-like USB CDC device found. "
            "Plug it in or set CLAWD_MOOD_PORT=/dev/cu.xxx (or COM3 on Windows)."
        )
    if len(candidates) > 1:
        print(
            f"  warning: multiple devices {candidates}, using {candidates[0]}",
            file=sys.stderr,
        )
    return candidates[0]


def open_serial(port: str) -> serial.Serial:
    ser = serial.Serial(
        port, BAUD_RATE, timeout=1, dsrdtr=False, rtscts=False,
    )
    ser.dtr = False
    ser.rts = False
    time.sleep(0.5)
    ser.read(1000)
    time.sleep(1.0)
    ser.read(1000)
    return ser


def bind_listen(preferred: int, env_forced: bool) -> tuple[socket.socket, int]:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", preferred))
    except OSError:
        if env_forced:
            raise
        s.bind(("127.0.0.1", 0))
    s.listen(8)
    return s, s.getsockname()[1]


def install_signal_handlers() -> None:
    signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
    try:
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    except (AttributeError, ValueError):
        pass  # Windows lacks a real SIGTERM


def check_singleton() -> None:
    if not PORTFILE.exists():
        return
    try:
        port = int(PORTFILE.read_text().strip())
    except (OSError, ValueError):
        return
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
            sys.exit("Another clawd-mood daemon is already running.")
    except OSError:
        pass  # stale portfile — take over


def main() -> None:
    install_signal_handlers()
    check_singleton()

    forced = os.environ.get("CLAWD_MOOD_PORT_TCP")
    preferred = int(forced) if forced else DEFAULT_TCP_PORT
    server, actual_port = bind_listen(preferred, forced is not None)
    PORTFILE.write_text(str(actual_port))
    atexit.register(lambda: PORTFILE.unlink(missing_ok=True))

    serial_port = detect_port()
    ser = open_serial(serial_port)
    print(f"clawd-mood daemon started")
    print(f"  TCP:    127.0.0.1:{actual_port}")
    print(f"  Serial: {serial_port}")
    print(f"  Ready!")

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


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Verify executable bit preserved**

```bash
chmod +x plugin/scripts/daemon.py
ls -l plugin/scripts/daemon.py
```

Expected: `-rwxr-xr-x ... plugin/scripts/daemon.py`

- [ ] **Step 3: Smoke check — daemon starts (with ESP32 connected)**

```bash
# Kill any stale daemon and portfile first
pkill -f scripts/daemon.py 2>/dev/null; rm -f /tmp/clawd-mood.port
./plugin/scripts/daemon.py
```

Expected stdout within ~3 seconds (first run installs pyserial via uv):
```
clawd-mood daemon started
  TCP:    127.0.0.1:48756
  Serial: /dev/cu.usbmodem101    (or similar)
  Ready!
```

And in another terminal:
```bash
cat /tmp/clawd-mood.port
```
Expected: `48756`

Press `Ctrl+C` in the daemon terminal to exit. Verify `/tmp/clawd-mood.port` is removed:
```bash
ls /tmp/clawd-mood.port 2>&1
```
Expected: `No such file or directory`

- [ ] **Step 4: Smoke check — TCP send forwards to serial**

Start the daemon again (`./plugin/scripts/daemon.py`). In another terminal:

```bash
printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat /tmp/clawd-mood.port)
```

Expected:
- Daemon stdout shows: `  -> {"state":"working"}`
- ESP32 displays the working eye animation

Try a bad JSON to verify error handling:
```bash
printf 'not json\n' | nc 127.0.0.1 $(cat /tmp/clawd-mood.port)
```

Expected daemon stderr: `  !! bad JSON: not json`
Daemon stays alive (no exit).

- [ ] **Step 5: Smoke check — singleton via portfile probe**

With one daemon running, start another:
```bash
./plugin/scripts/daemon.py
```

Expected: second daemon exits immediately with `Another clawd-mood daemon is already running.`

- [ ] **Step 6: Smoke check — port-conflict fallback**

Stop the daemon. Manually hold port 48756:
```bash
nc -l 48756 &
NC_PID=$!
./plugin/scripts/daemon.py
```

Expected: daemon picks a different port (printed in startup banner). Verify portfile contents differ from 48756:
```bash
cat /tmp/clawd-mood.port
```

Then clean up:
```bash
kill $NC_PID
```

- [ ] **Step 7: Commit**

```bash
git add plugin/scripts/daemon.py
git commit -m "$(cat <<'EOF'
feat(daemon): cross-platform IPC + serial + signal handling

- FIFO → TCP 127.0.0.1:<port> with portfile service discovery
- glob /dev/cu.usbmodem* → pyserial.tools.list_ports (mac/linux/win)
- pgrep singleton → portfile + connect() probe
- Port 48756 default with bind(0) fallback when occupied
- atexit cleans portfile; SIGTERM gracefully ignored on Windows
EOF
)"
```

---

## Task 2: 新增 hook.py（跨平台事件桥）

**Files:**
- Create: `plugin/scripts/hook.py`

- [ ] **Step 1: Create `plugin/scripts/hook.py`**

```python
#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""clawd-mood hook — maps CLI hook events to ESP32 states via TCP.

Receives hook JSON on stdin (from Claude Code / Codex CLI), looks up the
event in EVENT_TO_STATE, then opens a short TCP connection to the daemon
and sends a single NDJSON line. On SessionStart, spawns the daemon
detached if it is not already running. Pure stdlib (no third-party deps)
so uv run is fast (~80-200ms cached).
"""

import json
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

PORTFILE = Path(tempfile.gettempdir()) / "clawd-mood.port"
LOGFILE = Path(tempfile.gettempdir()) / "clawd-mood-daemon.log"

EVENT_TO_STATE = {
    "SessionStart":       "idle",
    "UserPromptSubmit":   "thinking",
    "PreToolUse":         "working",
    "PostToolUse":        "working",
    "PostToolUseFailure": "error",       # Claude Code only
    "Notification":       "waiting",     # Claude Code only
    "Stop":               "done",
    "SubagentStart":      "working",
    "SubagentStop":       "working",
    "PermissionRequest":  "waiting",     # Codex only
    "PreCompact":         "thinking",    # Codex only
    "PostCompact":        "working",     # Codex only
}


def read_portfile() -> int | None:
    try:
        return int(PORTFILE.read_text().strip())
    except (OSError, ValueError):
        return None


def probe_daemon() -> bool:
    port = read_portfile()
    if port is None:
        return False
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
            return True
    except OSError:
        return False


def ensure_daemon_running() -> None:
    if probe_daemon():
        return
    daemon_path = Path(__file__).parent / "daemon.py"
    log_fh = open(LOGFILE, "a")
    if sys.platform == "win32":
        CREATE_NO_WINDOW = 0x08000000
        DETACHED_PROCESS = 0x00000008
        subprocess.Popen(
            ["uv", "run", str(daemon_path)],
            stdout=log_fh,
            stderr=log_fh,
            stdin=subprocess.DEVNULL,
            creationflags=DETACHED_PROCESS | CREATE_NO_WINDOW,
            close_fds=True,
        )
    else:
        subprocess.Popen(
            [str(daemon_path)],
            stdout=log_fh,
            stderr=log_fh,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
            close_fds=True,
        )


def main() -> None:
    try:
        payload = json.loads(sys.stdin.read())
    except json.JSONDecodeError:
        return
    event = payload.get("hook_event_name", "")
    tool = payload.get("tool_name", "")
    state = EVENT_TO_STATE.get(event)
    if state is None:
        return

    if event == "SessionStart":
        ensure_daemon_running()

    port = read_portfile()
    if port is None:
        return  # daemon still warming up; next event will catch up
    line = json.dumps({"state": state, "event": event, "tool": tool}) + "\n"
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.3) as s:
            s.sendall(line.encode())
    except (OSError, socket.timeout):
        pass  # daemon died or rejected; silently drop


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Make executable**

```bash
chmod +x plugin/scripts/hook.py
ls -l plugin/scripts/hook.py
```

Expected: `-rwxr-xr-x ... plugin/scripts/hook.py`

- [ ] **Step 3: Smoke check — hook drives daemon end-to-end**

With daemon already running (from Task 1):

```bash
echo '{"hook_event_name":"PreToolUse","tool_name":"Bash"}' | ./plugin/scripts/hook.py
```

Expected:
- Daemon stdout shows: `  -> {"state": "working", "event": "PreToolUse", "tool": "Bash"}`
- ESP32 shows working eye animation

Try all 12 events to verify the mapping:

```bash
for evt in SessionStart UserPromptSubmit PreToolUse PostToolUse PostToolUseFailure Notification Stop SubagentStart SubagentStop PermissionRequest PreCompact PostCompact; do
  echo "{\"hook_event_name\":\"$evt\"}" | ./plugin/scripts/hook.py
  sleep 0.3
done
```

Expected: daemon logs 12 lines with correct state values. ESP32 cycles through expressions.

- [ ] **Step 4: Smoke check — hook is silent when daemon is down**

Kill daemon and remove portfile:
```bash
pkill -f scripts/daemon.py; rm -f /tmp/clawd-mood.port
echo '{"hook_event_name":"PreToolUse"}' | ./plugin/scripts/hook.py
echo "exit: $?"
```

Expected: no output, `exit: 0` (silently drops since no portfile).

- [ ] **Step 5: Smoke check — SessionStart auto-spawns daemon**

```bash
pkill -f scripts/daemon.py; rm -f /tmp/clawd-mood.port
echo '{"hook_event_name":"SessionStart"}' | ./plugin/scripts/hook.py
sleep 4   # let uv install pyserial on first run
cat /tmp/clawd-mood.port
pgrep -f scripts/daemon.py
```

Expected:
- portfile exists and contains a port number
- pgrep returns a PID (daemon detached + running)
- Daemon log at `/tmp/clawd-mood-daemon.log` shows startup banner

Note: the first SessionStart payload may be lost during the cold start window — this matches 0.1.x behavior.

- [ ] **Step 6: Commit**

```bash
git add plugin/scripts/hook.py
git commit -m "$(cat <<'EOF'
feat(hook): add cross-platform Python hook (replaces hook.sh)

- Pure stdlib (json/socket/subprocess/tempfile) — no third-party deps
- 12-event → state mapping (Claude Code + Codex CLI)
- TCP short connection with 300ms timeout (non-blocking on CLI)
- SessionStart auto-spawns daemon detached: start_new_session on POSIX,
  DETACHED_PROCESS | CREATE_NO_WINDOW on Windows
- Silent drop when portfile missing (cold-start tolerant)
EOF
)"
```

---

## Task 3: 切换 hooks.json command 字段

**Files:**
- Modify: `plugin/hooks/hooks.json` (9 occurrences of `command`)

- [ ] **Step 1: Replace all 9 `command` values**

```bash
sed -i.bak 's|"\$CLAUDE_PLUGIN_ROOT"/scripts/hook.sh|uv run "\$CLAUDE_PLUGIN_ROOT"/scripts/hook.py|g' plugin/hooks/hooks.json
rm plugin/hooks/hooks.json.bak
```

- [ ] **Step 2: Verify all 9 entries were replaced**

```bash
grep -c 'hook.py' plugin/hooks/hooks.json
grep -c 'hook.sh' plugin/hooks/hooks.json
```

Expected: `9` and `0` respectively.

- [ ] **Step 3: Verify JSON is still valid**

```bash
python3 -m json.tool plugin/hooks/hooks.json > /dev/null && echo "JSON OK"
```

Expected: `JSON OK`

- [ ] **Step 4: Inspect a sample entry to confirm the literal**

```bash
grep '"command"' plugin/hooks/hooks.json | head -1
```

Expected output (single line):
```
        { "type": "command", "command": "uv run \"$CLAUDE_PLUGIN_ROOT\"/scripts/hook.py", "async": true }
```

- [ ] **Step 5: Commit**

```bash
git add plugin/hooks/hooks.json
git commit -m "feat(hooks): point Claude Code hooks.json at uv run hook.py"
```

---

## Task 4: 切换 hooks-codex.json command 字段

**Files:**
- Modify: `plugin/hooks/hooks-codex.json` (10 occurrences of `command`)

- [ ] **Step 1: Replace all 10 `command` values**

```bash
sed -i.bak 's|"\$CLAUDE_PLUGIN_ROOT"/scripts/hook.sh|uv run "\$CLAUDE_PLUGIN_ROOT"/scripts/hook.py|g' plugin/hooks/hooks-codex.json
rm plugin/hooks/hooks-codex.json.bak
```

- [ ] **Step 2: Verify all 10 entries were replaced**

```bash
grep -c 'hook.py' plugin/hooks/hooks-codex.json
grep -c 'hook.sh' plugin/hooks/hooks-codex.json
```

Expected: `10` and `0` respectively.

- [ ] **Step 3: Verify JSON is still valid**

```bash
python3 -m json.tool plugin/hooks/hooks-codex.json > /dev/null && echo "JSON OK"
```

Expected: `JSON OK`

- [ ] **Step 4: Commit**

```bash
git add plugin/hooks/hooks-codex.json
git commit -m "feat(hooks): point Codex hooks-codex.json at uv run hook.py"
```

---

## Task 5: 删除旧的 hook.sh

**Files:**
- Delete: `plugin/scripts/hook.sh`

- [ ] **Step 1: Verify nothing else references hook.sh** (sanity check before deletion)

```bash
grep -rn 'hook\.sh' --include='*.json' --include='*.md' --include='*.py' --include='*.sh' .
```

Expected: matches only in `docs/superpowers/specs/2026-06-07-*.md`, `docs/superpowers/plans/2026-06-07-*.md`, and the new spec/plan (historical references — OK to leave as-is).

If `hooks.json` / `hooks-codex.json` / `README.md` / `CLAUDE.md` show matches → Tasks 3 / 4 were incomplete. Stop and fix before proceeding.

- [ ] **Step 2: Remove hook.sh**

```bash
git rm plugin/scripts/hook.sh
```

- [ ] **Step 3: Commit**

```bash
git commit -m "feat(hook): remove hook.sh — fully replaced by hook.py"
```

---

## Task 6: macOS 端到端集成回归（spec §9 全 8 用例）

**Files:** No file changes — pure verification on macOS hardware.

**Prerequisite:** ESP32 with current firmware connected via USB. Recent uv installed.

- [ ] **Step 1: Reinstall the plugin against the live path**

In a separate Claude Code session:
```bash
claude --plugin-dir /Users/liuyingwen/Documents/0.项目/0.aiwtf/2.aiwtf-claude-code-desktop/clawd-mood/plugin
```

Verify 9 events appear under `/hooks`.

For Codex:
```bash
cd /Users/liuyingwen/Documents/0.项目/0.aiwtf/2.aiwtf-claude-code-desktop/clawd-mood
codex plugin remove clawd-mood@clawd-mood 2>/dev/null
codex plugin add clawd-mood@clawd-mood
```

- [ ] **Step 2: Regression #1 — cold start**

```bash
pkill -f scripts/daemon.py 2>/dev/null
rm -f /tmp/clawd-mood.port
```

Start Claude Code, submit a trivial prompt ("ls"). Expected:
- First SessionStart may drop (cold start ~3s) — acceptable
- UserPromptSubmit → thinking eye
- PreToolUse / PostToolUse → working eye
- Stop → done eye → 3s later auto-revert to idle

- [ ] **Step 3: Regression #2 — port-conflict auto-fallback**

```bash
pkill -f scripts/daemon.py 2>/dev/null
nc -l 48756 &
NC_PID=$!
./plugin/scripts/daemon.py &
DAEMON_PID=$!
sleep 4
cat /tmp/clawd-mood.port
```

Expected: portfile contains a port ≠ 48756. Send a test:
```bash
printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat /tmp/clawd-mood.port)
```
Expected: ESP32 shows working eye. Cleanup:
```bash
kill $NC_PID $DAEMON_PID
```

- [ ] **Step 4: Regression #3 — explicit port conflict fails loud**

```bash
nc -l 48756 &
NC_PID=$!
CLAWD_MOOD_PORT_TCP=48756 ./plugin/scripts/daemon.py
echo "exit: $?"
kill $NC_PID
```

Expected: daemon exits non-zero with an OSError traceback (because `env_forced=True` re-raises).

- [ ] **Step 5: Regression #4 — singleton via portfile probe**

```bash
./plugin/scripts/daemon.py &
DAEMON_PID=$!
sleep 4
./plugin/scripts/daemon.py
echo "second exit: $?"
kill $DAEMON_PID
```

Expected: second daemon prints `Another clawd-mood daemon is already running.` and exits.

- [ ] **Step 6: Regression #5 — stale portfile self-heal**

```bash
./plugin/scripts/daemon.py &
DAEMON_PID=$!
sleep 4
kill -9 $DAEMON_PID
cat /tmp/clawd-mood.port  # portfile still present
./plugin/scripts/daemon.py &
sleep 4
echo '{"state":"working"}' | nc 127.0.0.1 $(cat /tmp/clawd-mood.port)
pkill -f scripts/daemon.py
```

Expected: second daemon takes over (no "Another daemon" message); ESP32 shows working eye.

- [ ] **Step 7: Regression #6 — dual-CLI concurrency**

Open one terminal with Claude Code, another with `codex`. In each, send prompts in alternation. Expected: ESP32 expressions interleave based on event order; no deadlock or daemon crash. Check `/tmp/clawd-mood-daemon.log` for both prefixes (`PreToolUse` from Claude + `PermissionRequest`/`PreCompact` from Codex if triggered).

- [ ] **Step 8: Regression #7 — Done → Idle 3s revert**

After any Stop event, eye should hold "done" for 3 seconds then auto-revert to "idle". (Firmware-level — verifies IPC changes did not regress timing.)

- [ ] **Step 9: Regression #8 — Sleeping after 5min idle**

Leave daemon + ESP32 untouched for 5 minutes. Expected: eye transitions to sleeping. (Firmware-level. Optionally accelerate by temporarily changing `SLEEP_IDLE_MS` to 15000UL in firmware and re-flashing, then restoring.)

- [ ] **Step 10: Record results (no commit)**

This task produces no file changes. If all 8 regressions pass, proceed to Task 7. If any fail, stop and debug before continuing.

---

## Task 7: 更新 CLAUDE.md

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Read current CLAUDE.md to confirm anchor strings**

```bash
grep -n '^##' CLAUDE.md
```

This lists section headers so the next edits can use precise old_string matches.

- [ ] **Step 2: Update the 项目概要 section**

Replace:
```
**仅支持 macOS**。明确不做：WiFi 控制、OTA、launchd 自启、Windows/Linux 兼容、自动化单元测试、蜂鸣器、多设备。
```

With:
```
**跨平台桌面摆件（macOS / Linux / Windows）**。明确不做：WiFi 控制、OTA、launchd/systemd 系统级自启、自动化单元测试、蜂鸣器、多设备。Windows 端代码完成但 **untested on Windows** —— 欢迎社区验证（见 README）。Linux 同样未在 CI 验证，但 POSIX 路径与 macOS 共用，回归风险低。
```

- [ ] **Step 3: Update the architecture diagram**

Replace these lines (in the三段式架构 code block):
```
/tmp/clawd-mood.fifo          (命名管道，多写者单读者)
       ↓
plugin/scripts/daemon.py      (uv 启动；常驻；读 FIFO 写串口)
```

With:
```
TCP 127.0.0.1:<port>          (portfile: <tempdir>/clawd-mood.port)
       ↓
plugin/scripts/daemon.py      (uv 启动；常驻；TCP server 写串口)
```

And replace:
```
plugin/scripts/hook.sh        (Bash + jq; 事件名 → state JSON)
```

With:
```
plugin/scripts/hook.py        (uv run; 标准库 only; 事件名 → state JSON)
```

- [ ] **Step 4: Delete constraint #1 (macOS timeout)**

Delete the entire block:
```
### 1. macOS 没有 `timeout` 命令
`hook.sh` 写 FIFO 时**不能**用 `timeout 1 bash -c ...` —— 那是 GNU coreutils，stock macOS 没有。必须用 bash 原生的读写打开模式防阻塞：

```bash
exec 3<>"$FIFO"
printf '...' >&3
exec 3>&-
```

修改 `hook.sh` 时如果重新引入 timeout 兜底，会在没装 coreutils 的机器上静默坏掉。参考 commit `0c1da90` 和 `4242ed2`。
```

Renumber the remaining 6 sections (2→1, 3→2, ..., 7→6).

- [ ] **Step 5: Update constraint that became #5 (multi-USB) — was #6**

Replace:
```
### 6. 多 USB CDC 设备
连了多台 ESP32 时，daemon 默认拿 `sorted(glob.glob("/dev/cu.usbmodem*"))[0]`。要指定其他端口用 `CLAWD_MOOD_PORT=/dev/cu.usbmodemXXX ./plugin/scripts/daemon.py`。
```

With:
```
### 5. 多 USB CDC 设备
连了多台 ESP32 时，daemon 默认拿排序后的第一个，扫描规则跨平台：
- macOS: `/dev/cu.usbmodem*`
- Linux: `/dev/ttyACM*`（ESP32-C3 原生 CDC）或 `/dev/ttyUSB*`（CH340/CP2102 桥）
- Windows: `COM*`（仅当 USB VID 不为空，过滤蓝牙 / 调制解调器虚拟口）

指定其他端口用环境变量：`CLAWD_MOOD_PORT=/dev/cu.usbmodemXXX`（mac/linux）或 `CLAWD_MOOD_PORT=COM7`（Windows）。
```

- [ ] **Step 6: Update constraint that became #6 (plugin-level auto-start) — was #7**

Replace the entire section #7 ("插件级 daemon 自启 ≠ launchd 自启") with:

```
### 6. 插件级 daemon 自启 ≠ 系统级自启

`hook.py` 在 `SessionStart` 事件里会先 `probe_daemon()` 探活，没活就用 `subprocess.Popen` 拉脱离的后台进程：
- mac/linux: `start_new_session=True`
- Windows: `creationflags=DETACHED_PROCESS | CREATE_NO_WINDOW`

这是**插件级 / 会话触发**的自启：只在 CLI 第一次启动时拉起 daemon，不依赖系统服务。"明确不做：launchd/systemd 系统级自启"指的仍是写 LaunchAgents / `.service` 让 daemon 跟随开机/登录启动 —— 两者不冲突。

实现上的取舍：
- **Singleton 用 portfile + connect() 探活而非 PID 文件**：避免脏 PID、避免 `pgrep -f` 在 Windows 上不存在的问题。代价是 daemon 异常崩了 portfile 可能残留，但下次启动会自动覆盖
- **首条 SessionStart 状态消息会丢**：daemon 冷启动约 2-3 秒（uv 首次装 pyserial），此时 portfile 还没出现，`hook.py` 走 `port is None` 路径静默退出。下一条 `UserPromptSubmit` 会把状态补推上去
- **daemon 永驻**：不在 `Stop` 事件里关 daemon，避免多个 CLI 会话相互踩踏。需要重启 daemon 就 `pkill -f scripts/daemon.py`（mac/linux）或 `taskkill /F /IM python.exe` 范围内 kill（Windows，注意可能误杀其他 python 进程，更稳的是按 PID）
- **日志位置**：`<tempfile.gettempdir()>/clawd-mood-daemon.log`，即 mac/linux 上 `/tmp/clawd-mood-daemon.log`，Windows 上 `%TEMP%\clawd-mood-daemon.log`
```

- [ ] **Step 7: Add new constraint #7 (portfile service discovery)**

After constraint #6 (the renumbered 自启 one), add:

```
### 7. portfile 服务发现 + 端口自适应

daemon 启动时尝试 bind 默认端口 `48756`，被占则回退到 `bind(0)` 让 OS 分配空闲端口。实际监听端口写入 portfile：`<tempfile.gettempdir()>/clawd-mood.port`（mac/linux: `/tmp/clawd-mood.port`，Windows: `%TEMP%\clawd-mood.port`）。

hook 端不直接读 `48756`，而是读 portfile 拿实际端口。这样：
- 默认端口冲突时自动避让，零配置
- hook 不需要管 daemon 是不是 singleton（read portfile → connect → 通了就发，连不上就静默退）
- 没有 pgrep / tasklist 依赖

显式覆盖：`CLAWD_MOOD_PORT_TCP=N` 强制 daemon 用 N。此时冲突直接报错退出（既然显式指定，应让用户知晓冲突）。
```

- [ ] **Step 8: Update 烧固件 section to include three platforms**

Replace the block under `### 烧固件`:
```
# 编译 + 上传（端口名按实际改）
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
arduino-cli upload -p /dev/cu.usbmodem101 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

With:
```
# 编译 + 上传（端口按平台改）
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood

# macOS
arduino-cli upload -p /dev/cu.usbmodem101 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood

# Linux (可能需先 sudo usermod -aG dialout $USER 后重登)
arduino-cli upload -p /dev/ttyACM0 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood

# Windows (查设备管理器拿 COM 号)
arduino-cli upload -p COM3 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

And replace the device-listing line:
```
板子检测：`ls /dev/cu.usbmodem*`
```

With:
```
板子检测：
- macOS: `ls /dev/cu.usbmodem*`
- Linux: `ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null`
- Windows PowerShell: `Get-CimInstance Win32_SerialPort | Select Name,DeviceID`
```

And update the 首次环境 line:
```
首次环境（README §1-3）：`brew install arduino-cli uv jq` + ESP32 核心 + Adafruit GFX / ST7789 / ArduinoJson 库。
```

To:
```
首次环境（README §1-3）：
- macOS: `brew install arduino-cli uv`（不再需要 jq）
- Linux: 系统包管理器装 `arduino-cli` + `uv`，加入 `dialout` 组
- Windows: `winget install arduino-cli astral-sh.uv`
全平台都要装 ESP32 核心 + Adafruit GFX / ST7789 / ArduinoJson 库。
```

- [ ] **Step 9: Update 跑 daemon section**

Replace the env override example:
```
# 或指定端口
CLAWD_MOOD_PORT=/dev/cu.usbmodem101 ./plugin/scripts/daemon.py
```

With:
```
# 指定串口（mac/linux 给路径，Windows 给 COM 名）
CLAWD_MOOD_PORT=/dev/cu.usbmodem101 ./plugin/scripts/daemon.py
CLAWD_MOOD_PORT=COM3 uv run plugin/scripts/daemon.py   # Windows PowerShell: $env:CLAWD_MOOD_PORT='COM3'

# 指定 TCP 端口（默认 48756，被占则自动回退；显式指定时冲突会直接报错退出）
CLAWD_MOOD_PORT_TCP=49000 ./plugin/scripts/daemon.py
```

- [ ] **Step 10: Update 手动测试 section**

Replace:
```
# 灌 FIFO 走完整管道（daemon → 串口 → 屏幕）
echo '{"state":"working"}' > /tmp/clawd-mood.fifo

# 直接喂 hook.sh（模拟 Claude Code 事件）
echo '{"hook_event_name":"PreToolUse","tool_name":"Bash"}' | ./plugin/scripts/hook.sh
```

With:
```
# 灌 TCP 走完整管道（daemon → 串口 → 屏幕）
# mac/linux:
printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat /tmp/clawd-mood.port)
# Windows PowerShell:
#   $p = Get-Content $env:TEMP\clawd-mood.port
#   $c = New-Object Net.Sockets.TcpClient('127.0.0.1', $p)
#   $s = $c.GetStream(); $b = [Text.Encoding]::UTF8.GetBytes('{"state":"working"}'+"`n")
#   $s.Write($b,0,$b.Length); $c.Close()

# 直接喂 hook.py（模拟 CLI 事件，三平台一致）
echo '{"hook_event_name":"PreToolUse","tool_name":"Bash"}' | ./plugin/scripts/hook.py
```

And update the JSON validation hint:
```
JSON 校验：`python3 -m json.tool plugin/hooks/hooks.json`
```

(Unchanged — already cross-platform.)

- [ ] **Step 11: Update 仓库结构要点**

Replace:
```
- `plugin/scripts/hook.sh` —— event → state 映射，写 FIFO（共享给两端）
- `plugin/scripts/daemon.py` —— PEP 723 内联依赖，FIFO ↔ 串口桥
```

With:
```
- `plugin/scripts/hook.py` —— event → state 映射，TCP 短连接写 daemon（PEP 723，标准库 only，共享给两端）
- `plugin/scripts/daemon.py` —— PEP 723 内联依赖，TCP server ↔ 串口桥；portfile 服务发现
```

And add to the docs list:
```
- `docs/superpowers/specs/2026-06-08-windows-linux-support-design.md` —— Windows + Linux 跨平台设计说明书
- `docs/superpowers/plans/2026-06-08-windows-linux-support-implementation.md` —— 跨平台实施计划
```

- [ ] **Step 12: Update 测试策略 section**

In the bullet list `**没有自动化测试**`, replace:
```
- **固件单独**：Arduino 串口监视器手输 7 种 JSON，肉眼确认 7 表情
- **daemon + 固件**：`echo … > /tmp/clawd-mood.fifo`
- **端到端**：跑真实任务（"列出当前目录"），观察 Idle → Thinking → Working → Done → Idle 序列
```

With:
```
- **固件单独**：Arduino 串口监视器手输 7 种 JSON，肉眼确认 7 表情
- **daemon + 固件**：`printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat /tmp/clawd-mood.port)`（mac/linux），Windows PowerShell 见上"手动测试"段
- **端到端**：跑真实任务（"列出当前目录"），观察 Idle → Thinking → Working → Done → Idle 序列
- **Linux / Windows**：未在本地端到端验证；代码层 review 确认无 macOS-isms。社区验证 welcome
```

- [ ] **Step 13: Sanity-check that all edits applied**

```bash
grep -c 'hook\.sh' CLAUDE.md
grep -c 'hook\.py' CLAUDE.md
grep -c '/tmp/clawd-mood\.fifo' CLAUDE.md
grep -c 'portfile' CLAUDE.md
```

Expected: `hook.sh` count should match historical commit references only (`0c1da90` / `4242ed2` etc.). FIFO count > 0 only if mentioning history. `hook.py` and `portfile` should be > 5 each.

- [ ] **Step 14: Commit**

```bash
git add CLAUDE.md
git commit -m "$(cat <<'EOF'
docs(claude-md): rewrite for macOS + Linux + Windows support

- Drop "仅支持 macOS" — three-platform with Windows untested
- Replace FIFO references with TCP/portfile description
- Replace hook.sh references with hook.py
- Drop obsolete "macOS no timeout" constraint
- Expand serial detection table (cu.usbmodem / ttyACM / COM)
- Document portfile + auto-fallback port selection
- Cross-platform install / flash / smoke-test commands
EOF
)"
```

---

## Task 8: 重写 README.md

**Files:**
- Modify: `README.md` (重组为平台共通 + 三平台并列)

- [ ] **Step 1: Read current README**

```bash
cat README.md | head -100
```

Note current structure to preserve tone / brand voice.

- [ ] **Step 2: Restructure into platform-aware sections**

Open `README.md` and rewrite as follows (preserve the project intro paragraph at top, then replace the rest):

```markdown
> **0.2.0 BREAKING**: `/tmp/clawd-mood.fifo` 不再使用，改 TCP `127.0.0.1:<port>`（端口写在 `<tempdir>/clawd-mood.port`）。`plugin/scripts/hook.sh` 已删除，统一用 `plugin/scripts/hook.py`。升级方式：Claude Code 用户重新 `claude --plugin-dir`；Codex 用户 `codex plugin remove/add` 刷新缓存。

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

> ⚠️ **Untested on Windows.** Code paths are reviewed but not end-to-end verified on real Windows + ESP32 hardware. If you try it, please open an issue with your result.

```powershell
winget install ArduinoSA.CLI
winget install astral-sh.uv
```

## 烧固件

先装 ESP32 板支持 + 库：

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit GFX Library" "Adafruit ST7735 and ST7789 Library" "ArduinoJson"
```

编译固件（三平台相同）：

```bash
arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
```

上传时根据平台选端口：

| 平台 | 上传命令 |
|---|---|
| macOS | `arduino-cli upload -p /dev/cu.usbmodem101 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood` |
| Linux | `arduino-cli upload -p /dev/ttyACM0 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood` |
| Windows | `arduino-cli upload -p COM3 -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood` |

查 ESP32 端口号：
- macOS: `ls /dev/cu.usbmodem*`
- Linux: `ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null`
- Windows PowerShell: `Get-CimInstance Win32_SerialPort | Select Name,DeviceID`

**⚠️ 接线注意**：显示器 VCC 接 **3.3V**，不要接 5V，否则烧屏。

## 挂插件

### Claude Code

```bash
claude --plugin-dir /absolute/path/to/clawd-mood/plugin
```

在 Claude Code 里 `/hooks` 应看到 9 个事件（SessionStart / UserPromptSubmit / PreToolUse / PostToolUse / PostToolUseFailure / Notification / Stop / SubagentStart / SubagentStop）。

### Codex CLI（≥ 0.133.0）

```bash
cd /absolute/path/to/clawd-mood
codex plugin marketplace add .
codex plugin add clawd-mood@clawd-mood
```

任意目录 `codex` 就能用。在 codex 里 `/hooks` 应看到 10 个事件（含 `PermissionRequest` / `PreCompact` / `PostCompact`）。

> Codex 把 plugin 拷到 `~/.codex/plugins/cache/`（非 symlink）。如果你改了仓库里的 `hook.py` / `hooks-codex.json`，要 `codex plugin remove/add` 刷新缓存才会生效。Claude Code 走 `--plugin-dir` 是实时路径，无此问题。

## 验证

打开任意 CLI 跑一个简单任务（如"列出当前目录"）。ESP32 屏幕应依次显示：

`idle → thinking → working → done`，3 秒后回 `idle`。5 分钟无消息进 `sleeping`。

直接测试管道（不开 CLI）：

```bash
# mac/linux
printf '{"state":"working"}\n' | nc 127.0.0.1 $(cat /tmp/clawd-mood.port)
```

```powershell
# Windows PowerShell（untested）
$p = Get-Content $env:TEMP\clawd-mood.port
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', $p)
$s = $c.GetStream()
$b = [Text.Encoding]::UTF8.GetBytes('{"state":"working"}' + "`n")
$s.Write($b, 0, $b.Length); $c.Close()
```

## 卸载

```bash
# Claude Code
# 把 --plugin-dir 从启动命令里去掉即可

# Codex
codex plugin remove clawd-mood@clawd-mood

# Stop daemon + clean up
# mac/linux:
pkill -f scripts/daemon.py; rm -f /tmp/clawd-mood.port
# Windows PowerShell:
#   Get-Process python | Where-Object { $_.CommandLine -like '*daemon.py*' } | Stop-Process
#   Remove-Item $env:TEMP\clawd-mood.port -ErrorAction SilentlyContinue
```
```

- [ ] **Step 3: Sanity-check README links and no stale FIFO references**

```bash
grep -c 'hook\.sh' README.md
grep -c '/tmp/clawd-mood\.fifo' README.md
grep -c 'hook\.py' README.md
grep -c 'portfile\|clawd-mood\.port' README.md
```

Expected: first two should be `0` (except possibly inside the BREAKING note); last two should be > 0.

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "$(cat <<'EOF'
docs(readme): restructure for macOS + Linux + Windows

- Add 0.2.0 BREAKING notice (FIFO → TCP/portfile, hook.sh → hook.py)
- Split 准备 / 烧固件 / 卸载 sections per platform
- Add Windows 'untested' banner
- Cross-platform serial-port probing and TCP smoke commands
EOF
)"
```

---

## Task 9: 最终验证 + ready PR

**Files:** No code changes — verify the full diff and prepare PR.

- [ ] **Step 1: Static grep for residual macOS-isms**

```bash
grep -rn 'mkfifo\|/tmp/clawd-mood\.fifo\|pgrep\|nohup' \
  plugin/ docs/superpowers/specs/2026-06-08-* docs/superpowers/plans/2026-06-08-* \
  README.md CLAUDE.md 2>&1 | grep -v ':\([0-9]*\):\s*#\|HISTORICAL\|0\.1\.x'
```

Expected: no matches in `plugin/` or current docs. Historical references in `docs/superpowers/specs/2026-06-07-*.md` are OK.

- [ ] **Step 2: Static grep for cross-platform usage**

```bash
grep -n 'sys\.platform\|list_ports\|tempfile\.gettempdir\|start_new_session\|DETACHED_PROCESS\|creationflags' \
  plugin/scripts/daemon.py plugin/scripts/hook.py
```

Expected: each platform branch appears (`darwin` / `linux` / `win32`), `list_ports` in daemon, `tempfile.gettempdir` in both, `start_new_session` and `DETACHED_PROCESS` in hook.

- [ ] **Step 3: Final JSON validation**

```bash
python3 -m json.tool plugin/hooks/hooks.json > /dev/null && echo "hooks.json OK"
python3 -m json.tool plugin/hooks/hooks-codex.json > /dev/null && echo "hooks-codex.json OK"
python3 -m json.tool plugin/.claude-plugin/plugin.json > /dev/null && echo "claude-plugin OK"
python3 -m json.tool plugin/.codex-plugin/plugin.json > /dev/null && echo "codex-plugin OK"
python3 -m json.tool .agents/plugins/marketplace.json > /dev/null && echo "marketplace OK"
```

Expected: 5 `OK` lines.

- [ ] **Step 4: One last end-to-end smoke on macOS**

```bash
pkill -f scripts/daemon.py 2>/dev/null; rm -f /tmp/clawd-mood.port
```

Restart Claude Code via `claude --plugin-dir …`, send a simple prompt. Verify expression sequence works. If yes, proceed.

- [ ] **Step 5: Show diff summary against main**

```bash
git log --oneline main..feature/windows-linux-support
git diff --stat main..feature/windows-linux-support
```

Expected: ~9 commits, file diff shows touched files matching the File Structure table above.

- [ ] **Step 6: Push and open PR (only if user confirms)**

This is a destructive-ish action (push to remote, may trigger CI). Ask the user before running:

```bash
git push -u origin feature/windows-linux-support
gh pr create --title "feat: cross-platform support (macOS + Linux + Windows)" --body "$(cat <<'EOF'
## Summary
- Replace POSIX-only IPC/hook stack with cross-platform Python equivalents
- `/tmp/clawd-mood.fifo` → TCP 127.0.0.1:<port> + portfile service discovery
- `hook.sh` (bash + jq) → `hook.py` (uv run, stdlib only)
- `pyserial.list_ports` for serial enumeration on all three platforms
- `subprocess.Popen(start_new_session / DETACHED_PROCESS)` for cross-platform daemon spawn
- portfile + `connect()` probe replaces `pgrep -f` for singleton detection

## Scope
- **macOS**: end-to-end regression tested (spec §9 — 8/8 passing)
- **Linux**: code complete + docs complete, untested (community verification welcome)
- **Windows**: code complete + docs complete, untested (community verification welcome)
- Firmware: unchanged

## Breaking changes (0.2.0)
- FIFO removed; TCP socket replaces it
- `hook.sh` removed; `hook.py` replaces it
- Upgrade: Claude Code users re-run `claude --plugin-dir`; Codex users `plugin remove/add` to refresh cache

## Test plan
- [x] macOS cold start
- [x] macOS port-conflict auto-fallback
- [x] macOS explicit `CLAWD_MOOD_PORT_TCP` conflict fails loud
- [x] macOS singleton via portfile probe
- [x] macOS stale-portfile self-heal
- [x] macOS dual-CLI (Claude Code + Codex) concurrency
- [x] macOS firmware-level Done → Idle 3s revert
- [x] macOS firmware-level Sleeping after 5min idle
- [ ] Linux end-to-end (no environment available — community welcome)
- [ ] Windows end-to-end (no environment available — community welcome)

EOF
)"
```

---

## Self-Review

After writing, the plan was reviewed against the spec. Findings:

**Spec coverage:**
- §1 目标 → covered by goal/scope at top + Task 6 macOS regression + Tasks 7/8 docs
- §2 范围 → all "做什么" items mapped to Tasks 1-8; "不做什么" honored (no automated tests, no launchd, no Windows-specific tests)
- §3 整体架构 → realized by Task 1 (daemon) + Task 2 (hook) + Task 3/4 (hooks.json)
- §4 文件结构 diff → matches File Structure table
- §5.1 hook.py → Task 2 contains complete code
- §5.2 daemon.py → Task 1 contains complete code
- §5.3 hooks.json/hooks-codex.json → Tasks 3 and 4
- §6.1 CLAUDE.md → Task 7 covers all 8 documented edits
- §6.2 README.md → Task 8
- §6.3 兼容性公告 → Task 8 step 2 includes BREAKING notice
- §7 依赖矩阵 → reflected in Task 1/2 code
- §8 错误处理 → realized in code (Task 1 / 2); verified in Task 6 regressions
- §9 测试策略 → Task 6 covers all 8 regression cases
- §10 风险与未决 → uv-on-Windows / CLAUDE_PLUGIN_ROOT / systemd-logind / portfile-multi-user / port 48756 — all 5 items are realized as code paths (PoP creationflags, `__file__` self-locating, atexit cleanup) or documented as "community verification" in Tasks 8 / 9. No coverage gap.

**Placeholder scan:** Searched for "TBD", "TODO", "implement later", "similar to Task N". None found. Each step contains either complete code or an exact command with expected output.

**Type consistency:** Function names align across tasks: `read_portfile()`, `probe_daemon()`, `ensure_daemon_running()`, `bind_listen()`, `check_singleton()`, `install_signal_handlers()`, `detect_port()`, `open_serial()` — defined in Task 1/2 and not referenced inconsistently elsewhere. Constants `PORTFILE`, `LOGFILE`, `BAUD_RATE`, `DEFAULT_TCP_PORT`, `EVENT_TO_STATE` consistent in case and spelling.

No fixes needed.
