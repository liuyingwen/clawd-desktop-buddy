#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["pyserial>=3.5"]
# ///
"""clawd-mood daemon — TCP server forwarding state to ESP32 via USB CDC.

Replaces the POSIX FIFO (/tmp/clawd-mood.fifo) used in 0.1.x with a TCP
localhost socket. The actual listening port is written to a portfile at
<tempdir>/clawd-mood.port so the hook can discover it. Cross-platform:
macOS / Linux / Windows.

Survives the board being unplugged: messages are dropped while it is gone, and
later messages re-detect and reopen the port. Exits only if there is no device
at startup. Stays compatible with Python 3.9 (DeepSeek Harness runs python3).
"""

import atexit
import errno
import json
import os
import signal
import socket
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

import serial
from serial.tools import list_ports

try:
    import pwd
except ImportError:
    pwd = None  # Windows

BAUD_RATE = 115200
DEFAULT_TCP_PORT = 48756
PORTFILE = Path(tempfile.gettempdir()) / "clawd-mood.port"
ESPRESSIF_VID = 0x303A
CLIENT_TIMEOUT_S = 0.3  # hooks send right after connecting
RECONNECT_INTERVAL_S = 3.0
NO_DEVICE_MESSAGE = (
    "No ESP32-like USB CDC device found. Plug it in or set "
    "CLAWD_MOOD_PORT (mac: /dev/cu.xxx, linux: /dev/ttyACM0, windows: COM3)."
)

_warned: set[str] = set()


def warn_once(message: str) -> None:
    # Detection repeats every few seconds while the board is unplugged.
    if message not in _warned:
        _warned.add(message)
        print(f"  warning: {message}", file=sys.stderr)


def config_path() -> Path:
    override = os.environ.get("CLAWD_MOOD_CONFIG")
    if override:
        return Path(override)
    # $HOME can be replaced (DeepSeek Harness does), so ask the user database first.
    if pwd is not None:
        try:
            return Path(pwd.getpwuid(os.getuid()).pw_dir) / ".clawd-mood" / "config.json"
        except KeyError:
            pass
    return Path.home() / ".clawd-mood" / "config.json"


def configured_port() -> Optional[str]:
    """serial_port from the config file. A config problem must never stop the daemon."""
    try:
        path = config_path()
        config = json.loads(path.read_text(encoding="utf-8"))
        value = config.get("serial_port")
        if value is not None and not isinstance(value, str):
            raise ValueError(f"serial_port is not a string: {value!r}")
        return value or None
    except FileNotFoundError:
        return None
    except Exception as e:
        warn_once(f"ignoring config: {e}")
        return None


def is_candidate(p) -> bool:
    if sys.platform == "darwin":
        return p.device.startswith("/dev/cu.usbmodem")
    if sys.platform == "linux":
        return p.device.startswith("/dev/ttyACM") or p.device.startswith("/dev/ttyUSB")
    if sys.platform == "win32":
        return p.device.upper().startswith("COM") and p.vid is not None
    return False


def scan_ports(ports: list, espressif_only: bool = False) -> Optional[str]:
    candidates = [p for p in ports if is_candidate(p)]
    # Prefer a native-USB ESP32, so JSON doesn't land on some other board.
    remaining = [p.device for p in candidates if p.vid == ESPRESSIF_VID]
    if not remaining and not espressif_only:
        remaining = [p.device for p in candidates]
    remaining.sort()
    if not remaining:
        return None
    if len(remaining) > 1:
        warn_once(f"multiple devices found {remaining}, using {remaining[0]}")
    return remaining[0]


def detect_port(espressif_only: bool = False) -> Optional[str]:
    """CLAWD_MOOD_PORT, then the config's serial_port if plugged in, then a scan."""
    override = os.environ.get("CLAWD_MOOD_PORT")
    if override:
        return override
    ports = list_ports.comports()
    configured = configured_port()
    if configured:
        wanted = os.path.normcase(configured)
        if any(os.path.normcase(p.device) == wanted for p in ports):
            return configured
        warn_once(f"configured serial_port {configured} not found, scanning instead")
    return scan_ports(ports, espressif_only)


def is_espressif(port: str) -> bool:
    try:
        return any(p.device == port and p.vid == ESPRESSIF_VID for p in list_ports.comports())
    except Exception:
        return False


def open_serial(port: str) -> serial.Serial:
    ser = serial.Serial(
        port, BAUD_RATE, timeout=1, dsrdtr=False, rtscts=False,
    )
    try:
        # Suppress reset on open
        try:
            ser.dtr = False
            ser.rts = False
        except OSError as e:
            # Virtual ports have no modem lines; pyserial's own open() ignores these too.
            if e.errno not in (errno.EINVAL, errno.ENOTTY):
                raise
        time.sleep(0.5)
        ser.read(1000)  # drain boot output
        time.sleep(1.0)
        ser.read(1000)
    except Exception:
        ser.close()
        raise
    return ser


class SerialLink:
    """The serial port, which can vanish and come back while the daemon runs."""

    def __init__(self, port: str) -> None:
        self.ser: Optional[serial.Serial] = open_serial(port)
        # Once on an ESP32, wait for one to come back. Falling back to the other board
        # left plugged in would succeed and then keep us there for good.
        self.espressif_only = is_espressif(port)
        self.retry_at = 0.0
        self.dropped = 0

    def forward(self, line: str) -> None:
        if self.ser is None and not self._reopen():
            self.dropped += 1
            return
        try:
            self.ser.write((line + "\n").encode())
            self.ser.flush()
        except Exception as e:  # flush() can raise termios.error, which isn't an OSError
            self._disconnect(e)
            self.dropped += 1
            return
        print(f"  -> {line}")

    def _disconnect(self, reason: Exception) -> None:
        print(f"  !! serial error: {reason}; dropping messages until it is back", file=sys.stderr)
        try:
            self.ser.close()
        except Exception:
            pass  # already gone
        self.ser = None
        self.retry_at = time.monotonic() + RECONNECT_INTERVAL_S

    def _reopen(self) -> bool:
        # Opening takes seconds, but only a port that exists gets that far. Clients don't
        # wait on us and their messages sit in the TCP backlog, so doing it inline is fine.
        if time.monotonic() < self.retry_at:
            return False
        try:
            port = detect_port(self.espressif_only)
            if port is None:
                self.retry_at = time.monotonic() + RECONNECT_INTERVAL_S
                return False
            self.ser = open_serial(port)
            self.espressif_only = is_espressif(port)
        except Exception as e:
            warn_once(f"reopen failed: {e}")
            self.retry_at = time.monotonic() + RECONNECT_INTERVAL_S
            return False
        print(f"  Serial: {port} is back, {self.dropped} message(s) were dropped")
        self.dropped = 0
        _warned.clear()  # the next unplug should be logged again
        return True


def bind_listen(preferred: int, env_forced: bool) -> tuple[socket.socket, int]:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("127.0.0.1", preferred))
    except OSError:
        if env_forced:
            raise
        s.bind(("127.0.0.1", 0))
    s.listen(8)
    return s, s.getsockname()[1]


def install_signal_handlers() -> None:
    signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
    try:
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    except (AttributeError, ValueError):
        pass  # Windows lacks a real SIGTERM


def check_singleton() -> None:
    if not PORTFILE.exists():
        return
    try:
        port = int(PORTFILE.read_text().strip())
    except (OSError, ValueError):
        return
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
            sys.exit("Another clawd-mood daemon is already running.")
    except OSError:
        pass  # stale portfile — take over


def read_lines(conn: socket.socket) -> list[str]:
    """One client's message. The loop is serial, so a silent client must not hold it up."""
    try:
        conn.settimeout(CLIENT_TIMEOUT_S)
        data = conn.recv(4096)
    except OSError:  # includes the timeout
        return []
    # Web pages can reach this port, and an HTTP body could carry valid lines.
    if data.lstrip()[:1] not in (b"", b"{"):
        print("  !! dropped a connection that isn't NDJSON", file=sys.stderr)
        return []
    return data.decode("utf-8", errors="replace").splitlines()


def main() -> None:
    sys.stdout.reconfigure(line_buffering=True)  # log is a file when auto-started
    install_signal_handlers()
    check_singleton()

    forced = os.environ.get("CLAWD_MOOD_PORT_TCP")
    preferred = int(forced) if forced else DEFAULT_TCP_PORT
    server, actual_port = bind_listen(preferred, forced is not None)
    PORTFILE.write_text(str(actual_port))
    atexit.register(lambda: PORTFILE.unlink(missing_ok=True))

    serial_port = detect_port()
    if serial_port is None:
        sys.exit(NO_DEVICE_MESSAGE)
    try:
        link = SerialLink(serial_port)
    except Exception as e:
        sys.exit(f"Cannot open {serial_port}: {e}")
    print("clawd-mood daemon started")
    print(f"  TCP:    127.0.0.1:{actual_port}")
    print(f"  Portfile: {PORTFILE}")
    print(f"  Serial: {serial_port}")
    print("  Ready!")

    while True:
        try:
            conn, _ = server.accept()
        except OSError as e:
            print(f"  !! accept failed: {e}", file=sys.stderr)
            time.sleep(0.1)  # don't spin if it keeps failing, e.g. out of file descriptors
            continue
        with conn:
            for line in read_lines(conn):
                line = line.strip()
                if not line:
                    continue
                try:
                    json.loads(line)
                except json.JSONDecodeError:
                    print(f"  !! bad JSON: {line}", file=sys.stderr)
                    continue
                link.forward(line)


if __name__ == "__main__":
    main()
