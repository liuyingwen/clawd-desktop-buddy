#!/bin/sh
# MiqiPet hook launcher: runs hook.py (next to this file) with whatever
# Python is available, forwarding stdin and all arguments.
#
#   uv on PATH          -> uv run hook.py "$@"   (fast, cached, the normal case)
#   else a real python3 -> python3 hook.py "$@"  (hook.py is stdlib-only, 3.9+)
#   else                -> do nothing
#
# On macOS /usr/bin/python3 is only a placeholder until the Command Line Tools
# are installed, and running it pops up an install dialog in the middle of the
# user's session; `xcode-select -p` succeeding is the sign that a real one is
# there. That check only applies when python3 resolves to /usr/bin/python3: a
# Homebrew / pyenv python3 on PATH works without the Command Line Tools. Agents launched from the Dock (Cursor, the desktop app) often have no
# uv on PATH, which is what the fallback is for.
#
# Always exits 0: a hook's non-zero exit (uv's own errors are 2) would be read
# as "block the action" by synchronous hooks, so the hook configs add `|| true`
# and this script never relies on it.

# No external commands before the interpreter is found: with a bare PATH even
# dirname may be missing.
case $0 in
  */*) dir=${0%/*} ;;
  *) dir=. ;;
esac
hook="$dir/hook.py"
[ -f "$hook" ] || exit 0

if command -v uv >/dev/null 2>&1; then
  uv run "$hook" "$@" || true
  exit 0
fi

python_ok=0
if py=$(command -v python3 2>/dev/null) && [ -n "$py" ]; then
  case $py in
    /usr/bin/python3)
      # Apple's stub: only real once the Command Line Tools are installed.
      # A Homebrew / pyenv / python.org python3 earlier on PATH is fine as is.
      if [ -x /usr/bin/xcode-select ]; then
        xcode-select -p >/dev/null 2>&1 && python_ok=1
      else
        python_ok=1
      fi ;;
    *) python_ok=1 ;;
  esac
fi
if [ "$python_ok" = 1 ]; then
  python3 "$hook" "$@" || true
fi
exit 0
