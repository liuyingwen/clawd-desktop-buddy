// clawd-mood plugin for DeepSeek Harness — pushes states straight to the daemon
// over TCP from inside the host. Unlike the hooks-dsh.json bridge it sees
// approvals, questions and errors, and it ignores the sessions dsh restores at
// startup. hook.py is only used to autostart the daemon.

import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, readFileSync, realpathSync } from "node:fs";
import { connect } from "node:net";
import { homedir, tmpdir } from "node:os";
import { basename, delimiter, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(realpathSync(fileURLToPath(import.meta.url)));
// Packed tarballs carry their own copy; a linked checkout uses the repo's
const HOOK_PY = [join(HERE, "scripts", "hook.py"), join(HERE, "..", "..", "scripts", "hook.py")].find(existsSync);
const PORTFILE = join(tmpdir(), "clawd-mood.port");
// The desktop app starts from Finder with a bare PATH, so uv must be findable
const PATH = [process.env.PATH, join(homedir(), ".local", "bin"), "/opt/homebrew/bin", "/usr/local/bin"]
  .filter(Boolean).join(delimiter);
// Approvals answered by policy return at once; only a real wait shows on screen
const WAIT_DELAY_MS = 400;

export const name = "clawd-mood";

const sha1 = (text) => createHash("sha1").update(text).digest("hex");
const isSubagent = (agent) => agent.session.header.origin === "subagent";
// Subagents can nest, but all of them follow the top-level session's creature
const roots = new Map();   // subagent session id -> top-level session id

function identity(agent) {
  const header = agent.session.header;
  const cwd = header.cwd ?? process.cwd();
  const fields = { name: basename(cwd).slice(0, 16), src: "DS" };
  if (!isSubagent(agent)) return { ...fields, session: sha1(header.id).slice(0, 8) };
  const root = roots.get(header.parentSession) ?? header.parentSession;
  roots.set(header.id, root);
  return { ...fields, session: sha1(root).slice(0, 8), agent: sha1(header.id).slice(0, 6) };
}

function send(agent, state, event, extra = {}) {
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
  socket.end(JSON.stringify({ state, event, tool: "", ...identity(agent), ...extra }) + "\n");
}

function startDaemon() {
  if (!HOOK_PY) return;
  const child = spawn("uv", ["run", HOOK_PY], {
    stdio: ["pipe", "ignore", "ignore"], detached: true, windowsHide: true, env: { ...process.env, PATH },
  });
  child.on("error", () => {}); // uv missing must never break the agent
  child.stdin.on("error", () => {});
  // source=compact: probe/spawn only, no state of its own
  child.stdin.end(JSON.stringify({ hook_event_name: "SessionStart", source: "compact" }));
  child.unref();
}

export function apply(ctx) {
  const last = new Map();   // agent -> last state sent

  // Subagents run as their own agents; they show as followers, and any end removes them
  const push = (agent, state, event, extra) => {
    if (agent === undefined) return;
    last.set(agent, state);
    if (isSubagent(agent) && (state === "done" || state === "error" || state === "idle")) {
      send(agent, "done", event, { ...extra, gone: true });
    } else {
      send(agent, state, event, extra);
    }
  };
  // Show waiting only while the answer is actually pending, then resume work
  const waitFor = async (agent, event, tool, next) => {
    const timer = setTimeout(() => push(agent, "waiting", event, { tool }), WAIT_DELAY_MS);
    try {
      return await next();
    } finally {
      clearTimeout(timer);
      if (last.get(agent) === "waiting") push(agent, "working", event, { tool });
    }
  };

  // agent/created also fires for every session restored at startup: stay off screen until used
  ctx.on("agent/pre-step", ({ agent, messages }, next) => {
    if (messages.length > 0) push(agent, "thinking", "agent/pre-step");
    return next();
  });
  ctx.on("tools/pre-execute", (exec, next) => {
    push(exec.agent, "working", "tools/pre-execute", { tool: exec.name.slice(0, 32) });
    return next();
  });
  ctx.on("approval/request", (req, next) => waitFor(req.agent, "approval/request", req.toolName.slice(0, 32), next));
  ctx.on("user-questions/request", (req, next) => waitFor(req.agent, "user-questions/request", "", next));
  ctx.on("agent/turn-stopping", ({ agent }) => push(agent, "done", "agent/turn-stopping"));
  ctx.on("agent/error", ({ agent }) => push(agent, "error", "agent/error"));
  ctx.on("agent/status", ({ agent, status }) => {
    // A cancelled turn skips turn-stopping; don't stay stuck mid-work
    const state = last.get(agent);
    if (status === "idle" && (state === "thinking" || state === "working")) push(agent, "idle", "agent/status");
  });
  ctx.on("agent/disposed", ({ agent }) => {
    if (!last.delete(agent)) return;
    if (isSubagent(agent)) send(agent, "done", "agent/disposed", { gone: true });
    else send(agent, "idle", "agent/disposed", { end: true });
    roots.delete(agent.session.header.id);
  });

  startDaemon();
}
