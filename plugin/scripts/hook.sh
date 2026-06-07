#!/bin/bash
# clawd-mood hook — maps Claude Code events to ESP32 states via FIFO.
# Receives hook JSON on stdin, writes state JSON to /tmp/clawd-mood.fifo.

FIFO="/tmp/clawd-mood.fifo"

# Exit silently if daemon isn't running or jq is missing.
[ -p "$FIFO" ] || exit 0
command -v jq >/dev/null || exit 0

INPUT=$(cat)
EVENT=$(echo "$INPUT" | jq -r '.hook_event_name // empty')
TOOL=$(echo "$INPUT" | jq -r '.tool_name // empty')

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
  *)                  exit 0 ;;
esac

# 1 second timeout protects against FIFO with no reader (daemon died mid-event).
timeout 1 bash -c "echo '{\"state\":\"$STATE\",\"event\":\"$EVENT\",\"tool\":\"$TOOL\"}' > '$FIFO'" || true
exit 0
