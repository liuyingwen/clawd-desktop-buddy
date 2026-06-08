#!/bin/bash
# clawd-mood hook — maps Claude Code events to ESP32 states via FIFO.
# Receives hook JSON on stdin, writes state JSON to /tmp/clawd-mood.fifo.

FIFO="/tmp/clawd-mood.fifo"
DAEMON="$CLAUDE_PLUGIN_ROOT/scripts/daemon.py"

command -v jq >/dev/null || exit 0

INPUT=$(cat)
EVENT=$(echo "$INPUT" | jq -r '.hook_event_name // empty')
TOOL=$(echo "$INPUT" | jq -r '.tool_name // empty')

# SessionStart spawns daemon if not already running. Detached so it survives
# across Claude Code sessions and outlives this hook. First SessionStart state
# is lost during cold start (~2-3s for uv to install pyserial on first run) —
# the next event (UserPromptSubmit) will re-push state. This is plugin-level
# auto-start (per-session); launchd-level system auto-start is still out of scope.
if [ "$EVENT" = "SessionStart" ] && ! pgrep -f "$DAEMON" >/dev/null 2>&1; then
  nohup "$DAEMON" >/tmp/clawd-mood-daemon.log 2>&1 </dev/null &
  disown
fi

# Exit silently if FIFO not ready (daemon still warming up or not running).
[ -p "$FIFO" ] || exit 0

case "$EVENT" in
  SessionStart)       STATE="idle" ;;
  UserPromptSubmit)   STATE="thinking" ;;
  PreToolUse)         STATE="working" ;;
  PostToolUse)        STATE="working" ;;
  PostToolUseFailure) STATE="error" ;;
  Notification)       STATE="waiting" ;;
  Stop)               STATE="done" ;;
  SubagentStart)      STATE="working" ;;
  SubagentStop)       STATE="working" ;;
  PermissionRequest)  STATE="waiting" ;;
  PreCompact)         STATE="thinking" ;;
  PostCompact)        STATE="working" ;;
  *)                  exit 0 ;;
esac

# Open FIFO in read-write mode so the write doesn't block when no reader is
# attached (e.g. daemon died mid-event). printf %s avoids shell-quote injection
# from tool names like "Edit's" that could break a naive double-quoted echo.
exec 3<>"$FIFO"
printf '{"state":"%s","event":"%s","tool":"%s"}\n' "$STATE" "$EVENT" "$TOOL" >&3
exec 3>&-
exit 0
