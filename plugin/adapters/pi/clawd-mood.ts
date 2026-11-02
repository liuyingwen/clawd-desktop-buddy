// clawd-mood extension for pi — pushes states straight to the daemon over TCP (no
// uv process per event, so order holds and the last state survives a fast exit).
// hook.py is only used to autostart the daemon.

import { spawn } from "node:child_process";
import { readFileSync, realpathSync } from "node:fs";
import { connect } from "node:net";
import { tmpdir } from "node:os";
import { basename, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const HOOK_PY = join(dirname(realpathSync(fileURLToPath(import.meta.url))), "..", "..", "scripts", "hook.py");
const PORTFILE = join(tmpdir(), "clawd-mood.port");
let identity: Record<string, string> = { session: `pi${process.pid}`.slice(0, 8), name: basename(process.cwd()).slice(0, 16), src: "PI" };

function send(state: string, event: string, tool = "", extra: object = {}): Promise<void> {
  return new Promise((resolve) => {
    let port: number;
    try {
      port = Number(readFileSync(PORTFILE, "utf8").trim());
    } catch {
      return resolve(); // daemon still warming up; next event will catch up
    }
    if (!Number.isInteger(port)) return resolve();
    const socket = connect(port, "127.0.0.1");
    socket.on("error", () => {}); // daemon not responding; silently drop
    socket.on("close", () => resolve());
    socket.setTimeout(300, () => socket.destroy());
    socket.end(JSON.stringify({ state, event, tool, ...identity, ...extra }) + "\n");
  });
}

function startDaemon(): void {
  const child = spawn("uv", ["run", HOOK_PY], { stdio: ["pipe", "ignore", "ignore"], detached: true, windowsHide: true });
  child.on("error", () => {}); // uv missing must never break the agent
  child.stdin.on("error", () => {});
  // source=compact: probe/spawn only. uv takes ~100ms, so a state pushed from
  // hook.py could land after (and overwrite) the first real state.
  child.stdin.end(JSON.stringify({ hook_event_name: "SessionStart", source: "compact" }));
  child.unref();
}

function follower(pi: any, owner: string): void {
  identity = { ...identity, session: `pi${owner}`.slice(0, 8), agent: String(process.pid).slice(-6) };
  const leave = (event: string) => send("done", event, "", { gone: true });
  pi.on("session_start", (event: any) => {
    if (event.reason !== "reload") send("thinking", "session_start");
  });
  pi.on("before_agent_start", () => send("thinking", "before_agent_start"));
  pi.on("tool_execution_start", (event: any) => send("working", "tool_execution_start", event.toolName));
  pi.on("tool_execution_end", (event: any) => send("working", "tool_execution_end", event.toolName));
  pi.on("ui_prompt_start", () => send("waiting", "ui_prompt_start"));
  pi.on("ui_prompt_end", () => send("working", "ui_prompt_end"));
  pi.on("agent_settled", () => leave("agent_settled"));
  pi.on("session_shutdown", async (event: any) => {
    if (event.reason !== "reload") await leave("session_shutdown");
  });
}

export default function (pi: any): void {
  // Subagents are pi child processes that load this extension too; only the
  // top-level process drives the main creature, the rest show as its followers.
  const owner = process.env.CLAWD_MOOD_PI_OWNER;
  if (owner && owner !== String(process.pid)) return follower(pi, owner);
  process.env.CLAWD_MOOD_PI_OWNER = String(process.pid);

  let running = false;
  let failed = false;

  pi.on("session_start", (event: any) => {
    if (event.reason === "reload") return;
    startDaemon();
    send("idle", "session_start");
  });
  pi.on("before_agent_start", () => {
    running = true;
    failed = false;
    send("thinking", "before_agent_start");
  });
  pi.on("tool_execution_start", (event: any) => {
    send("working", "tool_execution_start", event.toolName);
  });
  pi.on("tool_execution_end", (event: any) => {
    // A failed tool (e.g. `diff` exiting 1) doesn't stop the agent.
    send("working", "tool_execution_end", event.toolName);
  });
  pi.on("message_end", (event: any) => {
    if (event.message?.role === "assistant") failed = event.message.stopReason === "error";
  });
  pi.on("ui_prompt_start", () => {
    if (running) send("waiting", "ui_prompt_start");
  });
  pi.on("ui_prompt_end", () => {
    if (running) send("working", "ui_prompt_end");
  });
  pi.on("session_before_compact", () => {
    send("thinking", "session_before_compact");
  });
  // A manual /compact runs with no turn in flight, so "working" would stick.
  const afterCompact = (event: any) => {
    send(running ? "working" : "idle", event.type);
  };
  pi.on("session_compact", afterCompact);
  pi.on("session_compact_failed", afterCompact);
  // agent_end is not final (retries, compaction, queued work); agent_settled is.
  pi.on("agent_settled", (event: any) => {
    running = false;
    send(event.aborted ? "idle" : failed ? "error" : "done", "agent_settled");
  });
  pi.on("session_shutdown", async (event: any) => {
    if (event.reason === "quit") await send("idle", "session_shutdown", "", { end: true });
  });
}
