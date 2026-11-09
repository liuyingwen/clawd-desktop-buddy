#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["pyserial>=3.5", "bleak>=1.0"]
# ///
"""MiqiPet daemon — TCP server forwarding state to ESP32 via USB CDC.

Replaces the POSIX FIFO (/tmp/clawd-mood.fifo) used in 0.1.x with a TCP
localhost socket. The actual listening port is written to a portfile at
<tempdir>/miqipet.port so the hook can discover it. Cross-platform:
macOS / Linux / Windows.

Survives the board being unplugged: messages are dropped while it is gone, and
later messages re-detect and reopen the port. Exits only if there is no device
at startup. Stays compatible with Python 3.9 (DeepSeek Harness runs python3).

With no board on USB it falls back to Bluetooth LE (bleak), so the board can sit
on any charger. See docs/superpowers/specs/2026-11-05-ble-transport-design.md.

A client whose first line is {"sub":...} stays connected and gets a copy of every
line (with "x", the desktop fields the board never sees), after the latest line of every session and subagent heard from in the last
30 minutes: anything that wants to see what the board sees can subscribe. With
MIQIPET_KEEP_RUNNING=1 the daemon runs without any board, for subscribers.
"""

import asyncio
import atexit
import collections
import errno
import json
import os
import signal
import socket
import sys
import tempfile
import threading
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
PORTFILE = Path(tempfile.gettempdir()) / "miqipet.port"
ESPRESSIF_VID = 0x303A
CLIENT_TIMEOUT_S = 0.3  # hooks send right after connecting
RECONNECT_INTERVAL_S = 3.0
# Hooks connect while a reopen blocks the loop; a full backlog fails their probe and makes
# SessionStart start a second daemon.
LISTEN_BACKLOG = 64
NO_DEVICE_MESSAGE = (
    "No ESP32-like USB CDC device found and no MiqiPet board over Bluetooth. Plug it in or set "
    "MIQIPET_PORT (mac: /dev/cu.xxx, linux: /dev/ttyACM0, windows: COM3)."
)

# Must match firmware/miqipet/ble_link.h.
BLE_SERVICE_UUID = "ee97aa6d-6973-4f5a-9148-c26f84eb0f84"
BLE_RX_UUID = "f91d93a9-1bf8-4ae7-b4ac-31fcaa05cebc"
# What the board advertises: BLE_NAME_PREFIX + two MAC bytes. Boards still on firmware from before
# the rename advertise the old prefix; ble_name_variants() lets both name the same board.
BLE_NAME_PREFIX = "MiqiPet-"
BLE_NAME_PREFIX_OLD = "clawd-mood-"
BLE_SCAN_S = 10.0
BLE_RETRY_S = 5.0
# Lines kept while the board is out of range, flushed in order on reconnect: replaying a
# session's last states (and its SessionEnd) beats leaving it stuck until it expires.
BLE_BACKLOG = 100
KNOWN_BOARDS_MAX = 8

# A new subscriber first gets the latest line of every session (and subagent) it missed, as far
# back as the firmware keeps a quiet session. Per session, not the last N lines: busy agents push
# hundreds of tool lines a minute, and a quiet session's only line would scroll out.
SUB_REPLAY_S = 30 * 60
SUB_REPLAY_MAX = 300   # entries; the firmware shows 12 sessions

_warned: set[str] = set()


def warn_once(message: str) -> None:
    # Detection repeats every few seconds while the board is unplugged.
    if message not in _warned:
        _warned.add(message)
        print(f"  warning: {message}", file=sys.stderr)


def config_path() -> Path:
    override = os.environ.get("MIQIPET_CONFIG")
    if override:
        return Path(override)
    # $HOME can be replaced (DeepSeek Harness does), so ask the user database first.
    if pwd is not None:
        try:
            return Path(pwd.getpwuid(os.getuid()).pw_dir) / ".miqipet" / "config.json"
        except KeyError:
            pass
    return Path.home() / ".miqipet" / "config.json"


def configured(key: str) -> Optional[str]:
    """A string setting from the config file. A config problem must never stop the daemon."""
    try:
        path = config_path()
        config = json.loads(path.read_text(encoding="utf-8"))
        value = config.get(key)
        if value is not None and not isinstance(value, str):
            raise ValueError(f"{key} is not a string: {value!r}")
        return value or None
    except FileNotFoundError:
        return None
    except Exception as e:
        warn_once(f"ignoring config: {e}")
        return None


def configured_port() -> Optional[str]:
    return configured("serial_port")


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
    """MIQIPET_PORT, then the config's serial_port if plugged in, then a scan."""
    override = os.environ.get("MIQIPET_PORT")
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


def boards_path() -> Path:
    """Boards this daemon has seen on USB. Its own file: the config file belongs to the page."""
    return config_path().with_name("boards.json")


def ble_name_for(serial_number: Optional[str]) -> Optional[str]:
    """The name an ESP32-C3 advertises, from the MAC its USB port reports as serial number.

    The firmware names itself after the Bluetooth MAC, which is the base MAC plus 2.
    """
    try:
        mac = int((serial_number or "").replace(":", ""), 16)
    except ValueError:
        return None
    if not serial_number or serial_number.count(":") != 5:
        return None
    bt = (mac + 2) & 0xFFFFFFFFFFFF
    return f"{BLE_NAME_PREFIX}{(bt >> 8) & 0xFF:02X}{bt & 0xFF:02X}"


def ble_name_variants(name: str) -> set:
    """Every name the same board may advertise: the one given plus its old/new counterpart.

    MiqiPet-AB0A and clawd-mood-AB0A are one board on two firmware versions. boards.json written
    before the rename, MIQIPET_BLE_NAME and the config's ble_name may hold either form.
    """
    variants = {name}
    for prefix, other in ((BLE_NAME_PREFIX, BLE_NAME_PREFIX_OLD), (BLE_NAME_PREFIX_OLD, BLE_NAME_PREFIX)):
        if name.startswith(prefix):
            variants.add(other + name[len(prefix):])
    return variants


def known_boards() -> list:
    try:
        names = json.loads(boards_path().read_text(encoding="utf-8")).get("ble_names", [])
        return [n for n in names if isinstance(n, str)]
    except FileNotFoundError:
        return []
    except Exception as e:
        warn_once(f"ignoring {boards_path()}: {e}")
        return []


def remember_board(port: str) -> None:
    """Pin Bluetooth to the boards plugged in here, so it never picks a neighbour's."""
    try:
        info = next((p for p in list_ports.comports() if p.device == port), None)
        if info is None or info.vid != ESPRESSIF_VID:
            return
        name = ble_name_for(info.serial_number)
        names = known_boards()
        if name is None or (names and names[0] == name):
            return
        # Also drops the same board under its old name, so the file migrates to the new form.
        names = [name] + [n for n in names if n not in ble_name_variants(name)][: KNOWN_BOARDS_MAX - 1]
        path = boards_path()
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps({"ble_names": names}) + "\n", encoding="utf-8")
        os.replace(tmp, path)
    except Exception as e:
        warn_once(f"cannot remember board: {e}")


def ble_wanted() -> list:
    """Names Bluetooth may connect to, most recent first.

    Never any board in range: that could be a neighbour's, and scanning for nothing would
    ask macOS users for Bluetooth access they never wanted.
    """
    pinned = os.environ.get("MIQIPET_BLE_NAME") or configured("ble_name")
    return [pinned] if pinned else known_boards()


def ble_enabled() -> bool:
    if os.environ.get("MIQIPET_BLE", "1") == "0" or not ble_wanted():
        return False
    try:
        import bleak  # noqa: F401  only loaded when there is no board on USB
    except Exception as e:
        warn_once(f"Bluetooth unavailable: {e}")
        return False
    return True


class BleLink:
    """The board over Bluetooth LE. bleak is asyncio, so it runs on its own thread and loop."""

    def __init__(self) -> None:
        self.loop = asyncio.new_event_loop()
        self.pending: collections.deque = collections.deque(maxlen=BLE_BACKLOG)
        self.wake: Optional[asyncio.Event] = None
        self.connected = threading.Event()
        self.first_scan = threading.Event()  # set once the first scan finished, found or not
        self.found = False
        self.task: Optional[asyncio.Future] = None
        threading.Thread(target=self._thread, name="ble", daemon=True).start()

    def _thread(self) -> None:
        asyncio.set_event_loop(self.loop)
        self.wake = asyncio.Event()
        self.task = self.loop.create_task(self._run())
        try:
            self.loop.run_until_complete(self.task)
        except asyncio.CancelledError:
            pass

    def send(self, line: str) -> None:
        self.loop.call_soon_threadsafe(self._push, line)

    def _push(self, line: str) -> None:
        self.pending.append(line)
        self.wake.set()

    def stop(self) -> None:
        if self.task is not None:
            self.loop.call_soon_threadsafe(self.task.cancel)

    async def _find(self):
        from bleak import BleakScanner

        # Either firmware's name for every wanted board, see ble_name_variants().
        wanted = {v for n in ble_wanted() for v in ble_name_variants(n)}

        def match(device, adv) -> bool:
            if BLE_SERVICE_UUID not in [u.lower() for u in adv.service_uuids]:
                return False
            # The name comes in the scan response, so a wanted board may match a moment later.
            return (adv.local_name or device.name) in wanted

        return await BleakScanner.find_device_by_filter(match, timeout=BLE_SCAN_S)

    async def _run(self) -> None:
        from bleak import BleakClient

        while True:
            try:
                device = await self._find()
            except Exception as e:  # Bluetooth off, or no permission to use it
                warn_once(f"Bluetooth scan failed: {e}")
                device = None
            self.found = self.found or device is not None
            self.first_scan.set()
            if device is None:
                await asyncio.sleep(BLE_RETRY_S)
                continue
            try:
                async with BleakClient(device, disconnected_callback=lambda _: self.wake.set()) as client:
                    self.connected.set()
                    print(f"  Bluetooth: connected to {device.name or device.address}"
                          f"{f', sending {len(self.pending)} queued' if self.pending else ''}")
                    _warned.clear()  # the next drop should be logged again
                    await self._pump(client)
            except asyncio.CancelledError:
                raise
            except Exception as e:
                warn_once(f"Bluetooth: {e}")
            if self.connected.is_set():
                self.connected.clear()
                print("  !! Bluetooth: disconnected; queueing until it is back", file=sys.stderr)
            await asyncio.sleep(1.0)

    async def _pump(self, client) -> None:
        chunk = max(20, client.mtu_size - 3)  # one ATT write carries MTU - 3 bytes
        while client.is_connected:
            if not self.pending:
                self.wake.clear()
                await self.wake.wait()
                continue
            line = self.pending[0]
            data = (line + "\n").encode()
            for i in range(0, len(data), chunk):
                # Acknowledged writes: without a response, a line written as the link dies is
                # lost silently. A failed write keeps the line queued; the board drops a torn one.
                await client.write_gatt_char(BLE_RX_UUID, data[i:i + chunk], response=True)
            self.pending.popleft()
            print(f"  -> {line} (bt)")


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

    def __init__(self, port: Optional[str], ble: Optional[BleLink] = None) -> None:
        self.ser: Optional[serial.Serial] = open_serial(port) if port else None
        if port:
            remember_board(port)
        # Once on an ESP32, wait for one to come back. Falling back to the other board
        # left plugged in would succeed and then keep us there for good.
        self.espressif_only = is_espressif(port) if port else True
        self.retry_at = 0.0
        self.dropped = 0
        self.ble = ble  # takes over while there is no board on USB

    def forward(self, line: str) -> None:
        if self.ser is None and not self._reopen():
            self._fallback(line)
            return
        if not self._write(line):
            # An unplug nobody wrote through leaves a dead handle while the board may already
            # be back, so try once now instead of after the reconnect interval.
            self.retry_at = 0.0
            if not (self._reopen() and self._write(line)):
                self._fallback(line)
                return
        print(f"  -> {line}")

    def _fallback(self, line: str) -> None:
        if self.ble is None and ble_enabled():
            print("  Bluetooth: no board on USB, looking for one nearby")
            self.ble = BleLink()
        if self.ble is not None:
            self.ble.send(line)  # logged when it is actually written
        else:
            self.dropped += 1

    def _write(self, line: str) -> bool:
        try:
            self.ser.write((line + "\n").encode())
            self.ser.flush()
        except Exception as e:  # flush() can raise termios.error, which isn't an OSError
            self._disconnect(e)
            return False
        return True

    def _disconnect(self, reason: Exception) -> None:
        print(f"  !! serial error: {reason}; dropping messages until it reconnects", file=sys.stderr)
        try:
            self.ser.close()
        except Exception:
            pass  # already gone
        self.ser = None
        self.retry_at = time.monotonic() + RECONNECT_INTERVAL_S

    def _reopen(self) -> bool:
        # Opening takes about 3.5 s, but only a port that exists gets that far. Clients don't
        # wait on us and their messages sit in the TCP backlog (see LISTEN_BACKLOG), so doing
        # it inline is fine.
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
        remember_board(port)
        if self.ble is not None:  # the board is on USB again: the cable wins
            self.ble.stop()
            self.ble = None
            print("  Bluetooth: stopped, using USB")
        self.dropped = 0
        _warned.clear()  # the next unplug should be logged again
        return True


class Subscribers:
    """Connections that asked for a copy of every line.

    Sends never block: the accept loop is serial, so a subscriber that stops reading is dropped
    rather than waited for. It reconnects and catches up from the replay buffer.
    """

    def __init__(self) -> None:
        self.conns: list = []
        # (session, agent or None) -> (monotonic time, line), oldest first. Only to replay: the
        # lines themselves are still forwarded untouched.
        self.latest: dict = {}

    def add(self, conn: socket.socket) -> None:
        conn.setblocking(False)
        horizon = time.monotonic() - SUB_REPLAY_S
        for at, line in list(self.latest.values()):
            if at >= horizon and not self._send(conn, line):
                return
        self.conns.append(conn)
        print(f"  subscriber connected ({len(self.conns)})")

    def remember(self, line: str, obj) -> None:
        if not isinstance(obj, dict) or "cfg" in obj:
            return   # settings are the board's, not a session's
        session = str(obj.get("session") or "default")
        agent = obj.get("agent")
        if obj.get("end"):
            for key in [k for k in self.latest if k[0] == session]:
                del self.latest[key]
            return
        key = (session, str(agent) if agent else None)
        self.latest.pop(key, None)   # re-insert: the dict stays in order of the last line
        if agent and obj.get("gone"):
            return
        self.latest[key] = (time.monotonic(), line)
        while len(self.latest) > SUB_REPLAY_MAX:
            del self.latest[next(iter(self.latest))]

    def broadcast(self, line: str, obj=None) -> None:
        self.remember(line, obj)
        for conn in list(self.conns):
            if not self._send(conn, line):
                self.conns.remove(conn)
                print(f"  subscriber gone ({len(self.conns)})")

    @staticmethod
    def _send(conn: socket.socket, line: str) -> bool:
        data = (line + "\n").encode()
        try:
            if conn.send(data) == len(data):
                return True
        except OSError:  # includes BlockingIOError: its buffer is full, it is not reading
            pass
        try:
            conn.close()
        except OSError:
            pass
        return False


def for_board(line: str, obj) -> str:
    """The line as the board gets it: without "x", the desktop pet's fields (hook.py's
    desktop_fields: working directory, terminal), which would push a line over the firmware's
    320-byte LINE_MAX. Lines without it are forwarded byte for byte."""
    if not isinstance(obj, dict) or "x" not in obj:
        return line
    return json.dumps({k: v for k, v in obj.items() if k != "x"}, ensure_ascii=False, separators=(",", ":"))


def is_subscription(line: str) -> bool:
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        return False
    return isinstance(obj, dict) and "sub" in obj


def bind_listen(preferred: int, env_forced: bool) -> tuple[socket.socket, int]:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("127.0.0.1", preferred))
    except OSError:
        if env_forced:
            raise
        s.bind(("127.0.0.1", 0))
    s.listen(LISTEN_BACKLOG)
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
            sys.exit("Another MiqiPet daemon is already running.")
    except OSError:
        pass  # stale portfile — take over


def remove_own_portfile(port: int) -> None:
    """Only while it still names this daemon: another one may have taken over since."""
    try:
        if PORTFILE.read_text().strip() == str(port):
            PORTFILE.unlink()
    except OSError:
        pass


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

    forced = os.environ.get("MIQIPET_PORT_TCP")
    preferred = int(forced) if forced else DEFAULT_TCP_PORT
    server, actual_port = bind_listen(preferred, forced is not None)
    PORTFILE.write_text(str(actual_port))
    atexit.register(remove_own_portfile, actual_port)

    # Started for a subscriber: no board is fine
    keep_running = os.environ.get("MIQIPET_KEEP_RUNNING") == "1"
    serial_port = detect_port()
    ble = None
    if serial_port is None and ble_enabled():
        ble = BleLink()
        if not keep_running:
            # Same rule as USB: no board at startup, no daemon. The first scan decides.
            ble.first_scan.wait(BLE_SCAN_S + 5)
            if not ble.found:
                sys.exit(NO_DEVICE_MESSAGE)
    if serial_port is None and ble is None and not keep_running:
        sys.exit(NO_DEVICE_MESSAGE)
    try:
        link = SerialLink(serial_port, ble)
    except Exception as e:
        sys.exit(f"Cannot open {serial_port}: {e}")
    print("MiqiPet daemon started")
    print(f"  TCP:    127.0.0.1:{actual_port}")
    print(f"  Portfile: {PORTFILE}")
    print(f"  Serial: {serial_port or ('none, using Bluetooth' if ble else 'none, subscribers only')}")
    print("  Ready!")

    subscribers = Subscribers()
    while True:
        try:
            conn, _ = server.accept()
        except OSError as e:
            print(f"  !! accept failed: {e}", file=sys.stderr)
            time.sleep(0.1)  # don't spin if it keeps failing, e.g. out of file descriptors
            continue
        lines = [line.strip() for line in read_lines(conn)]
        lines = [line for line in lines if line]
        if lines and is_subscription(lines[0]):
            subscribers.add(conn)  # stays open; anything after the request is ignored
            continue
        with conn:
            for line in lines:
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    print(f"  !! bad JSON: {line}", file=sys.stderr)
                    continue
                subscribers.broadcast(line, obj)
                link.forward(for_board(line, obj))


if __name__ == "__main__":
    main()
