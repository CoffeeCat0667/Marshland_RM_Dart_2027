// ============================================================================
// Marshland 控制面板 — 演示驱动脚本 v2
//
// 本阶段不接任何真实接口（PLAN §7.4）：状态区数据为本地 mock。
// 摇杆为真实交互控件：偏移量 → 速度值为线性映射
//   value = offset / radius × max   （满偏 = ±max，当前 max = 5000 rpm）
// 松手自动回中归零。真实通路口后接 POST /api/control/manual
// （force_set / yaw_set 等 action）。
// ============================================================================

const $ = (id) => document.getElementById(id);

// ---------- 摇杆（仅纵向） ----------
function setupJoystick(el) {
  const max = Number(el.dataset.max);      // 满偏对应的速度上限（±5000 rpm）
  const name = el.dataset.name;
  const knob = el.querySelector(".joy-knob");
  const readout = el.parentElement.querySelector(".joy-readout b");
  const meter = el.parentElement.querySelector(".joy-meter .fill");

  let dragging = false;

  const travel = () => el.clientHeight / 2 - knob.offsetHeight / 2 - 6; // 旋钮纵向可动半径

  function apply(dy) {
    const t = travel();
    dy = Math.max(-t, Math.min(t, dy));    // 纵向限幅，水平方向始终锁定
    knob.style.transform = `translate(-50%, calc(-50% + ${dy}px))`;

    // 线性映射：向上为正，满偏 = ±max
    const value = Math.round((-dy / t) * max);

    readout.textContent = value;
    readout.className = value > 0 ? "pos" : value < 0 ? "neg" : "";

    // 底部线性条：从中线向两侧填充
    const ratio = value / max;
    meter.style.width = `${Math.abs(ratio) * 50}%`;
    meter.style.left = ratio >= 0 ? "50%" : `${50 - Math.abs(ratio) * 50}%`;
    meter.classList.toggle("neg", ratio < 0);

    return value;
  }

  function reset() {
    knob.style.transform = "translate(-50%, -50%)";
    readout.textContent = "0";
    readout.className = "";
    meter.style.width = "0";
    meter.style.left = "50%";
  }

  el.addEventListener("pointerdown", (e) => {
    dragging = true;
    el.classList.add("active");
    el.setPointerCapture(e.pointerId);
    const r = el.getBoundingClientRect();
    apply(e.clientY - (r.top + r.height / 2));
  });

  el.addEventListener("pointermove", (e) => {
    if (!dragging) return;
    const r = el.getBoundingClientRect();
    apply(e.clientY - (r.top + r.height / 2));
  });

  const release = (e) => {
    if (!dragging) return;
    dragging = false;
    el.classList.remove("active");
    if (el.hasPointerCapture?.(e.pointerId)) el.releasePointerCapture(e.pointerId);
    reset();
    pushLog(`[演示] ${name}摇杆回中，速度 0 ${el.dataset.unit} —— 接口尚未接入`);
  };
  el.addEventListener("pointerup", release);
  el.addEventListener("pointercancel", release);
}

document.querySelectorAll(".joy").forEach(setupJoystick);

// ---------- mock 状态区（演示动画用） ----------
const mock = {
  sysMode: ["STANDBY", "MANUAL", "AUTO_FIRE", "DEBUG"],
  sysModeIdx: 0,
  controlSource: ["NONE", "SBUS", "WEB", "AUTO"],
  controlSourceIdx: 2,
  autoFireStates: [
    "IDLE", "AUTO_PREPARE", "CHARGE", "FORCE", "SHOT_1_YAW",
    "RELOAD_LIFT", "FEED", "FEED_WAIT", "RELOAD_LOWER",
    "SHOT_N_YAW", "FIRE_READY", "FIRING", "FIRE_COMPLETED",
  ],
  autoFireIdx: 0,
  currentShot: 0,
  totalShots: 4,
  connected: true,
};

function setValue(id, text, colorClass) {
  const el = $(id);
  if (!el || el.textContent === text) return;
  el.textContent = text;
  el.classList.remove("flash");
  void el.offsetWidth;
  el.classList.add("flash");
  if (colorClass) el.className = `stat-value ${colorClass} flash`;
}

function renderShots() {
  document.querySelectorAll("#shots .shot").forEach((el) => {
    const i = Number(el.dataset.i);
    el.classList.toggle("done", i <= mock.currentShot);
    el.classList.toggle("active", i === mock.currentShot + 1 &&
      mock.autoFireIdx > 0 && mock.autoFireIdx < mock.autoFireStates.length - 1);
  });
  $("afFill").style.width = `${(mock.currentShot / mock.totalShots) * 100}%`;
}

const demoLogs = [
  "系统待机，等待操作…",
  "WEB 客户端已连接，控制源切换为 WEB",
  "收到自动启动请求 request_id=w1042，已受理",
  "自动流程推进：CHARGE → FORCE",
  "第 1 发 YAW 调整中…",
  "第 1 发完成，流程继续",
  "SBUS 信号失联，降级至 WEB 控制",
];

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

function demoTick() {
  $("connDot").classList.toggle("online", mock.connected);
  $("connText").textContent = mock.connected ? "WEB 已连接" : "未连接";

  mock.autoFireIdx = (mock.autoFireIdx + 1) % mock.autoFireStates.length;
  const state = mock.autoFireStates[mock.autoFireIdx];
  setValue("afState", state, "accent");
  if (state === "FIRE_COMPLETED") mock.currentShot = Math.min(mock.currentShot + 1, 4);
  if (mock.autoFireIdx === 0) mock.currentShot = 0;
  renderShots();

  if (Math.random() > 0.85) {
    mock.sysModeIdx = (mock.sysModeIdx + 1) % mock.sysMode.length;
    setValue("sysMode", mock.sysMode[mock.sysModeIdx], "accent");
  }
  if (Math.random() > 0.85) {
    mock.controlSourceIdx = (mock.controlSourceIdx + 1) % mock.controlSource.length;
    setValue("ctrlSrc", mock.controlSource[mock.controlSourceIdx]);
  }
  setValue("stopFire", mock.autoFireIdx === 0 ? "已置位" : "已解除",
    mock.autoFireIdx === 0 ? "warn" : "ok");

  pushLog(demoLogs[Math.floor(Math.random() * demoLogs.length)]);
}

// ---------- 按钮演示（不接接口，仅日志反馈） ----------
document.querySelectorAll(".ctl").forEach((btn) => {
  btn.addEventListener("click", () => pushLog(`[演示] 已点击「${btn.textContent}」——接口尚未接入`));
});

$("estopBtn").addEventListener("click", () => {
  pushLog("[演示] 软件急停触发——真实通路将经 ATOM-14，解除须人工确认");
});

// ---------- 启动 ----------
tickClock();
setInterval(tickClock, 1000);
renderShots();
setInterval(demoTick, 1600);
