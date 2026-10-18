// ============================================================================
// Marshland 控制面板 — 真实接口版
//
// 数据通道（ATOM-16）：
//   状态/电机实时显示：EventSource GET /api/stream（50ms 一帧 status 事件）
//   摇杆控制数据：    POST /api/control/channels（仅控制源为 WEB 时服务端受理）
//
// 摇杆 → WebData 通道映射：蓄力→CH2、换弹→CH3、YAW→CH4、变力→CH12
// 控制权 → CH5：< 0 = AUTO，>= 0 = WEB（仅符号有意义）
// ============================================================================

const $ = (id) => document.getElementById(id);

// ---------- 连接与状态渲染 ----------
function setConn(online) {
  $("connDot").classList.toggle("online", online);
  $("connText").textContent = online ? "WEB 已连接" : "未连接";
}

function setValue(id, text, colorClass) {
  const el = $(id);
  if (!el || el.textContent === text) return;
  el.textContent = text;
  el.classList.remove("flash");
  void el.offsetWidth;
  el.classList.add("flash");
  if (colorClass) el.className = `stat-value ${colorClass} flash`;
}

function renderMotors(motors) {
  document.querySelectorAll("#motorGrid .motor").forEach((el, i) => {
    const m = motors && motors[i];
    const rpmEl = el.querySelector(".m-rpm");
    const tempEl = el.querySelector(".m-temp");
    const dot = el.querySelector(".m-dot");
    if (!m || m.rpm == null) {
      rpmEl.textContent = "--";
      tempEl.textContent = "-- ℃";
      dot.className = "m-dot";
      el.classList.remove("offline");
      return;
    }
    rpmEl.textContent = `${m.rpm}`;
    tempEl.textContent = `${m.temperature} ℃`;
    tempEl.classList.toggle("hot", m.temperature >= 80);
    dot.className = "m-dot" + (m.online ? " on" : "");
    el.classList.toggle("offline", !m.online);
  });
}

function renderStatus(d) {
  if (d.control_source) setValue("ctrlSrc", d.control_source);
  if (d.system_state) setValue("sysMode", d.system_state, "accent");
  if (typeof d.estop === "boolean") {
    setValue("stopFire", d.estop ? "急停" : "正常", d.estop ? "warn" : "ok");
  }
  if (Array.isArray(d.motors) || d.motors === null) renderMotors(d.motors);
}

const es = new EventSource("/api/stream");
es.addEventListener("status", (e) => renderStatus(JSON.parse(e.data)));
es.onopen = () => { setConn(true); pushLog("已连接 /api/stream 实时推送"); };
es.onerror = () => setConn(false);   // EventSource 自动重连

// ---------- 摇杆（YAW 横向，其余纵向；±1000 线性映射，接真实接口） ----------
const JOY_CHANNEL = { "蓄力": 2, "换弹": 3, "YAW": 4, "变力": 12 };
const SEND_INTERVAL_MS = 50;

async function sendChannel(ch, value) {
  try {
    const res = await fetch("/api/control/channels", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ channels: { [String(ch)]: value } }),
    });
    const data = await res.json();
    if (!data.ok) pushLog(`控制被拒绝：${data.error?.code ?? res.status}`);
  } catch {
    pushLog("控制发送失败：网络不可达");
  }
}

function setupJoystick(el) {
  const max = Number(el.dataset.max);
  const name = el.dataset.name;
  const channel = JOY_CHANNEL[name];
  const horizontal = el.dataset.axis === "x";   // YAW 横向，其余纵向
  const knob = el.querySelector(".joy-knob");
  const readout = el.parentElement.querySelector(".joy-readout b");
  const meter = el.parentElement.querySelector(".joy-meter .fill");

  let dragging = false;
  let lastSent = 0;
  let lastValue = null;

  const travel = () => horizontal
    ? el.clientWidth / 2 - knob.offsetWidth / 2 - 6
    : el.clientHeight / 2 - knob.offsetHeight / 2 - 6;

  function send(value, force) {
    const now = performance.now();
    if (!force && now - lastSent < SEND_INTERVAL_MS) return;
    if (!force && value === lastValue) return;
    lastSent = now;
    lastValue = value;
    sendChannel(channel, value);
  }

  function apply(d) {
    const t = travel();
    d = Math.max(-t, Math.min(t, d));      // 单轴限幅，另一轴锁定
    knob.style.transform = horizontal
      ? `translate(calc(-50% + ${d}px), -50%)`
      : `translate(-50%, calc(-50% + ${d}px))`;

    // 线性映射，满偏 = ±max；纵向以上推为正，横向以右推为正
    const value = Math.round((horizontal ? d : -d) / t * max);
    readout.textContent = value;
    readout.className = value > 0 ? "pos" : value < 0 ? "neg" : "";

    const ratio = value / max;
    meter.style.width = `${Math.abs(ratio) * 50}%`;
    meter.style.left = ratio >= 0 ? "50%" : `${50 - Math.abs(ratio) * 50}%`;
    meter.classList.toggle("neg", ratio < 0);

    send(value, false);
  }

  function reset() {
    knob.style.transform = "translate(-50%, -50%)";
    readout.textContent = "0";
    readout.className = "";
    meter.style.width = "0";
    meter.style.left = "50%";
    lastValue = null;
    send(0, true);   // 松手回中必须发 0
  }

  const offsetOf = (e) => {
    const r = el.getBoundingClientRect();
    return horizontal ? e.clientX - (r.left + r.width / 2)
                      : e.clientY - (r.top + r.height / 2);
  };

  el.addEventListener("pointerdown", (e) => {
    dragging = true;
    el.classList.add("active");
    el.setPointerCapture(e.pointerId);
    apply(offsetOf(e));
  });
  el.addEventListener("pointermove", (e) => {
    if (!dragging) return;
    apply(offsetOf(e));
  });
  const release = (e) => {
    if (!dragging) return;
    dragging = false;
    el.classList.remove("active");
    if (el.hasPointerCapture?.(e.pointerId)) el.releasePointerCapture(e.pointerId);
    reset();
  };
  el.addEventListener("pointerup", release);
  el.addEventListener("pointercancel", release);
}

document.querySelectorAll(".joy").forEach(setupJoystick);

// ---------- 控制权切换（CH5：<0 = AUTO，>=0 = WEB） ----------
const CH5_AUTO = -1;
const CH5_WEB = 0;
let ch5IsWeb = false;   // 页面初始不写 CH5；首次点击切到 WEB

$("ch5Btn").addEventListener("click", () => {
  ch5IsWeb = !ch5IsWeb;
  sendChannel(5, ch5IsWeb ? CH5_WEB : CH5_AUTO);
  $("ch5Btn").textContent = ch5IsWeb ? "控制权 → AUTO" : "控制权 → WEB";
  pushLog(`控制权已请求：${ch5IsWeb ? "WEB" : "AUTO"}`);
});

// ---------- 底部日志 ----------
function pushLog(text) {
  const el = $("logText");
  el.textContent = text;
  el.classList.remove("log-swap");
  void el.offsetWidth;
  el.classList.add("log-swap");
}

function tickClock() {
  const t = new Date().toLocaleTimeString("zh-CN", { hour12: false });
  $("clock").textContent = t;
  $("logTime").textContent = t;
}

// 手动控制按钮：供弹 CH13 / 发射 CH7（-100 保持后恢复 100）
$("btnFeed").addEventListener("click", () => {
  sendChannel(13, -100);
  setTimeout(() => sendChannel(13, 100), 100);
});
$("btnFire").addEventListener("click", () => {
  sendChannel(7, -100);
  setTimeout(() => sendChannel(7, 100), 1000);
});
$("estopBtn").addEventListener("click", () => pushLog("急停通道未接入（本阶段仅电机显示与摇杆通道）"));

// ---------- 启动 ----------
sendChannel(7, 100);    // 发射舵机常态
sendChannel(13, 100);   // 供弹舵机常态
tickClock();
setInterval(tickClock, 1000);
