#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["pyserial>=3.5"]
# ///
"""clawd-mood config page — a local server for page/ that talks to the daemon.

    uv run page/serve.py [--no-open] [--port N]
    uv run page/serve.py --set theme=2 brightness=60   # or --resend, --test, --status: one action, no page

Binds 127.0.0.1 only. A 256-bit token travels in the URL fragment (never sent to the server or
written to a log) and every /api/* call must return it in X-Clawd-Token. Host and Origin are
checked on every request so another site cannot reach this server by DNS rebinding.

The page can only see the chain up to the serial port: the protocol is one-way, so the device
never confirms anything. "sent" means the daemon wrote the line to its serial port (its log says
so) or at least a device is present when the log cannot tell, nothing more.

Exits after 10 minutes without a user action (status polling does not count) and after 1 hour
in any case.
"""

import argparse
import hmac
import json
import os
import re
import secrets
import shutil
import signal
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

from serial.tools import list_ports

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DAEMON = ROOT / "plugin" / "scripts" / "daemon.py"
PORTFILE = Path(tempfile.gettempdir()) / "clawd-mood.port"
LOGFILE = Path(tempfile.gettempdir()) / "clawd-mood-daemon.log"

IDLE_LIMIT_S = 10 * 60
HARD_LIMIT_S = 60 * 60
HANDLER_TIMEOUT_S = 10
MAX_BODY_BYTES = 4096        # bodies must be under this
MAX_LINE_BYTES = 319         # the firmware's LINE_MAX minus the newline
DAEMON_START_WAIT_S = 6.0
DAEMON_STOP_WAIT_S = 5.0
SEND_EVIDENCE_WAIT_S = 5.0   # reopening a replugged port blocks the daemon for about 3.5 s
TEST_DONE_AFTER_S = 2.0
TEST_END_AFTER_S = 3.0
LOG_TAIL_BYTES = 64 * 1024
LOG_ERROR_LINES = 8

ESPRESSIF_VID = "0x303A"
CFG_DEFAULTS = {"theme": 1, "rotation": 1, "brightness": 100}
CFG_KEYS = tuple(CFG_DEFAULTS)
SECURITY_HEADERS = {
    "Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
                               "connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'",
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "no-referrer",
}
CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".png": "image/png",
}


def log(message: str) -> None:
    print(f"[serve] {message}", file=sys.stderr, flush=True)


# ── Config file ──────────────────────────────────────────────────

def real_home() -> Path:
    """The account's home, not $HOME, which can be replaced (the plugin sandboxes do)."""
    if os.name == "posix":
        import pwd
        return Path(pwd.getpwuid(os.getuid()).pw_dir)
    return Path.home()


def config_path() -> Path:
    override = os.environ.get("CLAWD_MOOD_CONFIG")
    return Path(override) if override else real_home() / ".clawd-mood" / "config.json"


def valid_cfg_value(key: str, value) -> bool:
    if not isinstance(value, int) or isinstance(value, bool):
        return False
    if key == "theme":
        return 1 <= value <= 255
    if key == "rotation":
        return 0 <= value <= 3
    return 10 <= value <= 100


def read_config() -> dict:
    """Missing or corrupt means defaults. Unknown keys are kept for the next write."""
    raw: dict = {}
    path = config_path()
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(loaded, dict):
            raw = loaded
        else:
            log(f"config is not an object, using defaults: {path}")
    except FileNotFoundError:
        pass
    except (OSError, ValueError) as e:
        log(f"config unreadable, using defaults: {path}: {e}")
    port = raw.get("serial_port")
    last = raw.get("last_sent") if isinstance(raw.get("last_sent"), dict) else {}
    unsent = raw.get("unsent") if isinstance(raw.get("unsent"), list) else []
    return {
        **raw,
        "serial_port": port if isinstance(port, str) and port else None,
        "last_sent": {k: last[k] if valid_cfg_value(k, last.get(k)) else CFG_DEFAULTS[k] for k in CFG_KEYS},
        "unsent": [k for k in CFG_KEYS if k in unsent],
    }


def write_config(config: dict) -> None:
    path = config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(config, f, ensure_ascii=False, indent=2)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


# ── Daemon: discovery, sending, start, restart ───────────────────

def read_portfile() -> int | None:
    try:
        port = int(PORTFILE.read_text().strip())
    except (OSError, ValueError):
        return None
    return port if 0 < port < 65536 else None


def probe_daemon() -> int | None:
    """The daemon's TCP port if something answers there, same as hook.py's probe."""
    port = read_portfile()
    if port is None:
        return None
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            return port
    except OSError:
        return None


def send_line(port: int, line: str) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1.0) as conn:
            conn.sendall((line + "\n").encode("utf-8"))
        return True
    except OSError:
        return False


def log_size() -> int:
    try:
        return LOGFILE.stat().st_size
    except OSError:
        return 0


def read_log_from(offset: int) -> str:
    try:
        with open(LOGFILE, "rb") as f:
            f.seek(offset)
            return f.read().decode("utf-8", errors="replace")
    except OSError:
        return ""


def log_tail_lines() -> list[str]:
    return read_log_from(max(0, log_size() - LOG_TAIL_BYTES)).splitlines()


def recent_log_errors() -> list[str]:
    errors = []
    for line in log_tail_lines():
        text = line.strip()
        if text.startswith("->"):   # forwarded lines; a state named "error" is not one
            continue
        if re.search(r"!!|No ESP32|Traceback|Exception|Error\b|warning:", text):
            errors.append(text[:200])
    return errors[-LOG_ERROR_LINES:]


def log_serial_port() -> str | None:
    """The port the most recent daemon logged. Absent when it was started by hand."""
    for line in reversed(log_tail_lines()):
        match = re.match(r"\s*Serial:\s*(.+?)(?: is back,.*)?$", line)
        if match:
            return match.group(1).strip()
    return None


def serial_link_down() -> bool:
    """The daemon survives an unplug and drops lines without logging each one, so its last word
    on the link is all there is: an error not followed by Ready! or "is back"."""
    for line in reversed(log_tail_lines()):
        if "!! serial error" in line:
            return True
        if "Ready!" in line or " is back," in line:
            return False
    return False


EXPLAIN = {
    "uv_missing": "第一处断点：找不到 uv，后台启动不了。先安装 uv（macOS: brew install uv），再重新运行本页。",
    "no_device": "第一处断点：USB。没有找到设备，后台已退出。插上 ESP32-C3，或在串口设备里指定端口。",
    "serial_open_failed": "第一处断点：USB。后台打不开串口，端口可能已拔出或被其他程序占用（比如串口监视器）。",
    "device_gone": "第一处断点：USB。后台在运行，但串口写不进去，设备可能已拔出。插回去后再发一次。",
    "daemon_failed": "第一处断点：后台。后台启动后退出了，详情见诊断信息里的日志。",
    "daemon_slow": "第一处断点：后台。后台 6 秒内没有就绪，可能还在安装依赖。稍等片刻再试一次。",
    "send_failed": "第一处断点：后台。连不上后台的 TCP 端口。",
    "old_stuck": "第一处断点：后台。旧的后台进程没有退出，新设置没有用上。",
    "old_unknown": "第一处断点：后台。找不到旧的后台进程，没法重启它，请手动重启。",
    "not_daemon": "第一处断点：后台。端口文件指向的进程不是 clawd-mood 后台，可能是残留的端口文件。",
}

# The script itself, wherever the plugin was copied to, not any file whose name ends in daemon.py.
# Strict form for signalling. The loose form also accepts a daemon started by hand from inside
# plugin/scripts (argv "daemon.py"); it only decides whether a send counts, never what gets killed.
DAEMON_COMMAND = re.compile(r"(^|[\s/])scripts/daemon\.py(\s|$)")
DAEMON_COMMAND_LOOSE = re.compile(r"(^|[\s/])daemon\.py(\s|$)")


class Daemon:
    """One start or restart at a time; a held lock also keeps sends from racing a restart."""

    def __init__(self):
        self.lock = threading.Lock()
        self.children: list[subprocess.Popen] = []

    def reap(self) -> None:
        self.children = [p for p in self.children if p.poll() is None]

    def spawn(self) -> subprocess.Popen:
        """Same detached start as hook.py. daemon.py reads serial_port from the config file itself."""
        with open(LOGFILE, "a") as log_fh:
            if sys.platform == "win32":
                CREATE_NO_WINDOW = 0x08000000
                DETACHED_PROCESS = 0x00000008
                proc = subprocess.Popen(
                    ["uv", "run", str(DAEMON)],
                    stdout=log_fh, stderr=log_fh, stdin=subprocess.DEVNULL,
                    creationflags=DETACHED_PROCESS | CREATE_NO_WINDOW, close_fds=True,
                )
            else:
                proc = subprocess.Popen(
                    [str(DAEMON)],
                    stdout=log_fh, stderr=log_fh, stdin=subprocess.DEVNULL,
                    start_new_session=True, close_fds=True,
                )
        self.children.append(proc)
        return proc

    def start(self) -> tuple[int | None, str | None]:
        """Returns (port, None) once the daemon logged Ready, else (None, reason).

        The daemon writes its portfile before it looks for a device, so a portfile alone proves
        nothing: it exits right after when there is no device. Ready! is printed after the
        serial port opened.
        """
        if not shutil.which("uv"):
            return None, "uv_missing"
        offset = log_size()
        try:
            proc = self.spawn()
        except OSError as e:
            log(f"daemon spawn failed: {e}")
            return None, "daemon_failed"
        deadline = time.monotonic() + DAEMON_START_WAIT_S
        while time.monotonic() < deadline:
            code = proc.poll()
            output = read_log_from(offset)
            if code is not None:
                port = probe_daemon()  # it lost the singleton check to a daemon a hook just started
                if port is not None:
                    return port, None
                if "No ESP32" in output:
                    return None, "no_device"
                if "Cannot open" in output:
                    return None, "serial_open_failed"
                return None, "daemon_failed"
            if "Ready!" in output:
                port = probe_daemon()
                if port is not None:
                    return port, None
            time.sleep(0.1)
        return None, "daemon_slow"

    def ensure(self) -> tuple[int | None, str | None, bool]:
        """(port, reason, started). Caller holds the lock. Starts the daemon at most once."""
        port = probe_daemon()
        if port is not None:
            return port, None, False
        port, reason = self.start()
        return port, reason, port is not None

    def deliver(self, line: str) -> tuple[bool, str | None, bool]:
        """(sent, reason, started). The daemon accepts lines even with the board unplugged, so a
        TCP send proves nothing; its log says whether the line reached the serial port."""
        with self.lock:
            port, reason, started = self.ensure()
            if port is None:
                return False, reason, False
            offset = log_size()
            if not send_line(port, line):
                return False, "send_failed", started
            deadline = time.monotonic() + SEND_EVIDENCE_WAIT_S
            while time.monotonic() < deadline:
                output = read_log_from(offset)
                if f"-> {line}" in output:
                    return True, None, started
                # A write error is followed by one reopen, which succeeds when the board is back
                if "!! serial error" in output and not device_expected(serial_candidates(), read_config()["serial_port"]):
                    return False, "device_gone", started
                time.sleep(0.05)
            if probe_daemon() is None:
                return False, "send_failed", started
            # Nothing logged: a dropped line, or a daemon started by hand that logs elsewhere.
            # A stale portfile can also point at some other program, which would take any line.
            if self.find_pids(port, DAEMON_COMMAND_LOOSE) == []:
                return False, "not_daemon", started
            candidates = serial_candidates()
            if serial_link_down() or not device_expected(candidates, read_config()["serial_port"]):
                return False, "device_gone", started
            return True, None, started

    def find_pids(self, port: int, pattern: re.Pattern = DAEMON_COMMAND) -> list[int] | None:
        """The process listening on the daemon's port, only if its command line matches pattern.

        Asked by port on purpose: matching on the command line could hit a daemon that belongs to
        another temp dir. None means no lsof to ask: no restart, and the page says so.
        """
        lsof = shutil.which("lsof") or next((p for p in ("/usr/sbin/lsof", "/usr/bin/lsof") if os.path.exists(p)), None)
        if lsof is None:
            return None
        confirmed = []
        try:
            out = subprocess.run([lsof, "-nP", f"-iTCP:{port}", "-sTCP:LISTEN", "-t"],
                                 capture_output=True, text=True, timeout=5).stdout
            for pid in (int(p) for p in out.split()):
                command = subprocess.run(["ps", "-o", "command=", "-p", str(pid)],
                                         capture_output=True, text=True, timeout=5).stdout
                if pattern.search(command.strip()):
                    confirmed.append(pid)
        except (OSError, subprocess.SubprocessError, ValueError) as e:
            log(f"could not look up the daemon's process: {e}")
        return confirmed

    def stop(self, port: int) -> str | None:
        """Signal, then wait for the process to exit and the portfile to vanish. Returns a reason on failure."""
        pids = self.find_pids(port)
        if not pids:
            return "old_unknown"
        for pid in pids:
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            except OSError as e:
                log(f"could not signal daemon {pid}: {e}")
                return "old_stuck"
        deadline = time.monotonic() + DAEMON_STOP_WAIT_S
        while time.monotonic() < deadline:
            self.reap()  # a daemon we started stays a zombie, and "alive" to kill(0), until reaped
            alive = False
            for pid in pids:
                try:
                    os.kill(pid, 0)
                    alive = True
                except ProcessLookupError:
                    pass
                except OSError:
                    alive = True
            if not alive and not PORTFILE.exists():
                return None
            if not alive:
                PORTFILE.unlink(missing_ok=True)  # killed harder than atexit survives
                return None
            time.sleep(0.05)
        return "old_stuck"

    def restart(self) -> tuple[int | None, str | None]:
        with self.lock:
            port = probe_daemon()
            if port is not None:
                reason = self.stop(port)
                if reason:
                    return None, reason
            else:
                PORTFILE.unlink(missing_ok=True)
            return self.start()


# ── Status ───────────────────────────────────────────────────────

def plugin_traces() -> dict:
    """Best effort from CLI-internal files; "unknown" when they cannot tell."""
    home = real_home()

    def cache_trace(root: Path) -> dict:
        try:
            cache = root / "plugins" / "cache" / "clawd-mood"
            if cache.is_dir():
                return {"state": "found", "detail": str(cache)}
            return {"state": "absent" if root.is_dir() else "unknown", "detail": str(cache)}
        except OSError as e:
            return {"state": "unknown", "detail": str(e)}

    listed = home / ".claude" / "plugins" / "installed_plugins.json"
    try:
        found = "clawd-mood" in listed.read_text(encoding="utf-8", errors="replace")
        # --plugin-dir leaves no trace, so not being listed does not mean not installed
        claude = {"state": "found" if found else "unknown", "detail": str(listed)}
    except OSError:
        claude = {"state": "unknown", "detail": str(listed)}
    hooks = home / ".cursor" / "hooks.json"   # written by tools/cursor_hooks.py
    try:
        text = hooks.read_text(encoding="utf-8", errors="replace")
        cursor = {"state": "found" if re.search(r"hook\.py\W{0,3} cursor\b", text) else "absent", "detail": str(hooks)}
    except FileNotFoundError:
        cursor = {"state": "absent" if hooks.parent.is_dir() else "unknown", "detail": str(hooks)}
    except OSError:
        cursor = {"state": "unknown", "detail": str(hooks)}
    return {
        "claude_code": claude,
        "codex": cache_trace(home / ".codex"),
        "workbuddy": cache_trace(home / ".workbuddy"),
        "cursor": cursor,
    }


def likely_device(device: str, vid) -> bool:
    """Same filter as daemon.py's is_candidate, so auto means the same thing here."""
    if sys.platform == "darwin":
        return device.startswith("/dev/cu.usbmodem")
    if sys.platform == "linux":
        return device.startswith(("/dev/ttyACM", "/dev/ttyUSB"))
    return device.upper().startswith("COM") and vid is not None


def serial_candidates() -> list[dict]:
    """Enumeration only: the ports are never opened, opening can reset the board."""
    try:
        ports = list_ports.comports()
    except OSError as e:
        log(f"comports failed: {e}")
        return []
    found = [{
        "device": p.device,
        "description": p.description if p.description and p.description != "n/a" else None,
        "manufacturer": p.manufacturer,
        "vid": f"0x{p.vid:04X}" if p.vid is not None else None,
        "pid": f"0x{p.pid:04X}" if p.pid is not None else None,
        "serial_number": p.serial_number,
        "likely": likely_device(p.device, p.vid),
    } for p in ports]
    # daemon.py prefers a native-USB ESP32 over any other candidate
    found.sort(key=lambda c: (not c["likely"], c["vid"] != ESPRESSIF_VID, c["device"]))
    return found


def env_port_present(candidates: list[dict]) -> bool:
    """CLAWD_MOOD_PORT is used as given, so it only counts when that port exists."""
    value = os.environ.get("CLAWD_MOOD_PORT")
    if not value:
        return False
    if sys.platform == "win32":
        return any(c["device"].upper() == value.upper() for c in candidates)
    return os.path.exists(value)


def device_expected(candidates: list[dict], configured: str | None) -> bool:
    """Would daemon.py's detect_port find something: env override, the saved port, or a scan."""
    return env_port_present(candidates) or any(
        c["likely"] or c["device"] == configured for c in candidates)


def first_break(uv_path, daemon_port, usb_ok, link_down) -> dict | None:
    """First link in page -> daemon -> USB that looks broken right now."""
    if daemon_port is None and not uv_path:
        return {"link": "uv", "explain": EXPLAIN["uv_missing"]}
    if not usb_ok or (daemon_port is not None and link_down):
        return {"link": "usb", "explain": "第一处断点：USB。没有找到设备，插上 ESP32-C3，或在串口设备里指定端口。"}
    if daemon_port is None:
        return {"link": "daemon", "explain": "第一处断点：后台没在运行。有改动时按「应用到屏幕」，或按「发送测试小人」，会尝试启动一次。"}
    return None


def build_status(life: "Lifetime") -> dict:
    config = read_config()
    uv_path = shutil.which("uv")
    portfile_port = read_portfile()
    port = probe_daemon()
    candidates = serial_candidates()
    configured = config["serial_port"]
    usb_ok = device_expected(candidates, configured)
    link_down = port is not None and serial_link_down()
    return {
        "platform": sys.platform,
        "plugins": plugin_traces(),
        "daemon": {
            "alive": port is not None,
            "port": port,
            "portfile": str(PORTFILE),
            "portfile_port": portfile_port,
            "serial": log_serial_port() if port is not None else None,
            "link_down": link_down,
        },
        "serial": {
            "candidates": candidates,
            "configured": configured,
            "env_override": os.environ.get("CLAWD_MOOD_PORT") or None,
            "usb_ok": usb_ok,
        },
        "log": {"path": str(LOGFILE), "errors": recent_log_errors()},
        "uv": {"on_path": uv_path is not None, "path": uv_path},
        "first_break": first_break(uv_path, port, usb_ok, link_down),
        "config_path": str(config_path()),
        "last_sent": config["last_sent"],
        "unsent": config["unsent"],
        "lifetime": life.remaining(),
    }


# ── Lifetime ─────────────────────────────────────────────────────

class Lifetime:
    def __init__(self):
        self.started = time.monotonic()
        self.last_action = self.started
        self.reason: str | None = None

    def touch(self) -> None:
        self.last_action = time.monotonic()

    def remaining(self) -> dict:
        now = time.monotonic()
        return {
            "idle_left_s": max(0, round(IDLE_LIMIT_S - (now - self.last_action))),
            "hard_left_s": max(0, round(HARD_LIMIT_S - (now - self.started))),
        }

    def expired(self) -> str | None:
        now = time.monotonic()
        if now - self.started >= HARD_LIMIT_S:
            return "hard limit"
        if now - self.last_action >= IDLE_LIMIT_S:
            return "idle"
        return None


# ── App: what the API does, apart from HTTP ──────────────────────

class ApiError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status
        self.message = message


class App:
    def __init__(self, token: str):
        self.token = token
        self.life = Lifetime()
        self.daemon = Daemon()
        self.config_lock = threading.Lock()
        self.test_lock = threading.Lock()
        self.test_thread: threading.Thread | None = None
        self.theme_ids = self.load_theme_ids()
        self.static = self.build_allowlist()

    @staticmethod
    def load_theme_ids() -> set[int]:
        try:
            manifest = json.loads((HERE / "previews" / "manifest.json").read_text(encoding="utf-8"))
            return {int(t["id"]) for t in manifest["themes"]}
        except (OSError, ValueError, KeyError, TypeError) as e:
            log(f"manifest unreadable, allowing the built-in themes: {e}")
            return {1, 2}

    @staticmethod
    def build_allowlist() -> dict[str, Path]:
        """Exact URL paths only, decided at startup. Requests never reach the filesystem otherwise."""
        table = {"/": HERE / "index.html"}
        for name in ("index.html", "page.css", "page.js"):
            table[f"/{name}"] = HERE / name
        for folder in ("assets", "previews"):
            for dirpath, dirnames, filenames in os.walk(HERE / folder, followlinks=False):
                dirnames[:] = [d for d in dirnames if not d.startswith(".")]
                for filename in filenames:
                    path = Path(dirpath) / filename
                    if path.suffix in CONTENT_TYPES and not path.is_symlink() and not filename.startswith("."):
                        table["/" + path.relative_to(HERE).as_posix()] = path
        return table

    def api_cfg(self, body: dict) -> tuple[int, dict]:
        force = body.get("force", False)
        if not isinstance(force, bool):
            raise ApiError(400, "force must be a boolean")
        unknown = set(body) - set(CFG_KEYS) - {"force"}
        if unknown:
            raise ApiError(400, f"unknown fields: {sorted(unknown)}")
        requested = {k: body[k] for k in CFG_KEYS if k in body}
        if not requested:
            raise ApiError(400, "nothing to send")
        for key, value in requested.items():
            if not valid_cfg_value(key, value):
                raise ApiError(400, f"bad value for {key}")
        if "theme" in requested and requested["theme"] not in self.theme_ids:
            raise ApiError(400, "unknown theme")

        with self.config_lock:
            config = read_config()
            last = config["last_sent"]
            merged = {**last, **requested}
            # Fields saved while the device was away still have to reach it
            changed = {k: merged[k] for k in CFG_KEYS
                       if k in config["unsent"] or (k in requested and (force or requested[k] != last[k]))}
            if not changed:
                return 200, {"result": "no_change", "last_sent": last, "unsent": []}
            line = json.dumps({"cfg": changed}, ensure_ascii=False, separators=(",", ":"))
            if len(line.encode("utf-8")) > MAX_LINE_BYTES:
                raise ApiError(400, "line too long")
            sent, reason, started = self.daemon.deliver(line)
            config["last_sent"] = merged
            config["unsent"] = [] if sent else [k for k in CFG_KEYS if k in changed]
            try:
                write_config(config)
            except OSError as e:
                log(f"config write failed: {e}")
                raise ApiError(500, f"配置文件写不进去：{e}")
        reply = {"line": line, "changed": changed, "last_sent": merged, "unsent": config["unsent"],
                 "daemon_started": started}
        if sent:
            return 200, {"result": "sent", **reply}
        return 200, {"result": "saved_not_sent", "reason": reason, "explain": EXPLAIN.get(reason, ""), **reply}

    def api_test(self) -> tuple[int, dict]:
        if not self.test_lock.acquire(blocking=False):
            return 200, {"result": "busy"}
        started_thread = False
        try:
            fields = {"session": "~test", "name": "测试小人", "src": "CC", "quiet": True}
            first = json.dumps({"state": "working", **fields}, ensure_ascii=False, separators=(",", ":"))
            sent, reason, started = self.daemon.deliver(first)
            if not sent:
                return 200, {"result": "not_sent", "reason": reason, "explain": EXPLAIN.get(reason, "")}
            self.test_thread = threading.Thread(target=self.finish_test, args=(fields,), name="test-creature")
            self.test_thread.start()
            started_thread = True
            return 200, {"result": "sent", "line": first, "daemon_started": started}
        finally:
            if not started_thread:
                self.test_lock.release()

    def finish_test(self, fields: dict) -> None:
        """Ordinary state lines, so the creature also works on firmware that ignores cfg."""
        try:
            time.sleep(TEST_DONE_AFTER_S)
            self.send_test_line({"state": "done", **fields})
            time.sleep(TEST_END_AFTER_S)
            self.send_test_line({"state": "idle", **fields, "end": True})
        finally:
            self.test_lock.release()

    def send_test_line(self, obj: dict) -> None:
        """Under the daemon lock, so a restart from 保存串口 cannot fall between probe and send."""
        line = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
        with self.daemon.lock:
            port = probe_daemon()
            sent = port is not None and send_line(port, line)
        if not sent:
            log(f"test creature line not sent: {line}")  # the firmware expires it after 30 minutes

    @staticmethod
    def validate_port(value) -> str | None:
        if value is None:
            return None
        if not isinstance(value, str) or not value or len(value) > 128 or any(ord(c) < 32 for c in value):
            raise ApiError(400, "bad port")
        if sys.platform == "win32":
            if re.fullmatch(r"COM\d{1,3}", value, re.IGNORECASE):
                return value
        elif value in {c["device"] for c in serial_candidates()}:
            return value
        elif re.fullmatch(r"/dev/[\w./-]+", value) and ".." not in value and os.path.exists(value):
            return value
        raise ApiError(400, "no such port")

    def api_serial(self, body: dict) -> tuple[int, dict]:
        if set(body) != {"port"}:
            raise ApiError(400, "expected {port}")
        port = self.validate_port(body["port"])
        with self.config_lock:
            config = read_config()
            config["serial_port"] = port
            try:
                write_config(config)
            except OSError as e:
                log(f"config write failed: {e}")
                raise ApiError(500, f"配置文件写不进去：{e}")
        if sys.platform == "win32":
            return 200, {"result": "saved_restart_manual", "serial_port": port,
                         "explain": "已保存。Windows 上需要手动重启后台，新端口才会用上。"}
        new_port, reason = self.daemon.restart()
        if new_port is None:
            return 200, {"result": "saved_not_restarted", "serial_port": port, "reason": reason,
                         "explain": EXPLAIN.get(reason, "")}
        return 200, {"result": "restarted", "serial_port": port, "daemon_port": new_port, "serial": log_serial_port()}


# ── HTTP ─────────────────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):
    server_version = "clawd-mood"
    sys_version = ""
    timeout = HANDLER_TIMEOUT_S

    @property
    def app(self) -> App:
        return self.server.app

    def log_message(self, format, *args):  # noqa: A002
        pass  # stay quiet; nothing here needs a request log

    def end_headers(self):
        # Here rather than in a reply helper so http.server's own error pages carry them too
        for name, value in SECURITY_HEADERS.items():
            self.send_header(name, value)
        super().end_headers()

    def reply(self, status: int, body: bytes, content_type: str, cache: str = "no-store") -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", cache)
        self.end_headers()
        self.wfile.write(body)

    def reply_json(self, status: int, obj: dict) -> None:
        self.reply(status, json.dumps(obj, ensure_ascii=False).encode("utf-8"), CONTENT_TYPES[".json"])

    def origin_ok(self) -> bool:
        """Host must be exactly this server; an Origin, when sent, must be that same host."""
        port = self.server.server_address[1]
        hosts = self.headers.get_all("Host") or []
        if len(hosts) != 1 or hosts[0] not in (f"127.0.0.1:{port}", f"localhost:{port}"):
            return False
        origins = self.headers.get_all("Origin") or []
        if len(origins) > 1:
            return False
        return not origins or origins[0] == f"http://{hosts[0]}"

    def token_ok(self) -> bool:
        return hmac.compare_digest(self.headers.get("X-Clawd-Token", "").encode(), self.app.token.encode())

    def read_json_body(self) -> dict:
        kind = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if kind != "application/json":
            raise ApiError(415, "Content-Type must be application/json")
        try:
            length = int(self.headers.get("Content-Length", ""))
        except ValueError:
            raise ApiError(411, "Content-Length required")
        if length < 0 or length >= MAX_BODY_BYTES:
            raise ApiError(413, "body too large")
        try:
            raw = self.rfile.read(length)
        except TimeoutError:
            raise ApiError(408, "body timed out")
        if len(raw) != length:
            raise ApiError(400, "short body")
        try:
            body = json.loads(raw.decode("utf-8"))
        except ValueError:
            raise ApiError(400, "bad JSON")
        if not isinstance(body, dict):
            raise ApiError(400, "body must be an object")
        return body

    def guarded(self, handle) -> None:
        if not self.origin_ok():
            self.reply_json(403, {"error": "forbidden host or origin"})
            return
        try:
            handle()
        except ApiError as e:
            self.reply_json(e.status, {"error": e.message})

    def do_GET(self):
        self.guarded(self.handle_get)

    def do_POST(self):
        self.guarded(self.handle_post)

    def handle_get(self) -> None:
        path = self.path.split("?", 1)[0]
        if path.startswith("/api/"):
            if not self.token_ok():
                self.reply_json(401, {"error": "missing or wrong token"})
            elif path == "/api/status":
                self.reply_json(200, build_status(self.app.life))
            else:
                self.reply_json(404, {"error": "not found"})
            return
        file = self.app.static.get(path)
        if file is None:
            self.reply_json(404, {"error": "not found"})
            return
        try:
            data = file.read_bytes()
        except OSError:
            self.reply_json(404, {"error": "not found"})
            return
        self.reply(200, data, CONTENT_TYPES[file.suffix], "no-cache")

    def handle_post(self) -> None:
        path = self.path.split("?", 1)[0]
        if not path.startswith("/api/"):
            self.reply_json(405, {"error": "method not allowed"})
            return
        if not self.token_ok():
            self.reply_json(401, {"error": "missing or wrong token"})
            return
        routes = {"/api/cfg": self.app.api_cfg, "/api/serial": self.app.api_serial,
                  "/api/test": lambda _: self.app.api_test(), "/api/touch": lambda _: (200, {"result": "ok"})}
        route = routes.get(path)
        if route is None:
            self.reply_json(404, {"error": "not found"})
            return
        body = self.read_json_body()
        self.app.life.touch()
        status, reply = route(body)
        self.app.life.touch()  # the slow ones count from when they finish
        self.reply_json(status, reply)


class LocalServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True

    def server_bind(self):
        # HTTPServer's own version calls getfqdn, which can stall for seconds
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


def watchdog(server: LocalServer, life: Lifetime) -> None:
    while True:
        time.sleep(0.5)
        reason = life.expired()
        if reason:
            life.reason = reason
            server.shutdown()
            return


def parse_settings(pairs: list[str]) -> dict:
    body = {}
    for pair in pairs:
        key, sep, value = pair.partition("=")
        if not sep or key not in CFG_KEYS or not re.fullmatch(r"\d{1,3}", value):
            raise ApiError(400, f"expected KEY=NUMBER with KEY in {', '.join(CFG_KEYS)}: {pair}")
        body[key] = int(value)
    return body


def run_once(args) -> int:
    """One action without the page, for agents and scripts: the same code paths as the page's API,
    so the config file's last_sent stays right. Prints JSON; exit 0 means sent (or no change)."""
    app = App(token="")
    try:
        if args.status:
            print(json.dumps(build_status(app.life), ensure_ascii=False, indent=2))
            return 0
        if args.test:
            _, reply = app.api_test()
            if app.test_thread is not None:
                app.test_thread.join()   # the creature works, finishes and leaves before we exit
        else:
            body = {**read_config()["last_sent"], "force": True} if args.resend else parse_settings(args.set)
            _, reply = app.api_cfg(body)
    except ApiError as e:
        reply = {"result": "error", "explain": e.message}
    print(json.dumps(reply, ensure_ascii=False, indent=2))
    return 0 if reply["result"] in ("sent", "no_change") else 1


def main() -> None:
    parser = argparse.ArgumentParser(description="clawd-mood config page")
    parser.add_argument("--no-open", action="store_true", help="do not open a browser")
    parser.add_argument("--port", type=int, default=0, help="TCP port on 127.0.0.1 (default: random)")
    once = parser.add_mutually_exclusive_group()
    once.add_argument("--set", nargs="+", metavar="KEY=N",
                      help="send theme / rotation / brightness to the device and exit")
    once.add_argument("--resend", action="store_true", help="send every saved setting again and exit (after reflashing)")
    once.add_argument("--test", action="store_true", help="show the test creature on the screen and exit")
    once.add_argument("--status", action="store_true", help="print the page's diagnostics as JSON and exit")
    args = parser.parse_args()
    if args.set or args.resend or args.test or args.status:
        sys.exit(run_once(args))
    if not 0 <= args.port <= 65535:
        parser.error("--port must be 0-65535")

    app = App(secrets.token_urlsafe(32))  # 256 bits
    try:
        server = LocalServer(("127.0.0.1", args.port), Handler)
    except OSError as e:
        sys.exit(f"cannot bind 127.0.0.1:{args.port}: {e}")
    server.app = app
    port = server.server_address[1]
    url = f"http://127.0.0.1:{port}/#token={app.token}"

    print(f"clawd-mood 配置页: {url}", flush=True)
    print("本页 10 分钟无操作后自动关闭。", flush=True)
    threading.Thread(target=watchdog, args=(server, app.life), name="watchdog", daemon=True).start()
    if not args.no_open:
        webbrowser.open(url)

    def interrupt(*_):
        raise KeyboardInterrupt

    # Closing the terminal or a plain kill takes the same way out as Ctrl-C, which ends the test creature
    for name in ("SIGTERM", "SIGHUP"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), interrupt)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        app.life.reason = "interrupted"
    finally:
        server.server_close()
    thread = app.test_thread
    if thread is not None:
        thread.join(TEST_DONE_AFTER_S + TEST_END_AFTER_S + 2)  # do not strand the test creature on screen
    print(f"clawd-mood 配置页已关闭（{app.life.reason}）。", flush=True)


if __name__ == "__main__":
    main()
