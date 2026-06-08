#!/usr/bin/env bash
# clawd-mood Wokwi simulator launcher
# 所有仿真状态都在 sim/ 里，主项目（firmware/ plugin/）零侵入
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SKETCH_DIR="$PROJECT_ROOT/firmware/clawd_mood"
BUILD_DIR="$SCRIPT_DIR/build"
WOKWI_TTY="/tmp/wokwi-tty"
DAEMON="$SCRIPT_DIR/daemon-sim.py"   # sim 替身，跳过 DTR/RTS（pty 不支持）
FBN="esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160"
SIM_TIMEOUT_MS="${SIM_TIMEOUT_MS:-86400000}"  # 24h, basically forever

# wokwi-cli 装在 ~/.wokwi/bin/，installer 的 symlink 在 ~/bin/ 但通常不在 PATH
export PATH="$HOME/.wokwi/bin:$PATH"

red()   { printf '\033[31m%s\033[0m\n' "$*" >&2; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
blue()  { printf '\033[34m%s\033[0m\n' "$*"; }

# ---------- preflight ----------
for tool in arduino-cli wokwi-cli socat; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    red "missing: $tool — 见 sim/README.md 安装说明"
    exit 1
  fi
done

if [ -z "${WOKWI_CLI_TOKEN:-}" ]; then
  red "missing WOKWI_CLI_TOKEN env var"
  echo "  1. 注册 https://wokwi.com" >&2
  echo "  2. https://wokwi.com/dashboard/ci 拿 token" >&2
  echo "  3. export WOKWI_CLI_TOKEN=...（建议写进 ~/.zshrc）" >&2
  exit 1
fi

if pgrep -f "scripts/daemon.py" >/dev/null 2>&1; then
  red "真 daemon 在跑，先停掉："
  echo "  pkill -f scripts/daemon.py" >&2
  exit 1
fi

# ---------- compile firmware ----------
blue "==> 编译固件 → $BUILD_DIR"
mkdir -p "$BUILD_DIR"
arduino-cli compile -b "$FBN" --output-dir "$BUILD_DIR" "$SKETCH_DIR"

if [ ! -f "$BUILD_DIR/clawd_mood.ino.elf" ]; then
  red "没找到 ELF：$BUILD_DIR/clawd_mood.ino.elf"
  ls -la "$BUILD_DIR" || true
  exit 1
fi

# ---------- cleanup hook ----------
PIDS=()
cleanup() {
  blue "==> 停止仿真"
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  rm -f "$WOKWI_TTY"
}
trap cleanup EXIT INT TERM

# ---------- bridge: socat ↔ wokwi-cli ----------
# socat 一端造 /tmp/wokwi-tty pty，另一端 EXEC 起 wokwi-cli
# wokwi-cli 的 stdin 被 socat 接管，--interactive 把 stdin 灌进模拟 USB CDC
blue "==> 启动 wokwi-cli + socat 桥接"
socat -d \
  PTY,link="$WOKWI_TTY",raw,echo=0 \
  "EXEC:wokwi-cli --interactive --quiet --timeout $SIM_TIMEOUT_MS '$SCRIPT_DIR',pty,raw,echo=0" &
PIDS+=($!)

# 等 pty symlink 出现
for _ in {1..30}; do
  [ -e "$WOKWI_TTY" ] && break
  sleep 0.2
done
if [ ! -e "$WOKWI_TTY" ]; then
  red "$WOKWI_TTY 6 秒内没出现，socat/wokwi-cli 启动失败"
  exit 1
fi

# ---------- start daemon pointed at virtual tty ----------
blue "==> 启动 daemon，串口指向 $WOKWI_TTY"
CLAWD_MOOD_PORT="$WOKWI_TTY" PYTHONUNBUFFERED=1 "$DAEMON" &
PIDS+=($!)

green "==> 就绪"
echo "  推状态：  echo '{\"state\":\"working\"}' > /tmp/clawd-mood.fifo"
echo "  喂事件：  echo '{\"hook_event_name\":\"PreToolUse\",\"tool_name\":\"Bash\"}' | $PROJECT_ROOT/plugin/scripts/hook.sh"
echo "  画面验证：CLI 是 headless 的，要看眼睛动画请用 https://wokwi.com 网页 IDE"
echo "  Ctrl+C 全停"
wait
