// clawd-mood config page. Theme, rotation and brightness are staged here and previewed on the
// stage; only "apply" sends them. The device never answers, so nothing here may claim it changed.
"use strict";

// In the firmware's Mood order, which is also the order of each theme's state_colors in the manifest
const STATE_NAMES = {
  idle: "空闲", thinking: "思考", working: "工作", waiting: "等待",
  done: "完成", error: "错误", sleeping: "睡眠",
};
const ROTATION_LABEL = ["0°", "90°", "180°", "270°"];
const DEFAULTS = { theme: 1, rotation: 1, brightness: 100 };
const STAGE_SCENE = "squad";
const CART_SCENE = "solo";
const POLL_MS = 3000;
const TOUCH_THROTTLE_MS = 30000;
const FAILS_BEFORE_CLOSED = 2;
const FLASH_LINES = [
  "arduino-cli compile -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood",
  "arduino-cli upload -p <串口> -b esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,UploadSpeed=921600 firmware/clawd_mood",
];

const $ = (id) => document.getElementById(id);
const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");

const state = {
  token: null,
  themes: [],
  staged: { ...DEFAULTS },
  baseline: { ...DEFAULTS },   // what the device was last told, as far as this machine knows
  unsent: [],
  status: null,
  closed: false,
  statusSeen: false,
  fails: 0,
  lastTouch: 0,
  serialTouched: false,
};

// ── Token: fragment -> tab storage, then out of the address bar ──

function readToken() {
  const match = location.hash.match(/^#token=([\w-]+)$/);
  if (match) {
    sessionStorage.setItem("clawd-token", match[1]);
    history.replaceState(null, "", location.pathname);
  }
  return sessionStorage.getItem("clawd-token");
}

async function api(path, body) {
  const options = { headers: { "X-Clawd-Token": state.token || "" } };
  if (body !== undefined) {
    options.method = "POST";
    options.headers["Content-Type"] = "application/json";
    options.body = JSON.stringify(body);
  }
  const res = await fetch(path, options);
  let data = {};
  try {
    data = await res.json();
  } catch {
    // an empty or non-JSON error still has a status code
  }
  return { status: res.status, data };
}

// ── Sprite playback: one clock for the stage and every cartridge ──

const players = [];

function makePlayer(canvas, sceneMeta, mirror) {
  const ctx = canvas.getContext("2d");
  ctx.imageSmoothingEnabled = false;
  const player = { canvas, ctx, mirror, image: null, meta: sceneMeta, frame: -1, started: 0 };
  if (mirror) {
    player.mirrorCtx = mirror.getContext("2d");
    player.mirrorCtx.imageSmoothingEnabled = false;
  }
  players.push(player);
  return player;
}

const imageCache = new Map();

function loadSheet(file) {
  if (!imageCache.has(file)) {
    imageCache.set(file, new Promise((resolve, reject) => {
      const img = new Image();
      img.onload = () => resolve(img);
      img.onerror = reject;
      img.src = "previews/" + file;
    }));
  }
  return imageCache.get(file);
}

async function playScene(player, sceneMeta) {
  player.meta = sceneMeta;
  const img = await loadSheet(sceneMeta.file);
  if (player.meta !== sceneMeta) return;  // switched again while this one loaded
  player.image = img;
  player.frame = -1;
  player.started = performance.now();
  drawFrame(player, 0);
}

function drawFrame(player, frame) {
  if (!player.image || frame === player.frame) return;
  player.frame = frame;
  const { w, h } = player.meta;
  player.ctx.drawImage(player.image, 0, frame * h, w, h, 0, 0, player.canvas.width, player.canvas.height);
  if (player.mirrorCtx) {
    player.mirrorCtx.drawImage(player.canvas, 0, 0);
  }
}

let rafId = 0;

function tick(now) {
  rafId = requestAnimationFrame(tick);
  for (const player of players) {
    if (!player.image) continue;
    const { fps, frames } = player.meta;
    drawFrame(player, Math.floor(((now - player.started) / 1000) * fps) % frames);
  }
}

function syncPlayback() {
  cancelAnimationFrame(rafId);
  rafId = 0;
  if (reducedMotion.matches) {
    for (const player of players) drawFrame(player, 0);
  } else {
    rafId = requestAnimationFrame(tick);
  }
}

// ── Staged values on the stage ───────────────────────────────────

const root = document.documentElement;
let stagePlayer = null;
let flashTimer = 0;

function themeById(id) {
  return state.themes.find((t) => t.id === id);
}

// The oldest firmware any theme accepts. A theme above it needs newer firmware, and older firmware
// shows the first theme instead and says so in its toast.
function baseFirmware() {
  return Math.min(...state.themes.map((t) => t.min_fw));
}

function needsNewerFirmware(theme) {
  return Boolean(theme) && theme.min_fw > baseFirmware();
}

function fallbackTheme() {
  return themeById(DEFAULTS.theme) || state.themes[0];
}

const cartNo = (theme) => String(theme.id).padStart(2, "0");

function setTheme(id, animate) {
  const theme = themeById(id);
  if (!theme) return;
  state.staged.theme = id;
  root.style.setProperty("--glow", theme.glow);
  root.dataset.theme = theme.key;
  if (stagePlayer) playScene(stagePlayer, theme.scenes[STAGE_SCENE]);
  for (const input of document.querySelectorAll('input[name="theme"]')) {
    input.checked = Number(input.value) === id;
  }
  paintChips(id);
  $("read-theme").textContent = theme.name;
  if (animate && !reducedMotion.matches) {
    const screen = $("screen");
    screen.classList.remove("is-swapping");
    void screen.offsetWidth;  // restart the animation on quick repeat switches
    screen.classList.add("is-swapping");
    clearTimeout(flashTimer);
    flashTimer = setTimeout(() => screen.classList.remove("is-swapping"), 520);
  }
  refreshPending();
}

function setRotation(rotation) {
  state.staged.rotation = rotation;
  // The firmware's default rotation 1 is how the panel sits upright, so that one shows unturned
  const turn = (rotation - 1) * 90;
  root.style.setProperty("--turn", turn + "deg");
  for (const input of document.querySelectorAll('input[name="rotation"]')) {
    input.checked = Number(input.value) === rotation;
  }
  $("read-rotation").textContent = ROTATION_LABEL[rotation];
  refreshPending();
}

function setBrightness(value) {
  state.staged.brightness = value;
  const fraction = value / 100;
  root.style.setProperty("--lum", String(0.16 + 0.84 * fraction));
  root.style.setProperty("--fill", ((value - 10) / 90) * 100 + "%");
  $("brightness").value = String(value);
  $("brightness-value").textContent = String(value);
  $("read-brightness").textContent = value + "%";
  refreshPending();
}

function pendingKeys() {
  return Object.keys(DEFAULTS).filter(
    (k) => state.staged[k] !== state.baseline[k] || state.unsent.includes(k),
  );
}

function refreshPending() {
  const count = pendingKeys().length;
  $("read-pending").textContent = count + " 项";
  $("read-pending").classList.toggle("is-pending", count > 0);
  $("apply").classList.toggle("has-pending", count > 0);
  if (!state.statusHeld) {
    setStatus(count > 0 ? `有 ${count} 项改动，只在本页预览，还没发送` : "没有待发送的改动");
  }
}

function setStatus(text, detail, tone, hold) {
  $("status").textContent = text;
  $("status-detail").textContent = detail || "";
  $("status").dataset.tone = tone || "";
  state.statusHeld = Boolean(hold);
}

// ── Cartridges and chips ─────────────────────────────────────────

function buildCartridges() {
  const holder = $("cartridges");
  const more = $("cart-more");
  const template = $("cart-template");
  holder.style.setProperty("--carts", String(state.themes.length));
  for (const theme of state.themes) {
    const node = template.content.firstElementChild.cloneNode(true);
    const input = node.querySelector("input");
    const newer = needsNewerFirmware(theme);
    input.value = String(theme.id);
    input.setAttribute("aria-label", `${cartNo(theme)} ${theme.name}${newer ? "，需新版固件" : ""}`);
    node.querySelector(".cart__no").textContent = cartNo(theme);
    node.querySelector(".cart__name").textContent = theme.name;
    node.querySelector(".cart__tagline").textContent = theme.tagline || "";
    node.querySelector(".cart__req").hidden = !newer;
    node.style.setProperty("--cart-glow", theme.glow);
    input.addEventListener("change", () => {
      if (input.checked) {
        state.statusHeld = false;
        setTheme(theme.id, true);
      }
    });
    holder.insertBefore(node, more);
    const scene = theme.scenes[CART_SCENE] || theme.scenes[STAGE_SCENE];
    playScene(makePlayer(node.querySelector("canvas"), scene), scene);
  }
}

function buildChips() {
  const holder = $("chips");
  for (const key of Object.keys(STATE_NAMES)) {
    const li = document.createElement("li");
    li.className = "chip";
    li.dataset.state = key;
    const led = document.createElement("i");
    const name = document.createElement("span");
    name.className = "chip__name";
    name.textContent = STATE_NAMES[key];
    const code = document.createElement("span");
    code.className = "chip__code";
    code.textContent = key;
    li.append(led, name, code);
    holder.append(li);
  }
}

function paintChips(themeId) {
  const theme = themeById(themeId);
  const colors = theme && theme.state_colors;
  if (!colors) return;
  Object.keys(STATE_NAMES).forEach((key, i) => {
    const chip = document.querySelector(`.chip[data-state="${key}"]`);
    if (chip && colors[i]) chip.style.setProperty("--chip", colors[i]);
  });
}

// The page cannot know which firmware the board runs, so this only says what each case looks like
function paintFirmwareHint() {
  const newer = state.themes.filter(needsNewerFirmware);
  const line = $("fw-themes");
  line.hidden = newer.length === 0;
  if (!newer.length) return;
  const names = newer.map((t) => `${cartNo(t)} ${t.name}`).join("、");
  const fallback = fallbackTheme().name;
  line.textContent = `${names} 需要新版固件。固件较旧时收到这些样式会改用${fallback}：原来是${fallback}的屏幕不会有任何变化，`
    + `原来是别的样式的会切到${fallback}并提示「主题 ${fallback}」。更新固件后按下面的按钮再发一次。`;
}

// ── Status poll: LEDs, drawer, serial list ───────────────────────

function setLed(id, tone, label) {
  const led = $(id);
  led.dataset.tone = tone;
  led.title = label;
  led.setAttribute("aria-label", led.querySelector("span").textContent + "：" + label);
}

const PLUGIN_NAMES = { claude_code: "Claude Code", codex: "Codex", workbuddy: "WorkBuddy" };
const PLUGIN_STATES = { found: "已找到", absent: "未安装", unknown: "未知" };

function paintLeds(s) {
  const traces = Object.values(s.plugins);
  const found = traces.some((p) => p.state === "found");
  setLed("led-plugin", found ? "ok" : "warn", found ? "找到了插件痕迹" : "没读到插件痕迹，--plugin-dir 方式这里看不到");
  setLed("led-daemon", s.daemon.alive ? "ok" : "bad", s.daemon.alive ? `运行中，端口 ${s.daemon.port}` : "没在运行");
  const usbOk = s.serial.usb_ok && !s.daemon.link_down;
  setLed("led-usb", usbOk ? "ok" : "bad", usbOk ? "找到了串口" : "没找到设备，或串口写不进去");
}

function portDetail(c) {
  const lines = [c.device];
  if (c.vid || c.pid) lines.push(`VID ${c.vid || "----"}  PID ${c.pid || "----"}`);
  if (c.description) lines.push(c.description);
  if (c.serial_number) lines.push(`SN ${c.serial_number}`);
  return lines.join("\n");
}

function paintSerial(s) {
  const select = $("serial");
  const wanted = state.serialTouched ? select.value : s.serial.configured || "";
  const devices = s.serial.candidates.map((c) => c.device);
  const extra = s.serial.configured && !devices.includes(s.serial.configured) ? [s.serial.configured] : [];
  const signature = JSON.stringify([devices, extra]);
  if (select.dataset.signature !== signature) {
    select.dataset.signature = signature;
    select.replaceChildren(new Option("自动选择", ""));
    for (const c of s.serial.candidates) {
      select.append(new Option(c.description ? `${c.device} · ${c.description}` : c.device, c.device));
    }
    for (const device of extra) select.append(new Option(`${device} · 未连接`, device));
  }
  select.value = wanted;
  if (select.selectedIndex < 0) select.value = "";
  paintPortInfo();
  const device = select.value || (s.serial.candidates.find((c) => c.likely) || {}).device;
  $("flash-command").textContent = FLASH_LINES.join("\n").replace("<串口>", device || "<串口>");
}

function paintPortInfo() {
  const s = state.status;
  if (!s) return;
  const value = $("serial").value;
  const info = $("port-info");
  const running = s.daemon.serial ? `后台当前: ${s.daemon.serial}\n` : "";
  if (!value) {
    const likely = s.serial.candidates.find((c) => c.likely);
    if (s.serial.env_override) {
      info.textContent = `${running}自动会选: ${s.serial.env_override}\n来自环境变量 CLAWD_MOOD_PORT`;
    } else if (likely) {
      info.textContent = `${running}自动会选: ${portDetail(likely)}`;
    } else {
      info.textContent = `${running}自动会选: 没有符合的设备`;
    }
  } else {
    const c = s.serial.candidates.find((x) => x.device === value);
    info.textContent = running + (c ? portDetail(c) : `${value}\n现在没有连接`);
  }
}

function row(label, value, tone) {
  const el = document.createElement("div");
  el.className = "diag-row";
  const key = document.createElement("span");
  key.className = "diag-row__key";
  key.textContent = label;
  const val = document.createElement("span");
  val.className = "diag-row__value";
  if (tone) val.dataset.tone = tone;
  val.textContent = value;
  el.append(key, val);
  return el;
}

function group(title, children) {
  const box = document.createElement("section");
  box.className = "diag-group";
  const h = document.createElement("h3");
  h.textContent = title;
  box.append(h, ...children);
  return box;
}

function paintDrawer(s) {
  $("diag-break").textContent = s.first_break ? s.first_break.explain : "目前没有发现断点。屏幕上的实际画面本页读不到，请看实物。";
  $("diag-break").dataset.tone = s.first_break ? "bad" : "ok";
  const plugins = Object.entries(s.plugins).map(([key, p]) =>
    row(PLUGIN_NAMES[key] || key, `${PLUGIN_STATES[p.state] || p.state}  ${p.detail || ""}`,
      p.state === "found" ? "ok" : undefined));
  const ports = s.serial.candidates.length
    ? s.serial.candidates.map((c) => row(c.likely ? "候选" : "其他", portDetail(c), c.likely ? "ok" : undefined))
    : [row("候选", "没有枚举到串口")];
  const errors = s.log.errors.length
    ? s.log.errors.map((line) => row("日志", line, "bad"))
    : [row("日志", "没有最近的错误")];
  $("diag-body").replaceChildren(
    group("插件痕迹", plugins),
    group("后台", [
      row("状态", s.daemon.alive ? `运行中，端口 ${s.daemon.port}` : "没在运行", s.daemon.alive ? "ok" : "bad"),
      row("端口文件", `${s.daemon.portfile}  (${s.daemon.portfile_port ?? "无"})`),
      row("当前串口", s.daemon.serial || "未知"),
      row("uv", s.uv.on_path ? s.uv.path : "不在 PATH 里", s.uv.on_path ? "ok" : "bad"),
    ]),
    group("串口", [
      row("已保存", s.serial.configured || "自动选择"),
      ...(s.serial.env_override ? [row("环境变量", s.serial.env_override)] : []),
      ...ports,
    ]),
    group("上次发送", [
      row("内容", JSON.stringify(s.last_sent)),
      row("未送达", s.unsent.length ? s.unsent.join(", ") : "无"),
      row("配置文件", s.config_path),
    ]),
    group("后台日志", [row("路径", s.log.path), ...errors]),
  );
}

function paintTimeLeft(s) {
  const left = Math.min(s.lifetime.idle_left_s, s.lifetime.hard_left_s);
  $("time-left").textContent = left <= 120 ? `还剩 ${left} 秒` : "";
}

function showClosed(why) {
  if (state.closed) return;
  state.closed = true;
  $("closed-title").textContent = why.title;
  $("closed-text").textContent = why.text;
  $("closed").hidden = false;
  document.body.classList.add("is-closed");
  for (const button of document.querySelectorAll("button, select, input")) button.disabled = true;
  if ($("drawer").open) $("drawer").close();
  // A send result already on screen stays true after the server exits, so keep it
  if (!state.statusHeld) setStatus(why.label, "", "bad", true);
  for (const id of ["led-plugin", "led-daemon", "led-usb"]) setLed(id, "off", why.label);
}

const CLOSED = {
  title: "本页已关闭",
  label: "页面服务已退出",
  text: "页面服务已退出：10 分钟无操作，或已运行满 1 小时。设备不受影响。要继续配置，在仓库目录重新运行：",
};
const NO_TOKEN = {
  title: "本页无法连接",
  label: "访问令牌无效",
  text: "这个页面的访问令牌无效。请回到终端，打开 serve.py 打印的完整地址，或重新运行：",
};

async function poll() {
  if (state.closed) return;
  try {
    const { status, data } = await api("/api/status");
    if (status === 401) {
      showClosed(NO_TOKEN);
      return;
    }
    if (status !== 200) throw new Error(String(status));
    state.fails = 0;
    state.status = data;
    state.baseline = data.last_sent;
    state.unsent = data.unsent;
    if (!state.statusSeen) {
      state.statusSeen = true;
      setTheme(themeById(data.last_sent.theme) ? data.last_sent.theme : DEFAULTS.theme, false);
      setRotation(data.last_sent.rotation);
      setBrightness(data.last_sent.brightness);
    }
    refreshPending();
    paintLeds(data);
    paintSerial(data);
    paintDrawer(data);
    paintTimeLeft(data);
  } catch {
    state.fails += 1;
    if (state.fails >= FAILS_BEFORE_CLOSED) showClosed(CLOSED);
  }
}

// ── Actions ──────────────────────────────────────────────────────

// Only these reasons mean the device is missing; for the rest the explain line names the break
const NO_DEVICE_REASONS = ["no_device", "serial_open_failed", "device_gone"];

function notSentHeadline(reason) {
  return NO_DEVICE_REASONS.includes(reason) ? "设备未连接，未发送" : "未发送";
}

function setBusy(button, busy) {
  button.classList.toggle("is-busy", busy);
  button.disabled = busy || state.closed;
}

async function sendCfg(button, force) {
  setBusy(button, true);
  setStatus("发送中…如果后台没开，会先启动它", "", "", true);
  try {
    const { status, data } = await api("/api/cfg", { ...state.staged, force });
    if (status === 200 && data.result === "sent") {
      const sentTheme = data.changed && themeById(data.changed.theme);
      const detail = needsNewerFirmware(sentTheme)
        ? `如果屏幕没变化，或提示「主题 ${fallbackTheme().name}」，说明固件还不认识${sentTheme.name}，见下方「屏幕没变化，或样式不对？」。`
        : "";
      setStatus("已发送，请看屏幕确认", detail, "ok", true);
    } else if (status === 200 && data.result === "saved_not_sent") {
      setStatus(`已保存，${notSentHeadline(data.reason)}`, data.explain, "bad", true);
    } else if (status === 200 && data.result === "no_change") {
      setStatus("没有需要发送的改动", "", "", true);
    } else {
      setStatus("没有发送", data.error || `服务返回 ${status}`, "bad", true);
    }
    if (data.last_sent) {
      state.baseline = data.last_sent;
      state.unsent = data.unsent || [];
      refreshPending();
    }
  } catch {
    setStatus("没有发送", "连不上页面服务", "bad", true);
    poll();
  } finally {
    setBusy(button, false);
  }
}

async function sendTest() {
  const button = $("test");
  setBusy(button, true);
  setStatus("发送中…如果后台没开，会先启动它", "", "", true);
  try {
    const { status, data } = await api("/api/test", {});
    if (status === 200 && data.result === "sent") {
      setStatus("已发送，请看屏幕确认", "测试小人会工作 2 秒，完成 3 秒，然后离开。", "ok", true);
    } else if (status === 200 && data.result === "busy") {
      setStatus("测试小人还在屏幕上", "等它离开再发一次。", "", true);
    } else if (status === 200 && data.result === "not_sent") {
      setStatus(notSentHeadline(data.reason), data.explain, "bad", true);
    } else {
      setStatus("没有发送", data.error || `服务返回 ${status}`, "bad", true);
    }
  } catch {
    setStatus("没有发送", "连不上页面服务", "bad", true);
    poll();
  } finally {
    setBusy(button, false);
  }
}

async function saveSerial() {
  const button = $("save-serial");
  const result = $("serial-result");
  setBusy(button, true);
  result.textContent = "保存中，正在重启后台…";
  result.dataset.tone = "";
  try {
    const port = $("serial").value || null;
    const { status, data } = await api("/api/serial", { port });
    if (status === 200 && data.result === "restarted") {
      result.textContent = `已保存，后台已重启${data.serial ? `，正在用 ${data.serial}` : ""}`;
      result.dataset.tone = "ok";
    } else if (status === 200 && data.result === "saved_not_restarted") {
      result.textContent = `已保存，后台没有起来。${data.explain}`;
      result.dataset.tone = "bad";
    } else if (status === 200 && data.result === "saved_restart_manual") {
      result.textContent = data.explain;
    } else {
      result.textContent = data.error || `服务返回 ${status}`;
      result.dataset.tone = "bad";
    }
    state.serialTouched = false;
    poll();
  } catch {
    result.textContent = "连不上页面服务";
    result.dataset.tone = "bad";
    poll();
  } finally {
    setBusy(button, false);
  }
}

// Staging on the page is a user action too, though nothing reaches the server by itself
function touch() {
  const now = Date.now();
  if (state.closed || !state.token || now - state.lastTouch < TOUCH_THROTTLE_MS) return;
  state.lastTouch = now;
  api("/api/touch", {}).catch(() => {});
}

function bindControls() {
  $("apply").addEventListener("click", () => sendCfg($("apply"), false));
  $("resend").addEventListener("click", () => sendCfg($("resend"), true));
  $("test").addEventListener("click", sendTest);
  $("save-serial").addEventListener("click", saveSerial);
  $("serial").addEventListener("change", () => {
    state.serialTouched = true;
    paintPortInfo();
  });
  for (const input of document.querySelectorAll('input[name="rotation"]')) {
    input.addEventListener("change", () => {
      state.statusHeld = false;
      setRotation(Number(input.value));
    });
  }
  $("brightness").addEventListener("input", (e) => {
    state.statusHeld = false;
    setBrightness(Number(e.target.value));
  });
  $("open-diag").addEventListener("click", () => {
    if (state.status) paintDrawer(state.status);
    $("drawer").showModal();
  });
  $("close-diag").addEventListener("click", () => $("drawer").close());
  $("drawer").addEventListener("click", (e) => {
    if (e.target === $("drawer")) $("drawer").close();  // the backdrop
  });
  document.addEventListener("pointerdown", touch, { passive: true });
  document.addEventListener("keydown", touch);
  reducedMotion.addEventListener("change", syncPlayback);
}

// ── Boot ─────────────────────────────────────────────────────────

async function boot() {
  state.token = readToken();
  bindControls();
  buildChips();
  $("flash-command").textContent = FLASH_LINES.join("\n");
  try {
    const manifest = await fetch("previews/manifest.json").then((r) => r.json());
    state.themes = Array.isArray(manifest.themes) ? manifest.themes : [];
  } catch {
    state.themes = [];
  }
  buildCartridges();
  paintFirmwareHint();
  stagePlayer = makePlayer($("stage-canvas"), null, $("reflect-canvas"));
  setTheme(DEFAULTS.theme, false);
  setRotation(DEFAULTS.rotation);
  setBrightness(DEFAULTS.brightness);
  syncPlayback();
  // Without the manifest there are no cartridges, but the status poll and the other settings still work
  if (!state.themes.length) setStatus("预览清单读不到，请重新运行 tools/preview/render.py", "", "bad", true);
  if (!state.token) {
    showClosed(NO_TOKEN);
    return;
  }
  await poll();
  setInterval(poll, POLL_MS);
}

boot();
