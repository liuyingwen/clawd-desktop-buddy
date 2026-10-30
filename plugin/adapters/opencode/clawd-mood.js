// clawd-mood plugin for opencode >= 2.0 — pushes states straight to the daemon over
// TCP (one uv process per event would lose the last state: `opencode run` exits
// milliseconds after the turn). hook.py is only used to autostart the daemon.
// Exports a plain object so it needs no @opencode/plugin install.

import { spawn } from "node:child_process";
import { readFileSync, realpathSync } from "node:fs";
import { connect } from "node:net";
import { tmpdir } from "node:os";
import { basename, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HOOK_PY = join(dirname(realpathSync(fileURLToPath(import.meta.url))), "..", "..", "scripts", "hook.py");
const PORTFILE = join(tmpdir(), "clawd-mood.port");
// One creature per process: child sessions stay folded into their parent.
const IDENTITY = { session: `oc${process.pid}`.slice(0, 8), name: basename(process.cwd()).slice(0, 16), src: "OC" };

function send(state, event) {
  let port;
  try {
    port = Number(readFileSync(PORTFILE, "utf8").trim());
  } catch {
    return; // daemon still warming up; next event will catch up
  }
  if (!Number.isInteger(port)) return;
  const socket = connect(port, "127.0.0.1");
  socket.on("error", () => {}); // daemon not responding; silently drop
  socket.setTimeout(300, () => socket.destroy());
  socket.end(JSON.stringify({ state, event, tool: "", ...IDENTITY }) + "\n");
}

function startDaemon() {
  const child = spawn("uv", ["run", HOOK_PY], { stdio: ["pipe", "ignore", "ignore"], detached: true, windowsHide: true });
  child.on("error", () => {}); // uv missing must never break the agent
  child.stdin.on("error", () => {});
  // source=compact: probe/spawn only. uv takes ~100ms, so a state pushed from
  // hook.py could land after (and overwrite) the first real state.
  child.stdin.end(JSON.stringify({ hook_event_name: "SessionStart", source: "compact" }));
  child.unref();
}

const END_STATES = {
  "session.execution.succeeded": "done",
  "session.execution.failed": "error",
  "session.execution.interrupted": "idle",
};

export default {
  id: "clawd-mood",
  async setup(ctx) {
    // Subagents are child sessions in-process; their end must not read as done.
    const children = new Set();
    const running = new Set();

    const onEvent = ({ type, data }) => {
      const session = data?.sessionID;
      switch (type) {
        case "session.created":
          if (data.parentID) children.add(session);
          break;
        case "session.deleted":
          children.delete(session);
          break;
        case "session.execution.started":
          if (children.has(session)) break;
          running.add(session);
          send("thinking", type);
          break;
        case "session.execution.succeeded":
        case "session.execution.failed":
        case "session.execution.interrupted":
          if (children.has(session)) break;
          running.delete(session);
          send(END_STATES[type], type);
          break;
        case "session.tool.called":
        case "session.tool.success":
        case "permission.replied":
        case "form.replied":
        case "form.cancelled":
          send("working", type);
          break;
        case "session.tool.failed":
          send("error", type);
          break;
        case "permission.asked":
        case "form.created":
          send("waiting", type);
          break;
        case "session.compaction.started":
          send("thinking", type);
          break;
        case "session.compaction.ended":
        case "session.compaction.failed":
          // A manual /compact runs with no turn in flight, so "working" would stick.
          send(running.size > 0 ? "working" : "idle", type);
          break;
      }
    };

    const controller = new AbortController();
    void (async () => {
      for await (const event of ctx.event.subscribe({ signal: controller.signal })) onEvent(event);
    })().catch(() => {}); // aborted on unload

    // Resumed sessions never emit session.created, so start the daemon on load.
    startDaemon();
    send("idle", "plugin.setup");
    return () => controller.abort();
  },
};
