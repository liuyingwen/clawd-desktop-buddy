#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["pyserial>=3.5"]
# ///
"""clawd-mood daemon — keeps serial port open and forwards state from FIFO to ESP32."""

import glob
import json
import os
import signal
import sys
import time

import serial

FIFO_PATH = "/tmp/clawd-mood.fifo"
BAUD_RATE = 115200


def detect_port() -> str:
    override = os.environ.get("CLAWD_MOOD_PORT")
    if override:
        return override
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit(
            "No /dev/cu.usbmodem* device found. "
            "Is the ESP32 plugged in? Or set CLAWD_MOOD_PORT=/dev/cu.xxx"
        )
    if len(ports) > 1:
        print(f"  warning: multiple devices found {ports}, using {ports[0]}",
              file=sys.stderr)
    return ports[0]


def open_serial(port: str) -> serial.Serial:
    ser = serial.Serial(
        port, BAUD_RATE, timeout=1,
        dsrdtr=False, rtscts=False,
    )
    # Suppress reset on open
    ser.dtr = False
    ser.rts = False
    time.sleep(0.5)
    ser.read(1000)  # drain boot output
    time.sleep(1.0)
    ser.read(1000)
    return ser


def make_fifo() -> None:
    try:
        os.unlink(FIFO_PATH)
    except FileNotFoundError:
        pass
    os.mkfifo(FIFO_PATH)


def cleanup(*_args):
    try:
        os.unlink(FIFO_PATH)
    except FileNotFoundError:
        pass
    sys.exit(0)


def main() -> None:
    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    port = detect_port()
    make_fifo()
    print(f"clawd-mood daemon started")
    print(f"  FIFO:   {FIFO_PATH}")
    print(f"  Serial: {port}")

    ser = open_serial(port)
    print("  Ready!")

    while True:
        # Opening FIFO blocks until a writer connects
        with open(FIFO_PATH, "r") as fifo:
            for line in fifo:
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
