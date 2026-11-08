// MiqiPet plugin for opencode >= 2.0 — pushes states straight to the daemon over
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
const PORTFILE = join(tmpdir(), "miqipet.port");
// One creature per process; child sessions show as followers above it.
const IDENTITY = { session: `oc${process.pid}`.slice(0, 8), name: basename(process.cwd()).slice(0, 16), src: "OC" };

function send(state, event, extra = {}) {
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
  socket.end(JSON.stringify({ state, event, tool: "", ...IDENTITY, ...extra }) + "\n");
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

// Child session events -> follower state; done also removes the follower
const CHILD_STATES = {
  "session.execution.started": "thinking",
  "session.execution.succeeded": "done",
  "session.execution.failed": "done",
  "session.execution.interrupted": "done",
  "session.deleted": "done",
  "session.tool.called": "working",
  "session.tool.success": "working",
  "session.tool.failed": "working",
  "permission.asked": "waiting",
  "permission.replied": "working",
  "form.created": "waiting",
  "form.replied": "working",
  "form.cancelled": "working",
  "session.compaction.started": "thinking",
  "session.compaction.ended": "working",
  "session.compaction.failed": "working",
};

function sendChild(session, type) {
  const state = CHILD_STATES[type];
  if (state) send(state, type, { agent: session.slice(-6), ...(state === "done" ? { gone: true } : {}) });
}

export default {
  id: "miqipet",
  async setup(ctx) {
    // Subagents are child sessions in-process; they must not drive the main creature.
    const children = new Set();
    const running = new Set();

    const onEvent = ({ type, data }) => {
      const session = data?.sessionID;
      if (children.has(session)) {
        sendChild(session, type);
        if (type === "session.deleted") children.delete(session);
        return;
      }
      switch (type) {
        case "session.created":
          if (data.parentID) {
            children.add(session);
            sendChild(session, "session.execution.started");
          }
          break;
        case "session.execution.started":
          running.add(session);
          send("thinking", type);
          break;
        case "session.execution.succeeded":
        case "session.execution.failed":
        case "session.execution.interrupted":
          running.delete(session);
          send(END_STATES[type], type);
          break;
        case "session.tool.called":
        case "session.tool.success":
        case "session.tool.failed": // e.g. `diff` exiting 1; the agent carries on
        case "permission.replied":
        case "form.replied":
        case "form.cancelled":
          send("working", type);
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
