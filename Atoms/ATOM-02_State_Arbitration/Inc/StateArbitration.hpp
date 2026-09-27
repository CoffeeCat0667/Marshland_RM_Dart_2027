// ============================================================================
// StateArbitration.hpp — ATOM-02 系统状态机与控制权仲裁（策略接口）
//
// 权威来源：
//   Design/Atomic/ATOM-02_State_Arbitration.md  单一职责与 14 条原子操作
//   Design/Interface.md  §3.2 控制权优先级、§6 数据模型、§7 状态机、
//                        §9 控制权与外部输入、§17.3 安全动作例外、§18.2 故障接口
//   Design/Config.md     §2 运行基线、§7.1 SBUS 映射、§7.2 控制源选择规则、
//                        §11.4 WEB 动作集、§14 故障/安全/恢复、§15 并发与安全动作例外
//   Design/PEF.md        §5 四发流程、§7 一般故障恢复、§8.3 急停解除
//
// 边界：
//   * 只输出状态、授权与事件结果，不访问硬件，不发送 CAN/IIC 报文。
//   * 不实现、不持有、不执行 FireAction：调用点在自动流程
//     FIRE_READY -> FIRING 之前（Interface §7.1、§8.1、§8.2）。
//   * 不实现故障管理（Interface §18.2）：只消费“是否故障 + 故障等级”。
//   * 不存在 main 入口函数。
//
// 无副作用约定：
//   SelectControlSource / IsModeTransitionAllowed / IsAutoFireTransitionAllowed /
//   IsFaultBlocking / EvaluateFireAuthorization / EvaluateIntent 为纯函数。
//   状态提交集中在 StateArbitrator 的显式提交接口，调用方不得把“拒绝”当“成功”。
//
// 类型位置：外部输入 / 意图 / 结果 / 事件类型已上提至
//   Atoms/Public/StateArbitrationTypes.hpp（ATOM02-BUG-013），
//   本头只保留纯函数策略与 StateArbitrator 本体。
// ============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "MarshlandTypes.hpp"
#include "StateArbitrationTypes.hpp"

namespace marshland {
namespace state_arbitration {

// ---------------------------------------------------------------------------
// 纯函数：无副作用
// ---------------------------------------------------------------------------

// Config §7.2 控制源选择。
// 软件急停的覆盖只在"急停尚未解除"期间生效（模式仍为 SOFTWARE_ESTOP）时返回 NONE；
// 人工确认后（模式已转 MANUAL）按 Config §7.2 正常选择，见 ATOM02-BUG-004 遗留点 4。
ControlSource SelectControlSource(const ArbitrationContext& ctx) noexcept;

// Interface §7.1 状态转换合法性（含急停/故障通配边）
bool IsModeTransitionAllowed(SystemMode from, SystemMode to, TransitionReason reason) noexcept;

// Interface §6.3/§7.1/§8 自动流程转换合法性。
// 带 stage 的重载用于消歧 FORCE -> SHOT_1_YAW / SHOT_N_YAW（ATOM02-BUG-001）。
bool IsAutoFireTransitionAllowed(AutoFireState from, AutoFireState to) noexcept;
bool IsAutoFireTransitionAllowed(AutoFireState from, AutoFireState to, ShotStage stage) noexcept;

// Interface §6.3：AUTO_COMPLETED / AUTO_TERMINATED 为终态
bool IsAutoFireStateTerminal(AutoFireState state) noexcept;

// Config §14.2：只有严重（SEVERE）与急停（ESTOP）等级故障全局阻断普通意图与发射；
// 信息（INFO，如 W-VISION-002 正常回退）与警告（WARNING，如 W-SBUS-001、
// F-VISION-001）不全局阻断（ATOM02-BUG-006）。
bool IsFaultBlocking(const ExternalInputs& inputs) noexcept;

// 每次实际发射前的复核：只有前置条件——急停 / 故障 / 停止发射 / YAW 到位
// （Interface §7.2 第 3 条）。**不判定优先级**（ATOM02-BUG-002 方案 B）：控制源
// 优先级一律由 EvaluateIntent/SubmitIntent 的 FIRE 分支先仲裁。
FireAuthorization EvaluateFireAuthorization(const ArbitrationContext& ctx,
                                            ControlSource requesting_source) noexcept;

// 普通 SBUS/WEB/自动流程控制意图的优先级仲裁
IntentDecision EvaluateIntent(const ArbitrationContext& ctx, const ControlIntent& intent) noexcept;

// 便于日志/诊断适配器
const char* ToString(TransitionReason reason) noexcept;
const char* ToString(RejectReason reason) noexcept;
const char* ToString(IntentKind kind) noexcept;
const char* ToString(EventKind kind) noexcept;
const char* ToString(ShotStage stage) noexcept;

// ---------------------------------------------------------------------------
// 状态机：唯一的可变状态持有者
// ---------------------------------------------------------------------------
class StateArbitrator {
public:
    static constexpr std::size_t kMaxEvents = 64;
    // Interface §6.6 AutoFireContext.total_shots = 4
    static constexpr std::uint32_t kTotalShots = 4;

    StateArbitrator() noexcept = default;

    // 回到 UNINITIALIZED / IDLE / AUTO，清空外部输入与事件（仅供初始化使用）
    void Reset(std::uint64_t timestamp_us = 0) noexcept;

    // ---- 外部输入：字段级写入（ATOM02-BUG-005） ---------------------------
    // 每个来源只写自己的字段，避免整体替换把他人字段清零。
    // 返回控制源变化（仅当该字段可能影响控制源选择时）。
    ControlSourceChange SetSbusStatus(bool online, bool auto_mode) noexcept;
    ControlSourceChange SetWebOnline(bool online) noexcept;
    // 急停输入：一旦置位即锁存（Config §15），外部写回 false 不会解除
    ControlSourceChange SetSoftwareEstopInput(bool active) noexcept;
    void SetStopFire(bool active) noexcept;
    void SetYawArrived(bool arrived) noexcept;
    void SetFault(bool active, FaultSeverity severity) noexcept;
    void SetClock(std::uint64_t timestamp_us, std::uint64_t sequence) noexcept;

    // 整体替换：仅限单一写入方（如 AppCoordinator 合并各来源后提交）或复位场景。
    // 多来源各自调用会互相清零对方字段（ATOM02-BUG-005）。
    ControlSourceChange UpdateInputs(const ExternalInputs& inputs) noexcept;

    // ---- 查询（无副作用） -------------------------------------------------
    const ExternalInputs& inputs() const noexcept { return ctx_.inputs; }
    const ArbitrationContext& context() const noexcept { return ctx_; }
    SystemMode system_mode() const noexcept { return ctx_.system_mode; }
    AutoFireState auto_fire_state() const noexcept { return ctx_.auto_fire_state; }
    ControlSource control_source() const noexcept { return ctx_.control_source; }
    std::uint64_t run_id() const noexcept { return run_id_; }
    // Interface §6.6 AutoFireContext.current_shot：0 表示尚未开始，1..4 为发次
    std::uint32_t current_shot() const noexcept { return current_shot_; }
    ShotStage shot_stage() const noexcept;
    bool is_software_estop() const noexcept { return ctx_.inputs.software_estop; }
    bool is_estop_latched() const noexcept { return estop_latched_; }
    bool is_auto_fire_active() const noexcept;
    ControlSource EvaluateControlSource() const noexcept;

    // 按 Config §7.2 重算并提交控制源；变化时记录 ControlSourceChange 事件
    ControlSourceChange RefreshControlSource() noexcept;

    // ---- 系统状态机 -------------------------------------------------------
    // 只有 (from, to, reason) 三元组在 Interface §7.1 表中（或急停/故障通配边）
    // 时才提交；非法切换不改变状态。
    ModeTransitionResult RequestModeTransition(SystemMode target, TransitionReason reason) noexcept;

    // 软件急停：最高安全覆盖，不可由普通模式切换解除
    ModeTransitionResult TriggerSoftwareEstop() noexcept;

    // 第一步（人工确认）：SOFTWARE_ESTOP -> MANUAL。
    // 只做模式迁移，**保持急停变量与锁存为 True**（ATOM02-BUG-004），
    // 以便 ATOM-14 在锁定保护下执行换弹抬起与蓄力双电机回零。
    ModeTransitionResult ConfirmSoftwareEstop() noexcept;

    // 第二步（解除生效）：由 ATOM-14 在
    //   * 换弹机构抬起动作组已完成
    //   * 蓄力电机 1、2 回零完成（两个限位开关均为低电平）
    // 全部满足后调用；成功解除锁存与急停变量，返回 true。
    // 前置：已经过 ConfirmSoftwareEstop()（模式已离开 SOFTWARE_ESTOP）。
    bool ReleaseSoftwareEstopLatch() noexcept;

    // ---- 自动流程状态 -----------------------------------------------------
    // 合法转换需处于 AUTO_FIRE 模式；FIRE_READY -> FIRING 需通过发射授权；
    // FIRE_COMPLETED -> CHARGE/AUTO_COMPLETED 由 current_shot 约束（ATOM02-BUG-001）。
    // AUTO_PREPARE 目标一律转交 BeginNewAutoFireRun（唯一流程入口，ATOM02-BUG-008）。
    // IDLE 表示"未启动"，等价于已停止：此时 AUTO_TERMINATED 为幂等空操作，
    // 返回 AUTO_FLOW_NOT_STARTED 且不改变状态（ATOM02-BUG-010）。
    AutoFireTransitionResult RequestAutoFireState(AutoFireState target) noexcept;

    // SBUS 接管：允许当前硬件动作组完成后终止（动作组由调用方负责完成），并切回 MANUAL。
    // 未启动（IDLE）→ AUTO_FLOW_NOT_STARTED；已完成（AUTO_COMPLETED）→ AUTO_FLOW_TERMINATED；
    // 已终止 → ALREADY_IN_TARGET（均为空操作，ATOM02-BUG-010）。
    AutoFireTransitionResult TerminateAutoFireOnSbusTakeover() noexcept;

    // 新的一次自动流程（run_id 自增、current_shot = 1）。
    // 这是离开 AUTO_COMPLETED / AUTO_TERMINATED 的唯一入口（原子操作 11）。
    AutoFireTransitionResult BeginNewAutoFireRun() noexcept;

    // ---- 仲裁 -------------------------------------------------------------
    // 普通意图只做判定，不改变控制目标；结论写入事件
    IntentDecision SubmitIntent(const ControlIntent& intent) noexcept;

    // 自动流程内的发射授权判定（FIRE_READY -> FIRING 之前调用）。
    // 只给出前置条件结论；SBUS/WEB 的介入优先级必须先经 SubmitIntent 仲裁。
    FireAuthorization AuthorizeFire(ControlSource requesting_source) noexcept;

    // 手动/调试模式下的单次发射授权（Config §11.4 fire_once，允许模式：手动、调试）。
    // 与自动流程状态机解耦：不读写 auto_fire_state，只给出授权结论。
    // 同样只做前置条件复核；请求源优先级由 SubmitIntent 仲裁后再调用。
    FireAuthorization AuthorizeManualFire(ControlSource requesting_source) noexcept;

    // ---- 事件 -------------------------------------------------------------
    std::size_t event_count() const noexcept { return event_count_; }
    // 因环形缓冲覆盖而丢失的事件条数（ATOM02-BUG-012）
    std::size_t dropped_event_count() const noexcept { return dropped_event_count_; }
    void ClearEvents() noexcept;
    // 从最旧到最新复制，返回实际复制条数
    std::size_t CopyEvents(StateEvent* out, std::size_t capacity) const noexcept;
    // 最新一条事件；无事件时返回 nullptr
    const StateEvent* latest_event() const noexcept;

private:
    void PushEvent(StateEvent event) noexcept;
    void TerminateAutoFlowInternal(TransitionReason reason) noexcept;
    ControlSourceChange RefreshControlSourceInternal(TransitionReason reason) noexcept;
    // 急停输入置位时同步进入 SOFTWARE_ESTOP，并刷新控制源
    ControlSourceChange SyncEstopAndRefresh() noexcept;

    ArbitrationContext ctx_{};
    std::array<StateEvent, kMaxEvents> events_{};
    std::size_t event_count_{0};
    std::size_t dropped_event_count_{0};
    std::size_t head_{0};
    std::uint64_t sequence_{0};
    std::uint64_t run_id_{0};
    std::uint32_t current_shot_{0};   // Interface §6.6 AutoFireContext.current_shot
    bool estop_latched_{false};       // Config §15 急停锁定：仅解除动作完成后可清
};

} // namespace state_arbitration
} // namespace marshland
