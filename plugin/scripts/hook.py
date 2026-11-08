#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# The 3.12 above is only what `uv run` picks (same as daemon.py); the code itself
# must stay runnable on Python 3.9: hook.sh falls back to the system python3 and
# hooks-dsh.json runs `python3 hook.py` directly. No 3.10+ syntax or stdlib here.
"""MiqiPet hook — maps CLI hook events to ESP32 states via TCP.

Receives hook JSON on stdin (from Claude Code / Codex CLI / WorkBuddy /
DeepSeek Harness / Cursor), looks up the event in EVENT_TO_STATE, then opens a
short TCP connection to the daemon and sends a single NDJSON line tagged with
the session it came from. On SessionStart, spawns the daemon detached if it is
not already running. Pure stdlib (no third-party deps) so uv run is fast
(~80-200ms cached), and it runs on the python3 macOS ships (3.9): hook.sh next
to this file falls back to that when uv is not on PATH, and DeepSeek Harness
uses it directly (uv cannot init its cache in that sandbox).

Pass "codex", "dsh" or "cursor" as argv[1] from those hook configs (Cursor's
entries are written by tools/cursor_hooks.py); otherwise the source is
WorkBuddy or Claude Code, told apart by environment. Cursor hooks are
synchronous, so this must never write to stdout: an empty reply lets the
action through, anything else is parsed as the hook's decision.
"""

from __future__ import annotations  # `int | None` on the python3 macOS ships

import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

PORTFILE = Path(tempfile.gettempdir()) / "miqipet.port"
LOGFILE = Path(tempfile.gettempdir()) / "miqipet-daemon.log"
DAEMON = Path(__file__).parent / "daemon.py"

EVENT_TO_STATE = {
    "SessionStart":       "idle",
    "UserPromptSubmit":   "thinking",
    "PreToolUse":         "working",
    "PostToolUse":        "working",
    "PostToolUseFailure": "working",     # Claude Code, Cursor; e.g. `diff` exiting 1, the agent carries on
    "Notification":       "waiting",     # Claude Code only
    "Stop":               "done",
    "StopFailure":        "error",       # Claude Code, Cursor
    "PermissionRequest":  "waiting",
    "PreCompact":         "thinking",
    "PostCompact":        "working",
    "Interrupt":          "idle",        # Codex, Cursor
    "SessionEnd":         "idle",
    "SubagentStart":      "thinking",    # only ever drives a follower, see main()
    "SubagentStop":       "done",
}

# Cursor's own event names, renamed to the ones above. Cursor has no event for
# a pending approval or for the end of a compaction, and its subagentStop
# doesn't say which subagent ended, so it gets no followers.
CURSOR_EVENTS = {
    "sessionStart":       "SessionStart",
    "beforeSubmitPrompt": "UserPromptSubmit",
    "preToolUse":         "PreToolUse",
    "postToolUse":        "PostToolUse",
    "postToolUseFailure": "PostToolUseFailure",
    "preCompact":         "PreCompact",
    "stop":               "Stop",
    "sessionEnd":         "SessionEnd",
}
CURSOR_STOPS = {"aborted": "Interrupt", "error": "StopFailure"}

# Claude Code also notifies when a finished turn sits idle or auth succeeds;
# neither is something pending. Payloads without a type keep meaning waiting.
WAITING_NOTIFICATIONS = {"permission_prompt", "elicitation_dialog"}

MAX_DEVICE_FOLLOWERS = 12  # MAX_FOLLOWERS in the firmware; older goodbyes have nothing to remove


def cursor_event(payload: dict) -> str | None:
    event = CURSOR_EVENTS.get(payload.get("hook_event_name", ""))
    if event == "Stop":
        return CURSOR_STOPS.get(payload.get("status"), "Stop")
    if event == "PostToolUseFailure" and payload.get("is_interrupt"):
        return None  # the user cancelled; stop follows with status aborted
    return event


def tail_lines(path: str, limit: int = 256 * 1024) -> list[str]:
    """Last lines of a possibly huge JSONL file, newest first."""
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        f.seek(max(0, size - limit))
        lines = f.read().decode("utf-8", errors="replace").splitlines()
    return (lines[1:] if size > limit else lines)[::-1]


def read_title(payload: dict, source: str) -> str | None:
    """Best-effort session title from CLI-internal files; None when unavailable."""
    if source == "cursor":
        return None  # Cursor keeps chat titles in its own database
    try:
        if source == "codex":
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


def detect_shared_source() -> str:
    """Claude Code and WorkBuddy share hooks.json; only WorkBuddy's engine sets this."""
    return "WB" if os.environ.get("CODEBUDDY_PLUGIN_ROOT") else "CC"


def session_fields(payload: dict, source: str) -> dict:
    """Short identity for the device (labels are cut to fit its 320-byte lines)."""
    if source == "cursor":
        # User hooks run from ~/.cursor and only tool events carry cwd, so
        # name the session after the workspace instead.
        roots = payload.get("workspace_roots") or []
        cwd = (roots[0] if roots else "") or os.environ.get("CURSOR_PROJECT_DIR") or os.getcwd()
        raw_id = payload.get("conversation_id") or payload.get("session_id") or cwd
    else:
        cwd = payload.get("cwd") or os.getcwd()
        raw_id = payload.get("session_id") or cwd
    short_id = hashlib.sha1(raw_id.encode()).hexdigest()[:8]
    fields = {
        "session": short_id,
        "name": clean_label(os.path.basename(cwd.rstrip("/\\\\"))) or short_id,
        "src": {"codex": "CX", "dsh": "DS", "cursor": "CU"}.get(source) or detect_shared_source(),
    }
    title = clean_label(read_title(payload, source) or "")
    if title:
        fields["title"] = title
    return fields


def journal_stamp(session: str) -> Path:
    return Path(tempfile.gettempdir()) / f"miqipet-journal-{session}.json"


def finished_workflow_agents(payload: dict, session: str) -> list[str]:
    """Workflow subagents fire no SubagentStop; their journal is the only end signal.

    CLI-internal, so best-effort: the device times followers out anyway. Hook runs
    race on the stamp, which at worst repeats a goodbye the device ignores.
    """
    finished: list[str] = []
    try:
        transcript = payload.get("transcript_path")
        if not transcript:
            return finished
        journals = list(Path(transcript).with_suffix("").glob("subagents/workflows/*/journal.jsonl"))
        if not journals:
            return finished
        stamp = journal_stamp(session)
        try:
            read_upto = json.loads(stamp.read_text())
        except (OSError, ValueError):
            read_upto = {}
        before = dict(read_upto)
        for journal in journals:
            offset = read_upto.get(journal.parent.name, 0)
            with open(journal, "rb") as f:
                f.seek(offset)
                chunk = f.read()
            chunk = chunk[:chunk.rfind(b"\n") + 1]  # a line may still be mid-write
            read_upto[journal.parent.name] = offset + len(chunk)
            for line in chunk.splitlines():
                entry = json.loads(line)
                if entry.get("type") == "result" and entry.get("agentId"):
                    finished.append(entry["agentId"])
        if read_upto != before:
            scratch = stamp.with_name(f"{stamp.name}.{os.getpid()}")
            scratch.write_text(json.dumps(read_upto))
            os.replace(scratch, stamp)
    except (OSError, ValueError, AttributeError):
        pass
    return finished[-MAX_DEVICE_FOLLOWERS:]


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


def config_path() -> Path:
    """The shared config file, same rules as daemon.py (which owns the format)."""
    override = os.environ.get("MIQIPET_CONFIG")
    if override:
        return Path(override)
    try:
        import pwd  # $HOME can be replaced (DeepSeek Harness does), so ask the user database first
        return Path(pwd.getpwuid(os.getuid()).pw_dir) / ".miqipet" / "config.json"
    except (ImportError, KeyError, AttributeError):
        return Path.home() / ".miqipet" / "config.json"


def configured_daemon_argv() -> list[str] | None:
    """`daemon_argv` from the config: how the packaged desktop app wants the daemon started.

    The desktop app writes it only when it runs as a bundle (it has no uv to
    offer); a dev checkout removes it. Ignored unless argv[0] is an existing
    file, so a stale path after an uninstall falls back to daemon.py.
    """
    try:
        argv = json.loads(config_path().read_text(encoding="utf-8")).get("daemon_argv")
        if isinstance(argv, list) and argv and all(isinstance(a, str) for a in argv) and Path(argv[0]).is_file():
            return argv
    except (OSError, ValueError, AttributeError):
        pass
    return None


def spawn_detached(argv: list[str], log_fh) -> None:
    if sys.platform == "win32":
        CREATE_NO_WINDOW = 0x08000000
        DETACHED_PROCESS = 0x00000008
        subprocess.Popen(
            argv,
            stdout=log_fh,
            stderr=log_fh,
            stdin=subprocess.DEVNULL,
            creationflags=DETACHED_PROCESS | CREATE_NO_WINDOW,
            close_fds=True,
        )
    else:
        subprocess.Popen(
            argv,
            stdout=log_fh,
            stderr=log_fh,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
            close_fds=True,
        )


def ensure_daemon_running() -> None:
    if probe_daemon():
        return
    # Fallback relies on uv: the shebang on POSIX, `uv run` on Windows.
    fallback = ["uv", "run", str(DAEMON)] if sys.platform == "win32" else [str(DAEMON)]
    configured = configured_daemon_argv()
    candidates = [configured, fallback] if configured else [fallback]
    try:
        log_fh = open(LOGFILE, "a")
    except OSError:
        return
    with log_fh:
        for argv in candidates:
            try:
                spawn_detached(argv, log_fh)
                return
            except OSError as e:  # missing uv, unreadable executable...; never fail the hook
                try:
                    log_fh.write(f"hook: could not start daemon with {argv!r}: {e}\n")
                    log_fh.flush()
                except OSError:
                    pass


def main() -> None:
    try:
        payload = json.loads(sys.stdin.read())
    except json.JSONDecodeError:
        return
    source = sys.argv[1] if len(sys.argv) > 1 else ""
    if source == "cursor":
        event = cursor_event(payload) or ""
    elif "cursor_version" in payload:
        # Cursor also runs the hooks of installed Claude Code plugins; its
        # sessions are reported by the cursor entries, not twice.
        return
    else:
        event = payload.get("hook_event_name", "")
    tool = (payload.get("tool_name") or "")[:32]
    state = EVENT_TO_STATE.get(event)
    if state is None:
        return
    if event == "Notification" and payload.get("notification_type", "permission_prompt") not in WAITING_NOTIFICATIONS:
        return

    # WorkBuddy can set agent_id from any session's store id, so outside the subagent
    # lifecycle events require a type too (its SubagentStart has an id but no type)
    agent_id = payload.get("agent_id") if payload.get("agent_type") or event.startswith("Subagent") else None
    if event.startswith("Subagent") and not agent_id:
        return  # can't tell which follower it means; must not touch the main state

    if event == "SessionStart":
        ensure_daemon_running()
        if payload.get("source") == "compact":
            return  # fires mid-turn after compaction; don't drop back to idle
        if sys.argv[1:] == ["dsh"]:
            return  # dsh fires this for every session it restores at startup and never ends them

    port = read_portfile()
    if port is None:
        return  # daemon still warming up; next event will catch up
    message = {"state": state, "event": event, "tool": tool, **session_fields(payload, source)}
    if event == "SessionEnd":
        message["end"] = True
    if agent_id:
        message["agent"] = agent_id[-6:]
        if event == "SubagentStop":
            message["gone"] = True
    lines = [message]
    session = message["session"]
    if event == "SessionEnd":
        journal_stamp(session).unlink(missing_ok=True)
    elif source != "cursor":  # Claude Code's workflow journals; Cursor hooks are synchronous
        lines += [
            {"state": "done", "event": "WorkflowAgentDone", "session": session, "agent": agent[-6:], "gone": True}
            for agent in finished_workflow_agents(payload, session)
        ]
    data = "".join(json.dumps(m, ensure_ascii=False) + "\n" for m in lines)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.3) as s:
            s.sendall(data.encode("utf-8"))
    except OSError:
        pass  # daemon not responding; silently drop


if __name__ == "__main__":
    main()
