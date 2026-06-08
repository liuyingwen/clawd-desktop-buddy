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
"""

import atexit
import json
import os
import signal
import socket
import sys
import tempfile
import time
from pathlib import Path

import serial
from serial.tools import list_ports

BAUD_RATE = 115200
DEFAULT_TCP_PORT = 48756
PORTFILE = Path(tempfile.gettempdir()) / "clawd-mood.port"


def detect_port() -> str:
    override = os.environ.get("CLAWD_MOOD_PORT")
    if override:
        return override
    candidates: list[str] = []
    for p in list_ports.comports():
        if sys.platform == "darwin" and p.device.startswith("/dev/cu.usbmodem"):
            candidates.append(p.device)
        elif sys.platform == "linux" and (
            p.device.startswith("/dev/ttyACM") or p.device.startswith("/dev/ttyUSB")
        ):
            candidates.append(p.device)
        elif (
            sys.platform == "win32"
            and p.device.upper().startswith("COM")
            and p.vid is not None
        ):
            candidates.append(p.device)
    candidates.sort()
    if not candidates:
        sys.exit(
            "No ESP32-like USB CDC device found. "
            "Plug it in or set CLAWD_MOOD_PORT=/dev/cu.xxx (or COM3 on Windows)."
        )
    if len(candidates) > 1:
        print(
            f"  warning: multiple devices {candidates}, using {candidates[0]}",
            file=sys.stderr,
        )
    return candidates[0]


def open_serial(port: str) -> serial.Serial:
    ser = serial.Serial(
        port, BAUD_RATE, timeout=1, dsrdtr=False, rtscts=False,
    )
    ser.dtr = False
    ser.rts = False
    time.sleep(0.5)
    ser.read(1000)
    time.sleep(1.0)
    ser.read(1000)
    return ser


def bind_listen(preferred: int, env_forced: bool) -> tuple[socket.socket, int]:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
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


def main() -> None:
    install_signal_handlers()
    check_singleton()

    forced = os.environ.get("CLAWD_MOOD_PORT_TCP")
    preferred = int(forced) if forced else DEFAULT_TCP_PORT
    server, actual_port = bind_listen(preferred, forced is not None)
    PORTFILE.write_text(str(actual_port))
    atexit.register(lambda: PORTFILE.unlink(missing_ok=True))

    serial_port = detect_port()
    ser = open_serial(serial_port)
    print(f"clawd-mood daemon started")
    print(f"  TCP:    127.0.0.1:{actual_port}")
    print(f"  Serial: {serial_port}")
    print(f"  Ready!")

    while True:
        conn, _ = server.accept()
        with conn:
            data = conn.recv(4096)
            for line in data.decode("utf-8", errors="replace").splitlines():
                line = line.strip()
                if not line:
                    continue
                try:
                    json.loads(line)
                except json.JSONDecodeError:
                    print(f"  !! bad JSON: {line}", file=sys.stderr)
                    continue
                try:
                    ser.write((line + "\n").encode())
                    ser.flush()
                    print(f"  -> {line}")
                except serial.SerialException as e:
                    print(f"  !! serial error: {e}", file=sys.stderr)
                    sys.exit(1)


if __name__ == "__main__":
    main()
