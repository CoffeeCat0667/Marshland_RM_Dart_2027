# ATOM-14 接线指南（ATOM14ConnectProject）

- **面向**：装配层（AppCoordinator / Entry）与各机构模块的适配器编写者
- **模块产物**：`Atoms/ATOM-14_Safety_Recovery`，CMake 目标 `Marshland::atom_14_safety_recovery`
- **模块头文件**：`Inc/SafetyRecovery.hpp`（主类）、`Inc/SafetyRecoveryPorts.hpp`（窄端口）、`Inc/SafetyRecoveryTypes.hpp`（类型）、`Inc/SafetyRecoveryFaultCatalog.hpp`（Config §14.2 故障码目录）
- **日期**：2026-10-15；基线：PRD 0.10、PEF 1.5、Interface 1.5、Config 0.8

本文件只描述**如何接线**，不重复业务契约（契约见 `Atoms/ATOM-14_Safety_Recovery/Guide.md` 与 `Design/Atomic/ATOM-14_Safety_Recovery.md`）。

---

## 1. 总体接线图

```
                        Entry / AppCoordinator（装配层，唯一接线人）
   ┌──────────────────────────────────────────────────────────────────┐
   │  构造 SafetyRecovery{ ports } ，注入全部端口                       │
   │  注册 CAN 回调、路由 WEB/SBUS 确认请求、每周期调用 update()         │
   └──────────────────────────────────────────────────────────────────┘
        │ 入向（调 ATOM-14）                      │ 出向（ATOM-14 回调）
        ▼                                        ▼
  update() / onEstopConfirmRequest()      ISafetyStateControl ──→ ATOM-02（经 Public 端口）
  registerFault / clearFault /            IControlAuthority   ──→ ATOM-02
  acknowledgeFault / copy*                IInputReporter      ──→ ATOM-02
  isOrdinaryCommandAllowed                IStateReader / IMonotonicClock ──→ 装配层
  onCanEvent / onZeroTorqueSendFailed     IConfigView         ──→ ATOM-15
        ▲                                 ISafetyTorqueGate   ──→ ATOM-04（适配器，待写）
   ATOM-04 CAN 线程                        IRecoveryActions     ──→ ATOM-10/11（适配器，待写）
                                          ILimitSwitchReader  ──→ GPIO 模块（适配器，待写）
                                          IMotorFeedbackReader──→ ATOM-05（适配器，待写）
                                          IFaultEventSink     ──→ ATOM-18/WebSocket（适配器，待写）
```

**禁止**：ATOM-14 不包含任何 ATOM 私有头；任何模块不得绕过端口直接访问 ATOM-14 内部状态；除 CAN 回调外，所有调用必须进入装配层统一同步域（见 §6）。

---

## 2. CMake 接入

```cmake
# 上层 Atoms 聚合或装配目标中：
target_link_libraries(<装配目标> PRIVATE Marshland::atom_14_safety_recovery)
```

模块目标已链接 `Marshland::PublicHeaders`，装配层无需为 ATOM-14 额外添加 include 路径。本模块不设全局编译选项，不产出可执行文件。

---

## 3. 构造与端口注入

```cpp
#include "SafetyRecovery.hpp"

marshland::safety::SafetyRecovery::Ports ports;
// ---- 必需（Public 已有实现，AppCoordinator 直接提供） ----
ports.safety_state       = &app_coordinator;   // ISafetyStateControl：仅注入本模块
ports.control_authority  = &app_coordinator;   // IControlAuthority
ports.input_reporter     = &app_coordinator;   // IInputReporter（ReportFault）
ports.state_reader       = &app_coordinator;   // IStateReader
ports.clock              = &monotonic_clock;   // IMonotonicClock
ports.config             = &config_view;       // IConfigView（ATOM-15）
// ---- 可空但不绑定则对应能力缺失（窄端口适配器，见 §5） ----
ports.torque_gate        = &torque_gate_adapter;
ports.recovery_actions   = &recovery_actions_adapter;
ports.limit_switches     = &limit_switch_adapter;
ports.motor_feedback     = &motor_feedback_adapter;
ports.fault_events       = &fault_event_adapter;

marshland::safety::SafetyRecovery safety_recovery{ports};
```

装配验收：`safety_recovery.portsReady()` 必须为 `true`（六个必需端口全部注入）后才允许其参与控制周期。可空端口未绑定时的行为：

| 未绑定端口 | 行为 |
|---|---|
| `torque_gate` | 无法切换出力阶段，电机保持 ATOM-04 默认 ZERO_TORQUE（fail-safe） |
| `recovery_actions` | 恢复/解除动作无法发起；急停确认直接进入 FAULT 锁定 |
| `limit_switches` | 回零判据恒不成立，恢复停在回零阶段 |
| `motor_feedback` | 按全部电机离线处理，恢复出力不放行 |
| `fault_events` | 故障仅登记不推送，Web 查询接口仍可用 |

---

## 4. 入向接线（装配层/其他模块调用 ATOM-14）

### 4.1 周期推进（必须）

每个控制周期调用一次（控制周期上限 100 ms，建议与 20 ms 控制环同周期）：

```cpp
safety_recovery.update();   // 急停检测、恢复编排推进、CAN 事件消费、自动恢复触发
```

`update()` 内部完成：权威状态借读 → 急停锁定进入/再次急停回退 → 急停解除编排推进 → 一般故障恢复状态机推进 → 根因清除后自动启动一般恢复。**装配层不需要**为急停触发调用任何 ATOM-14 方法——急停事实经 `IInputReporter::ReportSoftwareEstop(true)` 上报仲裁器后，`update()` 通过 `IStateReader` 自行检测锁存并进入锁定。

### 4.2 急停人工确认路由

| 来源 | 路由 |
|---|---|
| WEB `/api/control/estop/confirm` | HTTP 处理器 → 统一同步域 → `safety_recovery.onEstopConfirmRequest()` |
| SBUS 人工确认通道 | SBUS 解析 → 统一同步域 → 同上 |

只在 `phase() == ESTOP_LOCKED` 时有效，其他阶段调用为空操作。**不要**在确认前替 ATOM-14 做任何模式切换或电机动作。

### 4.3 CAN 事件绑定（ATOM-04）

`SafetyRecovery` 直接实现了 Public 的 `can::ICanSafetyNotifier` 与 `can::ICanEventSink`，装配层绑定即可：

```cpp
can_adapter.setSafetyNotifier(&safety_recovery);   // 零力矩发送失败 → F-CAN-002 全机静默
can_adapter.setEventSink(&safety_recovery);        // 总线错误 → F-CAN-001；节点状态 → F-MOTOR-001
```

这两个回调运行在 **ATOM-04 的 CAN 线程**，ATOM-14 内部只置原子标志，故障登记与门控动作推迟到 `update()` 完成——适配器**禁止**在回调里做额外处理，也**禁止**在回调里调用 ATOM-14 的其他方法。

### 4.4 故障登记入口（各归属模块）

故障由**检测到故障的模块**（或其装配适配器）调用登记，故障码必须是 `SafetyRecoveryFaultCatalog.hpp` 中的 23 个之一（未命中目录会被拒绝）：

```cpp
safety_recovery.registerFault("F-SERVO-001", "ATOM-07");   // source 填归属模块
safety_recovery.clearFault("F-SERVO-001");                 // 根因清除后由同一归属方清除
safety_recovery.acknowledgeFault("F-RELOAD-002");          // 人工确认（故障清除走既有契约，无新 HTTP 路由）
```

带 `pef_recovery` 的故障码（见目录头注释）登记后会自动挂起一般恢复意图，根因清除（无严重/急停级活动故障）且机构接口在线后由 `update()` 自动启动一般恢复——**调用方不需要也不应该**直接调用 `requestGeneralRecovery()`，该方法保留给装配层显式触发用。

### 4.5 安全闸门查询

执行器链路在放行**任何普通执行器命令**前查询：

```cpp
if (!safety_recovery.isOrdinaryCommandAllowed()) { /* 拒绝 */ }
```

这是第一道防线；`AppCoordinator::RunGate()` 是第二道，两者都保留，不互相替代。

### 4.6 故障查询（诊断/WEB）

```cpp
marshland::safety::FaultRecord buf[64];
auto n = safety_recovery.copyActiveFaults(buf, 64);      // 当前活动故障
auto m = safety_recovery.copyFaultHistory(buf, 64);      // 历史，最新优先，环形 1000 条
```

记录字段：故障码、来源、等级、UTC 展示时间（`utc_text`）、单调时钟基准（`monotonic_us`）、活动/恢复中/已确认标志。序列化与网络发送在锁外由调用方完成。

---

## 5. 出向适配器（窄端口实现要求）

以下 5 个接口由 ATOM-14 声明（`Inc/SafetyRecoveryPorts.hpp`），**装配层编写适配器**把机构模块绑定上来。通用约束：全部方法 `noexcept`；不得在实现中回调 ATOM-14；不得阻塞超过一个控制周期。

### 5.1 `ISafetyTorqueGate` → ATOM-04

```cpp
bool setTorqueOutputMode(can::TorqueOutputMode mode) noexcept;
can::TorqueOutputMode torqueOutputMode() const noexcept;
```

对应 Public `CanInterfaces.hpp` 注释中引用但尚未落地的 `ICanTorqueGate`。适配器转发到 ATOM-04 的力矩门控。**语义要求**：`ZERO_TORQUE` 与 `CONTROL` 互斥，同一周期只发一类帧；切到 `CONTROL` 后不得再发全零帧（否则覆盖恢复出力）。切换调用集中在 ATOM-14 的阶段迁移点，适配器无需自行加策略。

### 5.2 `IRecoveryActions` → ATOM-10（换弹）/ ATOM-11（发射）/ 蓄力机构模块

```cpp
ActionResult requestChargeHome() noexcept;    // 蓄力电机 1、2 回零
ActionResult requestRecoveryFire() noexcept;  // 发射机构一次完整单次发射
ActionResult requestReloadLift() noexcept;    // 双 M3508 抬起：左正转右反转
void cancelAll() noexcept;                    // 撤销全部恢复出力目标与计时
ActionStatus actionStatus(RecoveryAction) const noexcept;
```

适配器/机构模块的职责边界：

- **蓄力回零**：动作执行与零力矩保持归机构模块；**完成判据不由本端口报告**——ATOM-14 用 `ILimitSwitchReader` 双限位均低自行判定。
- **单次发射**：待机角度→发射角度→保持 `timing.fire_angle_hold_ms`（100 ms）→待机角度，回到待机后报 `SUCCEEDED`。三次发射期间其他机构电机零力矩、其他舵机角度不变，由机构模块保证。
- **换弹抬起**：两侧运行目标均成功提交后按 `timing.reload_lift_run_s`（3 s）定时运行，随后两台 0 rpm 目标均成功提交且未取消/失败才报 `SUCCEEDED`，之后保持速度闭环。**3 s 是正常运行时间，不作为动作超时**；任一侧电机离线/反馈/PID/命令异常时两侧零力矩并取消，报 `FAILED`。
- `cancelAll()` 必须幂等，任意状态下可调用。
- `actionStatus` 在 `requestXxx` 前应为 `IDLE`；`cancelAll` 后回到 `IDLE`。

### 5.3 `ILimitSwitchReader` → GPIO 模块

```cpp
bool chargeLimit1Low() const noexcept;
bool chargeLimit2Low() const noexcept;
```

返回蓄力机构两个限位开关**当前是否稳定为低电平**。GPIO 读取失败或状态无效时返回 `false`（判据不成立，恢复不会误判完成），并由 GPIO 侧登记 `F-GPIO-001`。

### 5.4 `IMotorFeedbackReader` → ATOM-05

```cpp
bool motorOnline(int motor_id) const noexcept;          // 按 can_comm_timeout_ms 在线判定
bool motorRpm(int motor_id, double& out_rpm) const noexcept;
```

`motor_id` 为 `dart_control.json` 的 `motor[].id`（1~6；当前：蓄力右 1、蓄力左 2、换弹右 3、换弹左 4）。ATOM-14 只用 `motorOnline` 做换弹联动与恢复前接口有效性检查；`motorRpm` 预留给到位复核，机构模块内部的 0 rpm 判定不依赖本端口。

### 5.5 `IFaultEventSink` → ATOM-18 / WebSocket

```cpp
void onFaultEvent(const FaultRecord& record) noexcept;
```

每次故障登记与清除各推送一次（记录含 UTC 展示时间与单调时钟基准）。适配器负责序列化与 WebSocket 推送，**在锁外/工作线程完成网络发送**，实现内不得阻塞。锁定/恢复阶段状态变化伴随故障事件（`E-ESTOP-001`、`F-RELOAD-002`）发布，不另有独立事件通道。

---

## 6. 并发与同步约束

1. **统一同步域**：`update()`、`onEstopConfirmRequest()`、故障管理 API、闸门查询全部由装配层在同一把网关锁内串行调用；ATOM-14 不跨线程保存 `StateReadView` 或仲裁器内部引用（借读规则见根 `PLAN.md` §5.3）。
2. **CAN 回调例外**：`onCanEvent`/`onZeroTorqueSendFailed` 来自 CAN 线程，ATOM-14 内部已用原子标志隔离；适配器不得扩展这两个回调的行为。
3. **锁内禁令**：ATOM-14 的端口实现不得在锁内做硬件 I/O、网络、日志刷盘、sleep、join。
4. **急停响应**：锁定动作在锁存置位后的第一个 `update()` 执行；配合 ≤100 ms 控制周期满足 Config §4 的 100 ms 上限。`SCHED_OTHER` 不构成硬实时保证，时限验收以实机压力测试为准。

---

## 7. 启动顺序

1. ATOM-15 配置加载完成（`IConfigView::isLoaded() == true`）。ATOM-14 的配置快照（恢复发射次数、motor role→id 映射）在 `update()` 中**惰性加载**，装配层无需显式调用初始化。
2. AppCoordinator 构造完成，六个必需端口可注入。
3. 构造 `SafetyRecovery`，绑定 CAN 回调与五个窄端口适配器。
4. `portsReady()` 检查通过后将 `update()` 挂入控制周期。
5. 启动阶段本模块处于 `NORMAL` 相、零力矩门控默认在 ATOM-04 侧为 `ZERO_TORQUE`，不需要额外初始化动作；本模块永远**不**触发重新初始化路径。

---

## 8. 接线验收清单

- [ ] `portsReady() == true`，五个窄端口适配器已绑定
- [ ] `update()` 已进入 ≤100 ms 控制周期
- [ ] WEB/SBUS 人工确认路由到 `onEstopConfirmRequest()`
- [ ] ATOM-04 的 `ICanSafetyNotifier`/`ICanEventSink` 已绑定本模块实例
- [ ] 普通执行器命令路径上有 `isOrdinaryCommandAllowed()` 检查
- [ ] 各机构模块的故障登记使用目录内故障码，未命中目录的登记被拒绝
- [ ] 急停全流程：触发→锁定（全零帧持续）→确认→恢复出力（无全零帧覆盖）→解除（锁存清除）逐步实测通过
- [ ] 再次急停、换弹单侧离线联动、F-RELOAD-002 锁定不重试实测通过
- [ ] 一般恢复顺序（回零→三次发射→换弹抬起→MANUAL）实测通过，完成后不续跑旧自动流程
- [ ] ARM 增量构建 `-Wall -Wextra -Wpedantic` 零警告
