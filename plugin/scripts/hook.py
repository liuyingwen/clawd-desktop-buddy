#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""clawd-mood hook — maps CLI hook events to ESP32 states via TCP.

Receives hook JSON on stdin (from Claude Code / Codex CLI), looks up the
event in EVENT_TO_STATE, then opens a short TCP connection to the daemon
and sends a single NDJSON line tagged with the session it came from. On
SessionStart, spawns the daemon detached if it is not already running. Pure
stdlib (no third-party deps) so uv run is fast (~80-200ms cached).

Pass "codex" as argv[1] from the Codex hook config; default source is Claude Code.
"""

import hashlib
import json
import os
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
    "StopFailure":        "error",       # Claude Code only
    "SubagentStart":      "working",
    "SubagentStop":       "working",
    "PermissionRequest":  "waiting",
    "PreCompact":         "thinking",
    "PostCompact":        "working",
    "Interrupt":          "idle",        # Codex only
    "SessionEnd":         "idle",
}


def tail_lines(path: str, limit: int = 256 * 1024) -> list[str]:
    """Last lines of a possibly huge JSONL file, newest first."""
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        f.seek(max(0, size - limit))
        lines = f.read().decode("utf-8", errors="replace").splitlines()
    return (lines[1:] if size > limit else lines)[::-1]


def read_title(payload: dict, codex: bool) -> str | None:
    """Best-effort session title from CLI-internal files; None when unavailable."""
    try:
        if codex:
            path = str(Path.home() / ".codex" / "session_index.jsonl")
            for line in tail_lines(path):
                entry = json.loads(line)
                if entry.get("id") == payload.get("session_id"):
                    return entry.get("thread_name")
        elif payload.get("transcript_path"):
            for line in tail_lines(payload["transcript_path"]):
                if '"ai-title"' in line:
                    entry = json.loads(line)
                    if entry.get("type") == "ai-title":
                        return entry.get("aiTitle")
    except (OSError, ValueError, AttributeError):
        pass
    return None


def clean_label(text: str) -> str:
    return "".join(c for c in text if c.isprintable())[:16]


def session_fields(payload: dict) -> dict:
    """Short identity for the device (labels are cut to fit its 320-byte lines)."""
    cwd = payload.get("cwd") or os.getcwd()
    raw_id = payload.get("session_id") or cwd
    short_id = hashlib.sha1(raw_id.encode()).hexdigest()[:8]
    codex = sys.argv[1:] == ["codex"]
    fields = {
        "session": short_id,
        "name": clean_label(os.path.basename(cwd.rstrip("/\\\\"))) or short_id,
        "src": "CX" if codex else "CC",
    }
    title = clean_label(read_title(payload, codex) or "")
    if title:
        fields["title"] = title
    return fields


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
    tool = (payload.get("tool_name") or "")[:32]
    state = EVENT_TO_STATE.get(event)
    if state is None:
        return

    if event == "SessionStart":
        ensure_daemon_running()
        if payload.get("source") == "compact":
            return  # fires mid-turn after compaction; don't drop back to idle

    port = read_portfile()
    if port is None:
        return  # daemon still warming up; next event will catch up
    message = {"state": state, "event": event, "tool": tool, **session_fields(payload)}
    if event == "SessionEnd":
        message["end"] = True
    line = json.dumps(message, ensure_ascii=False) + "\n"
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.3) as s:
            s.sendall(line.encode("utf-8"))
    except OSError:
        pass  # daemon not responding; silently drop


if __name__ == "__main__":
    main()
