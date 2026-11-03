#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Render the config page's headline with the firmware's wqy12 font, as a tiny transparent PNG.

Uses the same U8g2 library and font the device links, so the page's headline has real device
pixels. The page scales it up with image-rendering: pixelated.

    uv run tools/preview/headline.py
"""

import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import render  # noqa: E402  (shares the library lookup, object cache and PNG chunks)

TEXT = "选一套样式，让小队换衣服"
INK = (0xff, 0xf1, 0xe2)
OUT = render.ROOT / "page" / "assets" / "headline.png"


def build():
    """Reuses the library objects build_harness caches; only headline.cpp is compiled here."""
    harness = render.build_harness()  # makes sure libs/*.o exist
    objs = harness.parent / "libs"
    lib = render.find_libraries()
    gfx, u8g2 = lib / render.LIB_NAMES[0], lib / render.LIB_NAMES[1] / "src"
    cxx = os.environ.get("CXX", "c++")
    work = Path(tempfile.mkdtemp(prefix="clawd-headline-", dir=harness.parent))
    out = work / "headline"
    render.compile_unit(cxx, ["-std=gnu++17"], render.HERE / "headline.cpp", work / "headline.o",
                        [render.HERE / "shim", gfx, u8g2])
    subprocess.run([cxx, str(work / "headline.o"), str(objs / "gfx.o"), str(objs / "u8cpp.o"),
                    str(objs / "u8fonts.o"), "-o", str(out)], check=True)
    return out


def crop(width, height, data):
    rows = [data[y * width:(y + 1) * width] for y in range(height)]
    used_rows = [y for y, row in enumerate(rows) if any(row)]
    used_cols = [x for x in range(width) if any(row[x] for row in rows)]
    if not used_rows:
        sys.exit("the font drew nothing")
    return [row[used_cols[0]:used_cols[-1] + 1] for row in rows[used_rows[0]:used_rows[-1] + 1]]


def encode(rows):
    """1-bit indexed PNG: index 0 transparent, index 1 the ink."""
    width, height = len(rows[0]), len(rows)
    packed = bytearray()
    for row in rows:
        packed.append(0)
        for i in range(0, width, 8):
            byte = 0
            for v in row[i:i + 8].ljust(8, b"\0"):
                byte = byte << 1 | v
            packed.append(byte)
    return (b"\x89PNG\r\n\x1a\n"
            + render.png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 1, 3, 0, 0, 0))
            + render.png_chunk(b"PLTE", bytes((0, 0, 0)) + bytes(INK))
            + render.png_chunk(b"tRNS", b"\0")
            + render.png_chunk(b"IDAT", zlib.compress(bytes(packed), 9))
            + render.png_chunk(b"IEND", b""))


def main():
    done = subprocess.run([str(build()), TEXT], capture_output=True, check=True)
    head, _, body = done.stdout.partition(b"\n")
    width, height = map(int, head.split())
    rows = crop(width, height, body)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(encode(rows))
    print(f"{OUT.relative_to(render.ROOT)}: {len(rows[0])}x{len(rows)}, {OUT.stat().st_size} bytes")


if __name__ == "__main__":
    main()
