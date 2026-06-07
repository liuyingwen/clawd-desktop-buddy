# clawd-mood

ESP32-C3 desk mascot that shows 7 pixel-eye expressions reflecting Claude Code state, over USB serial.

Built on [clawd-mochi](../projects/clawd-mochi/) hardware + [claudigotchi](../projects/claudigotchi/)-style plugin architecture.

## What it does

A Claude Code plugin hooks into session events and pipes the current state to an ESP32-C3 over USB serial. The ESP32 drives a 1.54" ST7789 TFT showing pixel-eye expressions:

| State           | Trigger                              | Look                                   |
| --------------- | ------------------------------------ | -------------------------------------- |
| Idle            | SessionStart, Stop after 3s, boot    | Normal eyes, slow wiggle + blink       |
| Thinking        | UserPromptSubmit                     | Eyes cycle up/left/right/center        |
| Working         | PreToolUse/PostToolUse/Subagent*     | Jitter + animated dots                 |
| Waiting         | Notification (Claude wants input)    | Wide eyes bouncing + `?`               |
| Done            | Stop (3s transient)                  | Squish smile `> <`                     |
| Error           | PostToolUseFailure                   | Asymmetric jittery eyes                |
| Sleeping        | 5 minutes of no events               | Closed-line eyes + floating `Z`        |

Any incoming state message wakes the device.

## Hardware

Identical to [clawd-mochi](../projects/clawd-mochi/):

| Part                | Spec                       |
| ------------------- | -------------------------- |
| ESP32-C3 Super Mini | with USB-C                 |
| ST7789 1.54" TFT    | 240×240 SPI                |
| Jumper wires ×8     | 8–10 cm                    |
| USB-C cable         | for data + power           |

### Wiring

| Display pin | ESP32-C3 GPIO  |
| ----------- | -------------- |
| VCC         | 3V3            |
| GND         | GND            |
| SDA         | GPIO 10 (MOSI) |
| SCL         | GPIO 8  (SCK)  |
| RES         | GPIO 2         |
| DC          | GPIO 1         |
| CS          | GPIO 4         |
| BL          | GPIO 3         |

⚠️ Connect VCC to **3.3V only** — never 5V.

## Setup

### Prerequisites

- macOS
- [uv](https://docs.astral.sh/uv/) — `brew install uv`
- jq — `brew install jq`
- Arduino IDE 2.x with the [ESP32 board package](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)
- Arduino libraries (Library Manager): **Adafruit GFX Library**, **Adafruit ST7735 and ST7789 Library**, **ArduinoJson**

### 1. Flash the firmware

Open `firmware/clawd_mood/clawd_mood.ino` in Arduino IDE.

Tools → Board → **ESP32C3 Dev Module**. Then:

| Setting           | Value                  |
| ----------------- | ---------------------- |
| USB CDC On Boot   | **Enabled** ← required |
| CPU Frequency     | 160 MHz                |
| Upload Speed      | 921600                 |

Pick the right Port, then Upload.

### 2. Start the daemon

```bash
chmod +x plugin/scripts/daemon.py
./plugin/scripts/daemon.py
```

Keep it running. First run, uv will auto-install pyserial into a managed environment.

Override the serial port with `CLAWD_MOOD_PORT=/dev/cu.usbmodemXXX ./plugin/scripts/daemon.py` if needed.

### 3. Launch Claude Code with the plugin

```bash
claude --plugin-dir /absolute/path/to/clawd-mood/plugin
```

Optional permanent alias:
```bash
alias claude='claude --plugin-dir /absolute/path/to/clawd-mood/plugin'
```

### 4. Verify

Run `/hooks` inside Claude Code. You should see all 9 hook events registered to `clawd-mood`. The display should already be on `thinking`.

## Serial protocol

Newline-delimited JSON at 115200 baud, single direction (Mac → ESP32). Minimum message:

```json
{"state":"working"}
```

Optional debug fields:

```json
{"state":"working","event":"PreToolUse","tool":"Bash"}
```

Accepted states: `idle | thinking | working | waiting | done | error | sleeping`.

## Manual testing without Claude Code

With the daemon running:

```bash
echo '{"state":"working"}' > /tmp/clawd-mood.fifo
echo '{"state":"done"}'    > /tmp/clawd-mood.fifo
```

Or open Arduino Serial Monitor at 115200 baud with line ending = Newline and type JSON directly.

## Architecture

```
Claude Code → hook.sh → /tmp/clawd-mood.fifo → daemon.py → USB serial → ESP32-C3
```

See `docs/superpowers/specs/2026-06-07-clawd-mood-design.md` for the full design.

## License

MIT
