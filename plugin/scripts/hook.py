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
