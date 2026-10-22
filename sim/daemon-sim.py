#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["pyserial>=3.5"]
# ///
"""仿真专用：FIFO → 虚拟串口。

真 daemon（plugin/scripts/daemon.py）是 TCP → USB CDC，并设置 DTR/RTS。
socat 的 pty 不支持这两根信号，所以这里跳过。只由 sim/sim.sh 调用。
"""

import json
import os
import signal
import sys

import serial

FIFO_PATH = "/tmp/clawd-mood.fifo"
BAUD_RATE = 115200


def cleanup(*_):
    try:
        os.unlink(FIFO_PATH)
    except FileNotFoundError:
        pass
    sys.exit(0)


def main():
    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    port = os.environ.get("CLAWD_MOOD_PORT")
    if not port:
        sys.exit("CLAWD_MOOD_PORT required (sim mode points to /tmp/wokwi-tty)")

    try:
        os.unlink(FIFO_PATH)
    except FileNotFoundError:
        pass
    os.mkfifo(FIFO_PATH)

    ser = serial.Serial(port, BAUD_RATE, timeout=1)
    # NOTE: 真 daemon 在这里设 ser.dtr=False / ser.rts=False 抑制 ESP32 复位，
    # pty 不支持这俩 modem control 信号，跳过即可

    print(f"sim daemon ready: FIFO={FIFO_PATH} -> SERIAL={port}", flush=True)

    while True:
        with open(FIFO_PATH, "r") as fifo:
            for line in fifo:
                line = line.strip()
                if not line:
                    continue
                try:
                    json.loads(line)
                except json.JSONDecodeError:
                    print(f"  !! bad JSON: {line}", file=sys.stderr, flush=True)
                    continue
                ser.write((line + "\n").encode())
                ser.flush()
                print(f"  -> {line}", flush=True)


if __name__ == "__main__":
    main()
