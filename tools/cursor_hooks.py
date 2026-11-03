#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""Add or remove clawd-mood's hooks in Cursor's user-level hooks.json.

    uv run tools/cursor_hooks.py install      # or: uninstall
    uv run tools/cursor_hooks.py install --file <project>/.cursor/hooks.json

User-level hooks rather than a Cursor plugin: cursor-agent only runs
beforeSubmitPrompt / stop when user or project hooks define them, so a plugin
alone never shows thinking or done there. Entries point at this checkout's
hook.py, so edits take effect without reinstalling; run install again after
moving the repo. Other tools' entries in the file are left alone.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import sys
import tempfile
from pathlib import Path

EVENTS = [
    "sessionStart", "beforeSubmitPrompt", "preToolUse", "postToolUse",
    "postToolUseFailure", "preCompact", "stop", "sessionEnd",
]
HOOK = Path(__file__).resolve().parent.parent / "plugin" / "scripts" / "hook.py"
# Any clawd-mood hook.py run with the cursor argument, wherever the repo was
OURS = re.compile(r"""scripts/hook\.py['"]? cursor\b""")


def command() -> str:
    if sys.platform == "win32":
        # Cursor runs hooks through PowerShell, where `|| true` is a syntax
        # error; uv's exit code 2 doesn't survive powershell -Command anyway.
        return f'uv run "{HOOK}" cursor'
    # uv exits 2 on its own errors, which Cursor treats as "block the action"
    return f"uv run {shlex.quote(str(HOOK))} cursor || true"


def is_ours(entry: object) -> bool:
    cmd = entry.get("command", "") if isinstance(entry, dict) else ""
    return isinstance(cmd, str) and bool(OURS.search(cmd.replace("\\", "/")))


def load(path: Path) -> dict:
    if not path.exists():
        return {"version": 1, "hooks": {}}
    by_hand = (f"edit it by hand: under \"hooks\", add {{\"command\": {json.dumps(command(), ensure_ascii=False)}, "
               f"\"timeout\": 5}} to each of {', '.join(EVENTS)}")
    try:
        config = json.loads(path.read_text(encoding="utf-8") or "{}")
    except ValueError as e:
        sys.exit(f"{path} is not plain JSON ({e}); {by_hand}")
    if not isinstance(config, dict) or not isinstance(config.get("hooks", {}), dict):
        sys.exit(f"{path} has no hooks object; {by_hand}")
    config.setdefault("version", 1)
    config.setdefault("hooks", {})
    return config


def save(path: Path, config: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=".hooks-", suffix=".json")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("action", choices=["install", "uninstall"])
    parser.add_argument("--file", type=Path, default=Path.home() / ".cursor" / "hooks.json")
    args = parser.parse_args()

    config = load(args.file)
    hooks = config["hooks"]
    for event in list(hooks):
        if isinstance(hooks[event], list):
            hooks[event] = [e for e in hooks[event] if not is_ours(e)]
            if not hooks[event]:
                del hooks[event]
    if args.action == "install":
        for event in EVENTS:
            hooks.setdefault(event, []).append({"command": command(), "timeout": 5})
    save(args.file, config)
    done = "added" if args.action == "install" else "removed"
    print(f"clawd-mood hooks {done}: {args.file}")
    if args.action == "install":
        print(f"  -> {command()}")


if __name__ == "__main__":
    main()
