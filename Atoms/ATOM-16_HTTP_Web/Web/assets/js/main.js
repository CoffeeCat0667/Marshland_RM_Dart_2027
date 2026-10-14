// ============================================================================
// Marshland 控制面板 — 演示驱动脚本
//
// 本阶段不接任何真实接口（PLAN §7.4）：全部数据为本地 mock，
// 仅用定时器循环演示数值变化与动画。真实数据源将是
// ATOM-17 WebSocket 推送 + ATOM-16 HTTP 轮询兜底。
// ============================================================================

// ---------- mock 状态 ----------
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
  stopFire: true,
  connected: false,
};

const $ = (id) => document.getElementById(id);

// 数值变化时闪烁强调
function setValue(id, text, colorClass) {
  const el = $(id);
  if (!el || el.textContent === text) return;
  el.textContent = text;
  el.classList.remove("flash");
  void el.offsetWidth; // 重启动画
  el.classList.add("flash");
  if (colorClass) el.className = `stat-value ${colorClass} flash`;
}

// ---------- 发次进度 ----------
function renderShots() {
  document.querySelectorAll("#shots .shot").forEach((el) => {
    const i = Number(el.dataset.i);
    el.classList.toggle("done", i <= mock.currentShot);
    el.classList.toggle("active", i === mock.currentShot + 1 &&
      mock.autoFireIdx > 0 && mock.autoFireIdx < mock.autoFireStates.length - 1);
  });
  $("afFill").style.width = `${(mock.currentShot / mock.totalShots) * 100}%`;
}

// ---------- 底部日志 ----------
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
  $("logTime").textContent = new Date().toLocaleTimeString("zh-CN", { hour12: false });
}

// ---------- 演示循环：驱动全部动画 ----------
function demoTick() {
  // 连接状态呼吸
  mock.connected = !mock.connected || Math.random() > 0.1;
  $("connDot").classList.toggle("online", mock.connected);
  $("connText").textContent = mock.connected ? "WEB 已连接" : "未连接";

  // 自动流程步进循环
  mock.autoFireIdx = (mock.autoFireIdx + 1) % mock.autoFireStates.length;
  const state = mock.autoFireStates[mock.autoFireIdx];
  setValue("afState", state, "accent");
  if (state === "FIRE_COMPLETED") mock.currentShot = Math.min(mock.currentShot + 1, 4);
  if (mock.autoFireIdx === 0) mock.currentShot = 0;
  renderShots();

  // 偶发切换系统模式与控制源演示
  if (Math.random() > 0.85) {
    mock.sysModeIdx = (mock.sysModeIdx + 1) % mock.sysMode.length;
    setValue("sysMode", mock.sysMode[mock.sysModeIdx], "accent");
  }
  if (Math.random() > 0.85) {
    mock.controlSourceIdx = (mock.controlSourceIdx + 1) % mock.controlSource.length;
    setValue("ctrlSrc", mock.controlSource[mock.controlSourceIdx]);
  }

  mock.stopFire = mock.autoFireIdx === 0;
  setValue("stopFire", mock.stopFire ? "已置位" : "已解除", mock.stopFire ? "warn" : "ok");

  pushLog(demoLogs[Math.floor(Math.random() * demoLogs.length)]);
}

// ---------- 按钮演示（不接接口，仅反馈动画 + 日志） ----------
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
