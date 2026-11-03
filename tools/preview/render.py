#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""Host preview tool: compiles the real firmware on the host and records what it draws.

    render.py                 write page/previews/ (sprite sheets and manifest.json)
    render.py --check         previews are up to date and theme 1 still matches baseline.txt
    render.py --contact PATH  write an overview image of every theme
    render.py --baseline      re-record baseline.txt (only after looking at what changed)
    render.py --compare FILE  list every frame that differs from a recorded baseline
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from array import array
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SCENES = HERE / "scenes"
BASELINE = HERE / "baseline.txt"
FIRMWARE = ROOT / "firmware" / "clawd_mood"
PREVIEWS = ROOT / "page" / "previews"
LIB_NAMES = ["Adafruit_GFX_Library", "U8g2_for_Adafruit_GFX", "ArduinoJson"]
HOST_FLAGS = [
    "-O1", "-w", "-funsigned-char", "-DARDUINO=10819",  # ESP32's char is unsigned, the host's may not be
    "-DU8G2_USE_LARGE_FONTS",  # on by default only for ESP boards
    "-DARDUINOJSON_ENABLE_PROGMEM=0", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
    "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
]


def find_libraries():
    """Arduino's own library folder, so the host draws with the versions the device links."""
    candidates = []
    try:
        out = subprocess.run(["arduino-cli", "config", "get", "directories.user"],
                             capture_output=True, text=True, timeout=30)
        if out.returncode == 0 and out.stdout.strip():
            candidates.append(Path(out.stdout.strip()) / "libraries")
    except (OSError, subprocess.SubprocessError):
        pass
    candidates.append(Path.home() / "Documents" / "Arduino" / "libraries")
    for lib in candidates:
        if all((lib / name).is_dir() for name in LIB_NAMES):
            return lib
    sys.exit("Adafruit GFX, U8g2_for_Adafruit_GFX and ArduinoJson not found in: "
             + ", ".join(str(c) for c in candidates))


def compile_unit(compiler, std, src, obj, includes, extra=()):
    cmd = [compiler, *std, *HOST_FLAGS, *extra, *[f"-I{i}" for i in includes], "-c", str(src), "-o", str(obj)]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode:
        sys.exit(f"compile failed: {src}\n{done.stderr}")


def build_harness(sketch=FIRMWARE, defines=()):
    """Build outside the repo; library objects are cached, the sketch is rebuilt when it changes."""
    lib = find_libraries()
    gfx, u8g2, ajson = lib / LIB_NAMES[0], lib / LIB_NAMES[1] / "src", lib / LIB_NAMES[2] / "src"
    shim = HERE / "shim"
    includes = [shim, gfx, u8g2, ajson, sketch]
    cxx = os.environ.get("CXX", "c++")
    cc = os.environ.get("CC", "cc")
    gnu17 = ["-std=gnu++17"]
    cache = Path(os.environ.get("CLAWD_PREVIEW_BUILD", Path(tempfile.gettempdir()) / "clawd-mood-preview"))
    objs = cache / "libs"
    objs.mkdir(parents=True, exist_ok=True)
    libs = [(cxx, gnu17, gfx / "Adafruit_GFX.cpp", objs / "gfx.o"),
            (cxx, gnu17, u8g2 / "U8g2_for_Adafruit_GFX.cpp", objs / "u8cpp.o"),
            (cc, [], u8g2 / "u8g2_fonts.c", objs / "u8fonts.o")]
    with ThreadPoolExecutor() as pool:
        jobs = [pool.submit(compile_unit, c, std, src, obj, [shim, gfx, u8g2])
                for c, std, src, obj in libs if not obj.exists()]
        for job in jobs:
            job.result()

    sources = [HERE / "harness.cpp", *sorted(shim.rglob("*.h")), *sorted(sketch.glob("*.ino")),
               *sorted(sketch.glob("*.h")), *sorted(sketch.glob("*.cpp"))]
    key = hashlib.sha1("\0".join(defines).encode())
    for src in sources:
        key.update(src.name.encode())
        key.update(src.read_bytes())
    out = cache / f"harness-{key.hexdigest()[:12]}"
    if out.exists():
        return out
    work = cache / f"obj-{key.hexdigest()[:12]}"
    work.mkdir(parents=True, exist_ok=True)
    units = [HERE / "harness.cpp", *sorted(sketch.glob("*.cpp"))]
    unit_objs = [work / (u.stem + ".o") for u in units]
    with ThreadPoolExecutor() as pool:
        jobs = [pool.submit(compile_unit, cxx, gnu17, u, o, includes, defines) for u, o in zip(units, unit_objs)]
        for job in jobs:
            job.result()
    done = subprocess.run([cxx, *map(str, unit_objs), *[str(o) for *_, o in libs], "-o", str(out)],
                          capture_output=True, text=True)
    if done.returncode:
        sys.exit(f"link failed:\n{done.stderr}")
    shutil.rmtree(work, ignore_errors=True)
    return out


# ── Frame hashes ─────────────────────────────────────────────────

def scene_names():
    return sorted(p.stem for p in SCENES.glob("*.ndjson"))


def run_hashes(harness, name, workdir):
    log = Path(workdir) / f"{name}.hashes"
    subprocess.run([str(harness), "--scene", str(SCENES / f"{name}.ndjson"), "--hashes", str(log)], check=True)
    return log.read_text().split("\n")[:-1]


def record_all(harness):
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor() as pool:
        names = scene_names()
        return dict(zip(names, pool.map(lambda n: run_hashes(harness, n, tmp), names)))


def write_baseline(path, frames):
    with open(path, "w") as f:
        f.write("# Frame hashes of theme 1 for every scene: <virtual ms> <S if mid-slide> <FNV-1a 64 of the RGB565 frame>\n")
        for name, lines in frames.items():
            f.write(f"@ {name}\n")
            f.write("\n".join(lines) + "\n")


def read_baseline(path):
    frames, current = {}, None
    for line in Path(path).read_text().splitlines():
        if line.startswith("#"):
            continue
        if line.startswith("@ "):
            current = frames.setdefault(line[2:], [])
        else:
            current.append(line)
    return frames


def compare(baseline, current, verbose=True):
    """Returns the number of frames that differ; every difference is listed."""
    total = changed = changed_slide = 0
    for name in sorted(set(baseline) | set(current)):
        old, new = baseline.get(name), current.get(name)
        if old is None or new is None:
            print(f"  {name}: only in {'current run' if old is None else 'baseline'}")
            changed += len(old or new)
            continue
        if len(old) != len(new):
            print(f"  {name}: {len(old)} frames recorded, {len(new)} now")
            changed += abs(len(old) - len(new))
        total += min(len(old), len(new))
        diffs = [(a, b) for a, b in zip(old, new) if a != b]
        slide = sum(a.split()[1] == "S" and b.split()[1] == "S" for a, b in diffs)
        changed += len(diffs)
        changed_slide += slide
        if diffs and verbose:
            at = ", ".join(a.split()[0] for a, _ in diffs[:40])
            print(f"  {name}: {len(diffs)} of {len(old)} differ ({slide} mid-slide) at ms {at}"
                  + (" ..." if len(diffs) > 40 else ""))
    print(f"compared {total} frames: {changed} differ, {changed_slide} of them mid-slide, "
          f"{changed - changed_slide} not")
    return changed


# ── Previews ─────────────────────────────────────────────────────

FPS = 10
FRAME_MS = 1000 // FPS
W = H = 240
FRAME_BYTES = W * H * 2
MAX_SHEET_BYTES = 200 * 1024
TOAST_WAIT_MS = 3200   # the cfg line selecting a theme shows a 3 s toast; start scenes after it
PREVIEW_SCENES = ["squad", "solo", "empty"]


def read_scene(name):
    meta, lines = {}, []
    for raw in (SCENES / f"{name}.ndjson").read_text(encoding="utf-8").splitlines():
        line = json.loads(raw)
        if "t" not in line:
            meta.update(line)
        elif not line.get("end"):
            lines.append(line)
    return meta, lines


def line_json(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def run_lines(harness, lines, workdir, tag):
    """Runs a scene given as dicts; returns (hash log lines, frames as bytes, report)."""
    work = Path(workdir)
    scene, hashes, frames, report = (work / f"{tag}{ext}" for ext in (".ndjson", ".hashes", ".rgb565", ".json"))
    scene.write_text("\n".join(line_json(l) for l in lines) + "\n", encoding="utf-8")
    subprocess.run([str(harness), "--scene", str(scene), "--hashes", str(hashes), "--frames", str(frames),
                    "--report", str(report)], check=True)
    return hashes.read_text().splitlines(), frames.read_bytes(), json.loads(report.read_text())


def select_theme(theme_id):
    """Scene prefix: the theme arrives through the real handleLine, as a cfg line."""
    return [{"t": 0, "record": False}, {"t": 0, "send": {"cfg": {"theme": theme_id}}}]


def shifted(lines, offset):
    return [{**line, "t": line["t"] + offset} for line in lines]


def shortest_loop(hashes):
    """Frames come as N+1 so the last must equal the first; returns the smallest period that closes."""
    n = len(hashes) - 1
    if hashes[n] != hashes[0]:
        return None
    for period in range(1, n + 1):
        if n % period == 0 and all(hashes[i] == hashes[i % period] for i in range(n)):
            return period
    return n


def render_preview(harness, theme, name, workdir):
    meta, lines = read_scene(name)
    preview = meta["preview"]
    start = TOAST_WAIT_MS + preview["settle_ms"]
    stop = start + preview["loop_ms"]
    scene = select_theme(theme["id"]) + shifted(lines, TOAST_WAIT_MS)
    scene += [{"t": start, "record": True}, {"t": stop + 5, "end": True}]
    scene.sort(key=lambda l: l["t"])
    log, raw, report = run_lines(harness, scene, workdir, f"{theme['key']}-{name}")
    if report["serial_tx"]:
        sys.exit(f"{theme['key']}/{name}: firmware printed {report['serial_tx']} bytes on a normal run")
    if report["slide_frames"]:
        sys.exit(f"{theme['key']}/{name}: slide frames are not on the {FRAME_MS} ms grid")
    times = [int(l.split()[0]) for l in log]
    if times != list(range(start, stop + 1, FRAME_MS)):
        sys.exit(f"{theme['key']}/{name}: unexpected frame times {times[:3]}...")
    period = shortest_loop([l.split()[2] for l in log])
    if period is None:
        sys.exit(f"{theme['key']}/{name}: does not loop over {preview['loop_ms']} ms; "
                 "that is not a multiple of an animation on screen")
    return raw[:period * FRAME_BYTES], period


def rgb8(c):
    return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)


def png_chunk(tag, data):
    body = tag + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))


def encode_indexed_png(raw, frames):
    """Vertical sprite sheet. At 16 colours or fewer a frame fits deflate's 32 KB window, so the
    previous frame is reachable and the sheet shrinks a lot."""
    pixels = array("H")
    pixels.frombytes(raw)
    colors = sorted(set(pixels))
    if len(colors) > 256:
        sys.exit(f"{len(colors)} colours do not fit an indexed PNG")
    index = {c: i for i, c in enumerate(colors)}
    depth = 8 if len(colors) > 16 else 4 if len(colors) > 4 else 2 if len(colors) > 2 else 1
    per_row = W * depth // 8
    data = bytes(index[c] for c in pixels)
    if depth < 8:
        rows = bytearray()
        per_px = 8 // depth
        for i in range(0, len(data), per_px):
            byte = 0
            for v in data[i:i + per_px]:
                byte = byte << depth | v
            rows.append(byte)
        data = bytes(rows)
    height = H * frames
    rows = [data[y * per_row:(y + 1) * per_row] for y in range(height)]
    plain = b"".join(b"\0" + row for row in rows)
    # A row equal to the one above costs almost nothing as "Up"; the others stay unfiltered
    mixed = b"".join((b"\2" + bytes(per_row) if y and row == rows[y - 1] else b"\0" + row) for y, row in enumerate(rows))
    idat = min((zlib.compress(payload, 9) for payload in (plain, mixed)), key=len)
    plte = b"".join(bytes(rgb8(c)) for c in colors)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", W, height, depth, 3, 0, 0, 0))
            + png_chunk(b"PLTE", plte) + png_chunk(b"IDAT", idat) + png_chunk(b"IEND", b""))


def decode_png_pixels(blob):
    """(width, height, RGB of every pixel) of an indexed PNG, so --check doesn't depend on zlib's exact bytes."""
    pos, idat, plte, head = 8, b"", b"", None
    while pos < len(blob):
        size = struct.unpack(">I", blob[pos:pos + 4])[0]
        tag, body = blob[pos + 4:pos + 8], blob[pos + 8:pos + 8 + size]
        pos += 12 + size
        head = body if tag == b"IHDR" else head
        plte = body if tag == b"PLTE" else plte
        idat += body if tag == b"IDAT" else b""
    width, height, depth = struct.unpack(">IIB", head[:9])
    per_row = (width * depth + 7) // 8
    raw = zlib.decompress(idat)
    out = bytearray()
    previous = bytes(per_row)
    for y in range(height):
        offset = y * (per_row + 1)
        kind, row = raw[offset], raw[offset + 1:offset + 1 + per_row]
        if kind == 2:
            row = bytes((a + b) & 255 for a, b in zip(row, previous))
        elif kind != 0:
            sys.exit(f"unexpected PNG filter {kind}")
        previous = row
        for x in range(width):
            v = row[x * depth // 8] >> (8 - depth - x * depth % 8) & ((1 << depth) - 1)
            out += plte[v * 3:v * 3 + 3]
    return width, height, bytes(out)


def load_themes(harness):
    done = subprocess.run([str(harness), "--themes"], capture_output=True, text=True, check=True)
    return json.loads(done.stdout)


def render_all(harness):
    """Returns ({relative path: PNG bytes}, manifest)."""
    themes = load_themes(harness)
    jobs = [(t, name) for t in themes for name in PREVIEW_SCENES]
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor() as pool:
        done = list(pool.map(lambda job: render_preview(harness, job[0], job[1], tmp), jobs))
    files, entries = {}, {t["id"]: {} for t in themes}
    for (theme, name), (raw, frames) in zip(jobs, done):
        rel = f"theme-{theme['id']}/{name}.png"
        files[rel] = encode_indexed_png(raw, frames)
        entries[theme["id"]][name] = {"file": rel, "frames": frames, "fps": FPS, "w": W, "h": H}
    manifest = {"themes": [{"id": t["id"], "key": t["key"], "name": t["name"], "tagline": t["tagline"],
                            "min_fw": t["min_fw"], "glow": t["glow"], "scenes": entries[t["id"]]} for t in themes]}
    return files, manifest


def manifest_text(manifest):
    return json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"


def write_previews(files, manifest):
    for rel, blob in files.items():
        print(f"  {rel}: {len(blob) / 1024:.1f} KB")
        if len(blob) > MAX_SHEET_BYTES:
            sys.exit(f"{rel} is over {MAX_SHEET_BYTES // 1024} KB: shorten that scene's loop_ms")
    for rel, blob in files.items():
        path = PREVIEWS / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(blob)
    (PREVIEWS / "manifest.json").write_text(manifest_text(manifest), encoding="utf-8")


def check_previews(files, manifest):
    problems = []
    path = PREVIEWS / "manifest.json"
    if not path.exists() or path.read_text(encoding="utf-8") != manifest_text(manifest):
        problems.append("manifest.json")
    for rel, blob in files.items():
        path = PREVIEWS / rel
        if not path.exists() or decode_png_pixels(path.read_bytes()) != decode_png_pixels(blob):
            problems.append(rel)
        elif path.stat().st_size > MAX_SHEET_BYTES:
            problems.append(f"{rel} (over {MAX_SHEET_BYTES // 1024} KB)")
    return problems


# ── Contrast ─────────────────────────────────────────────────────

def luminance(c):
    def channel(v):
        v /= 255
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = map(channel, rgb8(c))
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    hi, lo = sorted((luminance(a), luminance(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)


def contrast_warnings(theme):
    """WCAG ratios computed on the RGB565 values that reach the panel, before any overlay."""
    p = theme["pal"]
    states = ["idle", "thinking", "working", "waiting", "done", "error", "sleeping"]
    target = p["sleepTarget"]
    dim = lambda c: ((c >> 1) & 0x7BEF) + ((target >> 1) & 0x7BEF)
    pairs = []
    for floor in ("bg", "waitFlash"):
        pairs += [(f"text on {floor}", p["text"], p[floor], 4.5), (f"tag on {floor}", p["tag"], p[floor], 4.5),
                  (f"+N on {floor}", p["more"], p[floor], 3)]
    pairs += [("waiting follower on waitFlash", p["state"][3], p["waitFlash"], 2),
              ("error frame on bg", p["errorFrame"], p["bg"], 3), ("smoke on bg", p["smoke"], p["bg"], 3),
              ("pager on", p["pagerOn"], p["pagerBg"], 3), ("pager off", p["pagerOff"], p["pagerBg"], 1.5),
              ("toast", p["toastText"], p["toastBg"], 4.5)]
    for name, c in zip(states, p["state"]):
        pairs += [(f"{name} icon on plate", c, p["plate"], 3), (f"{name} tile on bg", c, p["bg"], 2)]
    pairs += [(f"unknown agent {i} on bg", c, p["bg"], 3) for i, c in enumerate(p["fallback"])]
    pairs += [(f"confetti {i} on bg", c, p["bg"], 2) for i, c in enumerate(p["confetti"])]
    for k in theme["skins"]:
        used = "".join(k["rows"])
        under = {"B": k["body"], "A": k["accent"], "F": k["face"]}[k["eye_on"]]
        pairs += [(f"{k['src']} body on bg", k["body"], p["bg"], 3), (f"{k['src']} body on waitFlash", k["body"], p["waitFlash"], 2),
                  (f"{k['src']} asleep on bg", dim(k["body"]), p["bg"], 2),
                  (f"{k['src']} eye", k["eye"], under, 3), (f"{k['src']} eye asleep", dim(k["eye"]), dim(under), 1.5)]
        if "A" in used:
            pairs.append((f"{k['src']} accent on bg", k["accent"], p["bg"], 3))
        if "F" in used:
            pairs.append((f"{k['src']} face on body", k["face"], k["body"], 1.5))
    return [(name, contrast(a, b), need) for name, a, b, need in pairs if contrast(a, b) < need]


def report_contrast(themes):
    for theme in themes:
        warnings = contrast_warnings(theme)
        print(f"contrast, theme {theme['id']} {theme['key']}: {len(warnings)} warnings")
        for name, ratio, need in warnings:
            print(f"  {name}: {ratio:.2f}, wanted {need}")


# ── Contact sheet ────────────────────────────────────────────────

MATRIX_STATES = [("idle", 600), ("thinking", 600), ("working", 600), ("waiting", 400), ("waiting", 600),
                 ("done", 600), ("error", 600), ("sleeping", 600)]   # waiting twice: flash off and on
MATRIX_SOURCES = ["CC", "CX", "OC", "PI", "DS", "WB", "ZZ"]


def matrix_frames(harness, theme, workdir):
    """Every agent in every state, plus one frame per preview and one with a toast."""
    scene, t = select_theme(theme["id"]), TOAST_WAIT_MS - TOAST_WAIT_MS % 1000 + 1000
    for src in MATRIX_SOURCES:
        for state, sample in MATRIX_STATES:
            scene += [{"t": t, "send": {"state": state, "session": "m", "name": "clawd-mood", "title": "固件主题", "src": src}},
                      {"t": t + sample, "record": True}, {"t": t + sample + 5, "record": False},
                      {"t": t + 900, "send": {"session": "m", "end": True}}]
            t += 1000
    scene.append({"t": t, "end": True})
    scene.sort(key=lambda l: l["t"])
    _, raw, _ = run_lines(harness, scene, workdir, f"matrix-{theme['id']}")
    return [raw[i:i + FRAME_BYTES] for i in range(0, len(raw), FRAME_BYTES)]


def toast_frame(harness, theme, workdir):
    meta, lines = read_scene("squad")
    scene = select_theme(theme["id"]) + shifted(lines, TOAST_WAIT_MS)
    at = TOAST_WAIT_MS + 1000
    scene += [{"t": at, "send": {"cfg": {"rotation": 2, "brightness": 70}}}, {"t": at + 300, "record": True},
              {"t": at + 305, "end": True}]
    scene.sort(key=lambda l: l["t"])
    return run_lines(harness, scene, workdir, f"toast-{theme['id']}")[1][:FRAME_BYTES]


def write_contact(harness, path, scale, only):
    themes = [t for t in load_themes(harness) if not only or t["id"] == only]
    margin, gap = 12, 4
    cell = W // scale
    cols = len(MATRIX_STATES)
    width = margin * 2 + cols * cell + (cols - 1) * gap
    blocks = []
    with tempfile.TemporaryDirectory() as tmp:
        for theme in themes:
            top = [render_preview(harness, theme, n, tmp)[0][:FRAME_BYTES] for n in PREVIEW_SCENES]
            top.append(toast_frame(harness, theme, tmp))
            matrix = matrix_frames(harness, theme, tmp)
            blocks.append((top, matrix))
    per_row_top = max(1, (width - margin * 2 + gap) // (W + gap))
    top_rows = 4 // per_row_top + (4 % per_row_top > 0)
    block_h = top_rows * (W + gap) + len(MATRIX_SOURCES) * (cell + gap) + margin
    height = margin + block_h * len(blocks)
    canvas = bytearray(b"\x20\x1c\x1a" * (width * height))

    def paste(frame, ox, oy, shrink):
        pixels = array("H")
        pixels.frombytes(frame)
        for y in range(0, H // shrink):
            at = (oy + y) * width * 3 + ox * 3
            for x in range(0, W // shrink):
                if shrink == 1:
                    r, g, b = rgb8(pixels[y * W + x])
                else:   # box filter, so scanlines average out instead of aliasing
                    samples = [rgb8(pixels[(y * shrink + j) * W + x * shrink + i])
                               for j in range(shrink) for i in range(shrink)]
                    r, g, b = (sum(s[k] for s in samples) // len(samples) for k in range(3))
                canvas[at + x * 3:at + x * 3 + 3] = bytes((r, g, b))

    y = margin
    for top, matrix in blocks:
        for i, frame in enumerate(top):
            paste(frame, margin + (i % per_row_top) * (W + gap), y + (i // per_row_top) * (W + gap), 1)
        y += top_rows * (W + gap)
        for i, frame in enumerate(matrix):
            paste(frame, margin + (i % cols) * (cell + gap), y + (i // cols) * (cell + gap), scale)
        y += len(MATRIX_SOURCES) * (cell + gap) + margin
    rows = b"".join(b"\0" + bytes(canvas[r * width * 3:(r + 1) * width * 3]) for r in range(height))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                           + png_chunk(b"IDAT", zlib.compress(rows, 6)) + png_chunk(b"IEND", b""))
    print(f"contact sheet -> {path} ({width}x{height})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="fail if previews or baseline.txt are out of date")
    ap.add_argument("--contact", metavar="PATH", help="write an overview image instead of the previews")
    ap.add_argument("--contact-scale", type=int, default=2, choices=(1, 2), help="1: agent matrix at full size")
    ap.add_argument("--theme", type=int, help="contact sheet of this theme only")
    ap.add_argument("--baseline", nargs="?", const=str(BASELINE), metavar="FILE", help="re-record frame hashes")
    ap.add_argument("--compare", metavar="FILE", help="list frames that differ from a recorded baseline")
    ap.add_argument("--sketch", metavar="DIR", default=str(FIRMWARE), help="firmware folder to compile")
    ap.add_argument("--define", action="append", default=[], metavar="NAME", help="extra -D for the firmware")
    args = ap.parse_args()
    harness = build_harness(Path(args.sketch).resolve(), [f"-D{d}" for d in args.define])
    if args.baseline:
        frames = record_all(harness)
        write_baseline(args.baseline, frames)
        print(f"{sum(map(len, frames.values()))} frames in {len(frames)} scenes -> {args.baseline}")
    elif args.compare:
        sys.exit(1 if compare(read_baseline(args.compare), record_all(harness)) else 0)
    elif args.contact:
        write_contact(harness, args.contact, args.contact_scale, args.theme)
        report_contrast(load_themes(harness))
    elif args.check:
        failed = False
        if compare(read_baseline(BASELINE), record_all(harness)):
            print("theme 1 no longer matches baseline.txt")
            failed = True
        files, manifest = render_all(harness)
        stale = check_previews(files, manifest)
        if stale:
            print("out of date, run render.py: " + ", ".join(stale))
            failed = True
        report_contrast(load_themes(harness))
        print("FAILED" if failed else f"ok: previews up to date, {len(files)} sheets")
        sys.exit(1 if failed else 0)
    else:
        files, manifest = render_all(harness)
        write_previews(files, manifest)
        report_contrast(load_themes(harness))


if __name__ == "__main__":
    main()
