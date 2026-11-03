# 配置页与主题

**Date**: 2026-11-02

目标：不改代码、不重烧，就能在本地网页上切换屏幕外观、方向和亮度；加一套新外观只需要写一个文件；没收到过设置的设备和以前完全一样。

**硬件状态**：没有接板子验证过，见[未验证](#未验证)。

## 架构

```
浏览器 ── HTTP ──→ page/serve.py ── TCP ──┐
                                          ├──→ plugin/scripts/daemon.py ── USB CDC ──→ 固件 handleLine() ──→ THEMES[] + NVS
hook.py / 适配器 ──────────────── TCP ────┘

tools/preview/render.py ── 主机编译真实固件 ──→ page/previews/（雪碧图 + manifest.json）
```

数据流仍然单向。页面和 hook 走同一个 TCP 端口，互不感知；daemon 只转发。页面和 daemon 共用配置文件 `~/.clawd-mood/config.json`，见下文。

| 目录 | 内容 |
|---|---|
| `firmware/clawd_mood/` | `theme.h`（`Theme` 结构和 `THEME_TABLE`）、`theme_classic.cpp`、`theme_noir.cpp` |
| `tools/preview/` | `harness.cpp`、`shim/`、`scenes/*.ndjson`、`render.py`、`baseline.txt`、`headline.py`、`headline.cpp` |
| `page/` | `serve.py`、`index.html`、`page.css`、`page.js`、`assets/`、`previews/` |

## 主题

| id | key | 名称 | 风格 |
|---|---|---|---|
| 1 | `classic` | 经典 | 拆分前的外观，也是默认值 |
| 2 | `noir` | 午夜 | 深色地面 + 霓虹，无会话时是月亮星空动画 |

`Theme` 带调色板（按角色分，不借用状态色）、6 个 agent 的造型、Codex 帽子、图标集，以及可选绘制钩子 `cellBg` / `tile` / `idleScreen` / `overlay`，空钩子走内置绘制。

取舍：

- **编译进固件，不从主机上传**：协议单向，没有回包就没法可靠地传大块数据；一套主题还带绘制代码，不只是配色。代价是新主题要重烧一次。
- **元数据放在 `Theme` 里**：`tagline`、`glow`、`minFw` 只给页面用，占几个字节闪存，换来加主题不用改第二份列表。`render.py` 从编译好的固件读它们。`minFw` 目前没有固件版本号与之对照，两个主题都写 1。
- **id 只增不复用**：`cfg` 和 NVS 都存 id。NVS 里有当前固件不认识的 id（比如降级后）时画主题 1，但保留原值，升级回来还在。

## 设置行

```json
{"cfg":{"theme":2,"rotation":1,"brightness":60}}
```

键都可选，整行小于 319 字节，没有 `state` 字段，走 daemon 现有 TCP 端口。固件在任何会话逻辑之前处理并返回，不创建也不碰会话。

| 键 | 默认 | 不合法时 |
|---|---|---|
| `theme` | 1 | 未知 id 回退主题 1（并按主题 1 保存） |
| `rotation` | 1 | 不是 0–3 的整数就忽略 |
| `brightness` | 100 | 限制到 10–100 |

- **持久化**：`Preferences`（NVS）。`setup` 里首帧前加载；改动后防抖 1.5 秒，只写变化的键，所以拖滑块只写一次闪存，改了又改回去不写。
- **确认**：有变化时屏幕提示 3 秒，这是单向链路上唯一的确认。没变化不提示。
- **成功时不打串口**：没人读，TX 缓冲满了会卡主循环。
- **背光**：LEDC 5 kHz、8 位驱动 `TFT_BLK`，`ledcAttach` 失败时退回常亮。

## 预览：在主机上编译真实固件

页面上看到的每一帧都是真实固件画的，不是手绘图。`render.py` 用 Arduino 库目录里的 Adafruit GFX、U8g2、ArduinoJson 加 `shim/` 在主机编译 sketch，用虚拟时钟跑场景，经真实的 `handleLine` 送 `cfg` 选主题，把帧写成竖向雪碧图和 `manifest.json`。

选这个方案而不是手绘或画 CSS：外观只有一份来源，改了固件，预览跟着变，不会和设备对不上。

```json
{"themes":[{"id":1,"key":"classic","name":"经典","tagline":"...","min_fw":1,"glow":"#da7756",
  "scenes":{"squad":{"file":"theme-1/squad.png","frames":72,"fps":10,"w":240,"h":240}}}]}
```

- 预览场景是 `squad` / `solo` / `empty`，10 fps。每张图不超过 200 KB，`render.py` 强制检查末帧回到首帧，闭合不了或超限都直接报错。
- `baseline.txt` 是主题 1 全部 29 个场景 2957 帧的 hash。拆主题时用它证明外观没变：拆分前后 0 帧不同；之后单独修了翻页时来源标签折行到左边的残影（`canvas.setTextWrap(false)`），29 帧变化，全部在滑动中。
- `--contact` 出总览图，同时报告 RGB565 对比度。主题 2 是 0 条警告；主题 1 有 11 条，都是拆分前就有的颜色，被 baseline 锁住。
- 它是**主题作者手动跑的工具，不是 CI**，也不改变仓库「没有自动化测试」的约定。它验证的是帧缓冲内容，不是屏幕、背光、NVS 或真机帧时间。

## 配置页

```bash
uv run page/serve.py            # 打开浏览器
uv run page/serve.py --no-open  # 只打印地址
```

**放在 `plugin/` 外**：Codex 对整个插件目录算 hash，放进去的任何文件变动都会让用户重新批准 12 个 hook。`page/`、`tools/` 改动不影响已批准的 hook。

**视觉来自 Stitch 的「舞台中心板」方案，只当视觉来源**：不用它的代码、文案和外链资源。稿子写的 ESP32-S3 和 Wi-Fi 与本项目不符，页面按 ESP32-C3 + USB 写。CSP 不允许字体文件，页面用系统字体；标题 `assets/headline.png` 由 `tools/preview/headline.py` 用固件同款 wqy12 字体生成，整数倍放大。

**页面只说「已发送」，不说「已生效」**。协议单向，设备不回话，页面能看到的只到串口为止：

| 页面文案 | 实际含义 |
|---|---|
| 已发送，请看屏幕确认 | daemon 日志记录了把这行写进串口；如果日志不在预期位置（手动启动的 daemon），等 2.5 秒后，有设备且日志里没有串口错误也算 |
| 已保存，设备未连接，未发送 | 写入了配置文件，这行没发出去；下次「应用到屏幕」时补发（`unsent`） |

TCP 发送成功不算数：daemon 在设备拔掉时仍接受连接并丢弃消息，所以 `serve.py` 要看 daemon 的 `-> line` 日志。旧固件会忽略 `cfg`，页面照样显示已发送，因此有「屏幕没变化？」提示和「重新发送全部设置」按钮（设置没改动时「应用到屏幕」不会发任何东西）。

点卡带只改预览，不发串口；按「应用到屏幕」才发，并且只发变化的键。

**安全**：只绑 `127.0.0.1`，随机端口；256 位 token 放在 URL fragment（不进服务器日志），`/api/*` 要带 `X-Clawd-Token`；每个请求检查 Host 和 Origin，防 DNS rebinding；静态文件走白名单；所有响应带 CSP。

**生命周期**：10 分钟没有用户操作退出（状态轮询不算），最长 1 小时。

**配置文件**：`~/.clawd-mood/config.json`，用真实用户目录（POSIX 上用 `pwd.getpwuid`，因为 `$HOME` 可能被替换），可用 `CLAWD_MOOD_CONFIG` 覆盖。页面原子写入。

```json
{"serial_port":null,"last_sent":{"theme":1,"rotation":1,"brightness":100},"unsent":[]}
```

只有 `daemon.py`（读 `serial_port`）和页面读它，`hook.py` 和适配器不读。文件缺失或损坏时用默认值并记日志。`last_sent` 是页面最后想要的值，不是设备的真实状态；`unsent` 记发送失败的键，否则失败后再点应用会被当成没有变化。

## daemon 的配套改动（0.3.0）

- **热插拔**：写串口失败时关掉串口但不退出，TCP 继续收，消息丢弃。断开只记一次日志，恢复时带丢弃条数。之后的消息触发重新探测和打开，每 3 秒最多一次。已经连着 Espressif 板时，重连只等 Espressif 板，不会落到另一块插着的板子上。启动时没有设备仍然退出。
- **串口选择**：`CLAWD_MOOD_PORT` → 配置文件 `serial_port`（已枚举到才用，否则警告并扫描）→ 扫描（优先 VID `0x303A`，剩多个时警告并取排序后第一个）。
- **连接过滤**：第一个非空白字节不是 `{` 的连接直接丢弃，因为网页也能连这个端口。`recv` 超时 0.3 秒，发呆的客户端不会卡住串行循环。
- **`hooks-codex.json` 的命令加 `|| true`**，和 `hooks.json` 一致。

这些改动让插件版本升到 0.3.0，Codex 用户要重新批准 hook。

## 加一个主题

1. 复制 `firmware/clawd_mood/theme_noir.cpp`（要自定义绘制）或 `theme_classic.cpp`（只换配色和造型）为 `theme_<key>.cpp`。从 `theme_classic.cpp` 复制时删掉 `KNOT_LOGO` 的定义，它全局只有一份。
2. 改全局名和表头：`const Theme THEME_<KEY> = {3, "<key>", "<名称>", "<一句话风格>", 0xRRGGBB, 1, ...}`。id 取下一个未用过的数字；名称不超过 8 个汉字，且要在 GB2312 内，设备提示用的是同一款字体。
3. 在 `theme.h` 的 `THEME_TABLE` 末尾加 `X(THEME_<KEY>)`。不要动已有项的顺序，`THEMES[0]` 必须是主题 1。
4. 看效果和对比度：`uv run tools/preview/render.py --contact /tmp/contact.png --theme 3`。
5. 确认主题 1 没被碰到并生成预览：

   ```bash
   uv run tools/preview/render.py --compare tools/preview/baseline.txt
   uv run tools/preview/render.py
   uv run tools/preview/render.py --check
   ```

   某张图超过 200 KB 时报错，缩短对应 `scenes/<scene>.ndjson` 里的 `loop_ms`；闭合不了时报错，`loop_ms` 要是画面上动画周期的整数倍。这个时长是所有主题共用的。
6. 把新主题的 `Palette.state` 手工抄进 `page/assets/state-colors.json`（键是 id，顺序同文件里的 `order`）。`manifest.json` 里没有状态色，缺这一项时图例会停在上一个主题的颜色。这是页面里唯一一份手抄的数据，改调色板时也要同步。页面的卡带由 `manifest.json` 生成，不用改。
7. 编译（默认 + 带 `CLAWD_PROFILE`），烧到板子上看实物，读串口的 `[profile]` 行确认帧时间：

   ```bash
   arduino-cli compile --build-property compiler.cpp.extra_flags=-DCLAWD_PROFILE -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood
   ```

第 1–5 步在仓库的临时副本里演练过（复制 `theme_noir.cpp` 当主题 3）：主题 1 仍是 0 帧不同，生成前 `--check` 报过期，生成后通过，`manifest.json` 有 3 个主题。第 6 步没有演练，第 7 步的上板部分没做过。

## 未验证

**没有接板子，也没有烧录过。** 硬件行为全部未验证：

- 面板实际输出；`rotation` 在真 ST7789 上的方向；预览的旋转是否和面板一致。
- LEDC 调光；NVS 写入和断电保存。
- 真机帧时间。主题 2 每帧多画扫描线，帧时间没测过。主机上的 profile 数字是 0，因为时钟是虚拟的。
- 主机帧等于设备帧是假设：主机 `unsigned long` 是 64 位，设备是 32 位，算术只做过人工检查，没测 `millis()` 回绕。
- 真实的 USB 拔插和重新枚举，真实的 VID 读取。上面的 daemon 行为都在 pty 上测，部分场景伪造了串口枚举。
- `ESP_ARDUINO_VERSION_MAJOR < 3` 的 LEDC 分支没编译过，本机只有 core 3.3.10。

设备端能确定的只有编译结果：默认构建闪存 589904 字节（45%），全局变量 22176 字节（6%），另有堆上 115 KB 的画布。

**从源码推断，没有测量**：`setSPISpeed(40000000)` 在 `tft.init()` 之前调用，而 `init` 会按 Adafruit 默认的 32 MHz 重新初始化，所以这行看起来不起作用。按 32 MHz 算，推 115200 字节约 28.8 ms，留给滑动帧（50 ms）的绘制时间约 21 ms。带 `CLAWD_PROFILE` 上板后，串口的 `late=` 会显示实际超时帧数。

**其他平台**：Windows 上的 `saved_restart_manual` 分支和脱离进程启动没跑过；Linux 没跑过，没有 `lsof` 时换串口会提示手动重启。只用 headless Chrome 看过页面，页面依赖 `:has` 和 `color-mix`，Firefox、Safari 没试过。

**Codex 的 hash 范围是观察加推断**：本机 Codex 0.166.0 的 `config.toml` 里，本插件 12 个 hook 的 `trusted_content_hash` 完全相同，`trusted_hash` 各不相同，且前者不等于插件缓存里任何单个文件的 sha256。这和「按整个插件目录算」相符，但没有实际改一个文件再看 `/hooks` 的提示。
