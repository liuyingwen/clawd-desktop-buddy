# clawd-mood Wokwi 仿真

不接 ESP32 硬件、不焊屏幕，用 Wokwi 看固件表情：

```
echo '{"state":"..."}' > /tmp/clawd-mood.fifo → daemon-sim.py
                                                      ↓
                                              /tmp/wokwi-tty (socat 虚拟串口)
                                                      ↓
                                              wokwi-cli --interactive
                                                      ↓
                                              Wokwi 模拟 ESP32-C3 + LCD
```

真机路径是另一条：`hook.py` → TCP `127.0.0.1` → `plugin/scripts/daemon.py` → USB CDC。仿真 FIFO **不会**收到 hook 的输出。要在模拟器里看某一种表情，把对应的 `{"state":"..."}` 写进 FIFO。

**这个目录跟主项目完全隔离**——不动 `firmware/`、`plugin/`、`docs/`、`CLAUDE.md`、`AGENTS.md`。`rm -rf sim/` 就能撤回。

> **为啥另有一个 `daemon-sim.py`？**
> 真 `plugin/scripts/daemon.py` 是 TCP → USB CDC。它打开串口后会设 `ser.dtr=False / ser.rts=False`，避免 ESP32 被复位脉冲重启。这两根信号只有真 USB CDC 支持，socat 造的 pty 一设就 `OSError: Inappropriate ioctl for device`。
> `sim/daemon-sim.py` 因此做成 FIFO → 虚拟串口，跳过 DTR/RTS，只由 `sim.sh` 调用。

## 一次性准备

1. **系统包**

   ```bash
   brew install socat
   curl -fsSL https://wokwi.com/ci/install.sh | sh   # 装到 ~/.wokwi/bin/wokwi-cli
   ```

2. **Wokwi token**（免费）

   - 注册 https://wokwi.com
   - 去 https://wokwi.com/dashboard/ci 拿 CI token
   - 写进 shell：

     ```bash
     echo 'export WOKWI_CLI_TOKEN=wok_xxxxxxxx' >> ~/.zshrc
     source ~/.zshrc
     ```

3. （可选）静态校验配置：

   ```bash
   ~/.wokwi/bin/wokwi-cli lint sim/
   ```

## 跑起来

```bash
./sim/sim.sh
```

脚本自动：

1. 检查 `arduino-cli`、`wokwi-cli`、`socat`、`WOKWI_CLI_TOKEN` 齐了没
2. 如果真 daemon 在跑就拒绝启动（避免抢串口）
3. 用 `arduino-cli compile` 把固件编译到 `sim/build/`（不动 `firmware/`）
4. `socat` 造虚拟串口 `/tmp/wokwi-tty`
5. `wokwi-cli --interactive` 拉起模拟器
6. `daemon.py` 用 `CLAWD_MOOD_PORT=/tmp/wokwi-tty` 连虚拟串口
7. 终端打出 Wokwi 浏览器 URL，点开看屏幕

`Ctrl+C` 全部清理。

## 测什么

```bash
# daemon-sim → 固件。hook.py 走 TCP，不会写这条 FIFO
echo '{"state":"working"}' > /tmp/clawd-mood.fifo
echo '{"state":"done"}'    > /tmp/clawd-mood.fifo  # 3 秒后自动回 idle

# 7 种表情依次过一遍
for s in idle thinking working waiting done error sleeping; do
  echo "{\"state\":\"$s\"}" > /tmp/clawd-mood.fifo
  sleep 2
done
```

插件和仿真不要叠在一起跑。`claude --plugin-dir` 会让 `hook.py` 去连 TCP daemon，并在没有 daemon 时启动 `plugin/scripts/daemon.py`；`sim.sh` 启动前如果发现这个进程会直接退出。看 CLI 事件对应的脸时，按 README 的事件表把 `state` 写进 FIFO。

## 已知 gap（仿真器 vs 真硬件）

| 项 | 状况 |
|---|---|
| **显示驱动 IC** | Wokwi 没原生 ST7789 件，用 ILI9341 凑（SPI 协议同族）。Adafruit_ST7789 的初始化指令在 ILI9341 模型上可能颜色偏、原点偏，肉眼测够用，不能做像素级校验 |
| **屏幕尺寸** | ILI9341 是 240×320，固件按 240×240 画，图像只占屏幕上半部分 |
| **USB CDC 延迟** | socat pty 桥跟真 USB CDC 时序不一样，状态切换会有几十 ms 抖动 |
| **`Done → Idle 3s` 计时** | 固件内本地状态机，跟仿真器无关，时序准 |
| **`Sleeping 5min` 触发** | 同上，时序准；测时可临时把固件里 `SLEEP_IDLE_MS` 改 `15000UL` |
| **真 daemon 不能同时跑** | `sim.sh` 启动前会拒绝，避免抢 `/tmp/clawd-mood.fifo` 和串口 |
| **多次 `sim.sh` 同时跑** | 不支持，会撞 `/tmp/wokwi-tty` |

## 出错排查

| 症状 | 看这里 |
|---|---|
| `missing WOKWI_CLI_TOKEN` | 去 https://wokwi.com/dashboard/ci 拿，写进 `~/.zshrc` |
| `wokwi-cli not found` | `ls ~/.wokwi/bin/wokwi-cli` 没有就重装；有就说明 `sim.sh` 的 `PATH` 拼接出问题 |
| `pty did not appear` | `socat -d -d` 加多个 -d 看更详细日志；或单独跑 `wokwi-cli --interactive sim/` 看错在哪 |
| 屏幕黑屏不刷新 | 大概率是 ILI9341 模型不吃 ST7789 init 序列。改 `firmware/clawd_mood/clawd_mood.ino` 暂时换成 `Adafruit_ILI9341`（**仅当仿真用**，别提交） |
| 真 daemon 没关报错 | `pkill -f scripts/daemon.py` |
| 多个 ESP32 物理板子干扰 | sim 模式串口走虚拟 tty，不受物理板影响 |

## 清理

```bash
rm -rf sim/build/        # 编译产物（已 gitignore）
pkill -f wokwi-cli       # 残留进程
pkill -f scripts/daemon.py
rm -f /tmp/wokwi-tty /tmp/clawd-mood.fifo
```
