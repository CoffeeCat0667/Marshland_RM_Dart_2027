// ============================================================================
// StateArbitrationTypes.hpp — 状态机与仲裁的输入 / 意图 / 结果 / 事件契约
//
// 上提依据：Atoms/ATOM-02_State_Arbitration/BugLists.md ATOM02-BUG-013。
// 这些类型的生产方/消费方是 ATOM-03、ATOM-11、ATOM-12、ATOM-13、ATOM-14、
// ATOM-16/17、ATOM-18，不应横向依赖 ATOM-02 的私有头。上提后 ATOM-02 的
// Inc/ 只保留纯函数策略与 StateArbitrator 本体。
//
// 本文件是叶子契约：只含类型，仅依赖 MarshlandTypes.hpp，不链接任何 ATOM。
// ============================================================================
#pragma once

#include <cstdint>

#include "MarshlandTypes.hpp"

namespace marshland {
namespace state_arbitration {

// ---------------------------------------------------------------------------
// 外部输入快照
//
// 写入方（用户确认）：SBUS 解析器写 SBUS 字段；WEB、视觉、故障模块写各自字段；
// 软件急停为统一 bool（Config §15）。字段级写入接口见 StateArbitrator。
//   停止发射标志：Config §2 初值为 True；WEB 前端置 True；视觉正常 YAW 到位置
//                 False（Interface §6.4）
//   YAW 到位：视觉置 True；主控超时回退置 True；发射完成置 False
// ---------------------------------------------------------------------------
struct ExternalInputs {
    bool web_online{false};
    bool sbus_online{false};
    bool sbus_auto_mode{false};   // Config §7.1 CH6「自动模式」，1684 为 True
    bool software_estop{false};   // Config §15 统一 bool，True 表示锁定急停
    // Config §2：停止发射标志初值为 True（修正 ATOM02-BUG-005 的默认值偏差）
    bool stop_fire{true};
    bool fault_active{false};     // 故障表见 Config §14.2
    // 缺省按最严格等级（fail-safe）：写入方若只置 fault_active 而漏填等级，
    // 行为等同严重故障而不是静默放行（ATOM02-BUG-006）。
    FaultSeverity fault_severity{FaultSeverity::SEVERE};
    bool yaw_arrived{false};      // Interface §6.4 / §7.2 第 2 条
    std::uint64_t timestamp_us{0};// 单调时钟，禁止墙上时钟（Interface §3.3）
    std::uint64_t sequence{0};
};

// 仲裁上下文快照：外部输入 + 本模块持有的状态
struct ArbitrationContext {
    ExternalInputs inputs{};
    SystemMode system_mode{SystemMode::UNINITIALIZED};
    AutoFireState auto_fire_state{AutoFireState::IDLE};
    ControlSource control_source{ControlSource::AUTO};
};

// ---------------------------------------------------------------------------
// 发次阶段：用于消歧 FORCE -> SHOT_1_YAW / SHOT_N_YAW（ATOM02-BUG-001）。
// ANY 只用于转换表的通配边，不作为运行期阶段值使用。
// ---------------------------------------------------------------------------
enum class ShotStage : std::uint8_t {
    FIRST,       // 第 1 发（Interface §8.1）
    SUBSEQUENT,  // 第 2～4 发（Interface §8.2）
    ANY
};

// ---------------------------------------------------------------------------
// 原因与拒绝原因
// ---------------------------------------------------------------------------
enum class TransitionReason : std::uint8_t {
    NONE,
    START,
    INIT_SUCCESS,
    INIT_FAILURE,
    ENTER_MANUAL,
    AUTO_START,
    SBUS_TAKEOVER,
    WEB_MANUAL_TAKEOVER,
    SOFTWARE_ESTOP_TRIGGER,
    ESTOP_CONFIRM,
    ESTOP_RELEASED,          // 解除动作完成，锁存解除（Config §15、PEF §8.3、ATOM-14）
    FAULT_DETECTED,
    RECOVERY_START,
    RECOVERY_COMPLETE,
    DEBUG_ENTER,
    DEBUG_EXIT,
    CALIBRATION_ENTER,
    CALIBRATION_EXIT,
    AUTO_FLOW_RUN,
    AUTO_FLOW_STEP,
    AUTO_FLOW_DONE,
    AUTO_FLOW_ABORT,
    CONTROL_SOURCE_REFRESH
};

enum class RejectReason : std::uint8_t {
    NONE,
    INVALID_INTENT,
    UNKNOWN_TRANSITION,
    ALREADY_IN_TARGET,
    NOT_READY,
    NOT_AUTO_FIRE_MODE,
    AUTO_FLOW_ACTIVE,
    AUTO_FLOW_TERMINATED,
    // ATOM02-BUG-010：IDLE 表示"未启动"，等价于已停止。此时收到 auto_stop /
    // SBUS 接管终止请求属幂等空操作，不改变状态，也不算错误。
    AUTO_FLOW_NOT_STARTED,
    NOT_SBUS_TAKEOVER,
    SHOT_STAGE_MISMATCH,     // 发次与转换边不匹配（ATOM02-BUG-001）
    SOFTWARE_ESTOP,
    ESTOP_NOT_CONFIRMED,     // 尚未人工确认即请求解除锁存（ATOM02-BUG-004）
    FAULT_ACTIVE,
    STOP_FIRE_SET,
    YAW_NOT_ARRIVED,
    LOWER_PRIORITY_CONFLICT,
    FIRE_NOT_AUTHORIZED
};

// ---------------------------------------------------------------------------
// 结果类型
// ---------------------------------------------------------------------------
struct ModeTransitionResult {
    bool accepted{false};
    SystemMode from{SystemMode::UNINITIALIZED};
    SystemMode to{SystemMode::UNINITIALIZED};
    TransitionReason transition_reason{TransitionReason::NONE};
    RejectReason reason{RejectReason::NONE};
};

struct AutoFireTransitionResult {
    bool accepted{false};
    AutoFireState from{AutoFireState::IDLE};
    AutoFireState to{AutoFireState::IDLE};
    TransitionReason transition_reason{TransitionReason::NONE};
    RejectReason reason{RejectReason::NONE};
};

struct ControlSourceChange {
    bool changed{false};
    ControlSource from{ControlSource::NONE};
    ControlSource to{ControlSource::NONE};
    TransitionReason reason{TransitionReason::NONE};
};

// ---------------------------------------------------------------------------
// 控制意图：WEB 请求必须携带请求标识与控制源标识（Interface §9.2）
// ---------------------------------------------------------------------------
enum class IntentKind : std::uint8_t {
    CONTROL,          // 普通机构控制意图
    FIRE,             // 发射授权请求（FireAction 之前的判定）
    SAFETY,           // 安全动作：零力矩、换弹抬起、蓄力回零、故障恢复动作。
                      // Config §15「安全动作例外」+ Interface §17.3 第 7 条：
                      // 不受普通命令拒绝规则影响（ATOM02-BUG-007）
    STOP_FIRE_SET,    // 置停止发射标志（安全单调：只收紧，不受优先级阻断）
    STOP_FIRE_CLEAR,  // 清停止发射标志（Interface §9.2 第三条路径）
    AUTO_START,
    AUTO_STOP
};

struct ControlIntent {
    bool valid{true};
    ControlSource source{ControlSource::NONE};
    IntentKind kind{IntentKind::CONTROL};
    std::uint32_t request_id{0};
    std::uint64_t timestamp_us{0};
};

struct IntentDecision {
    bool accepted{false};
    ControlSource source{ControlSource::NONE};
    ControlSource control_source{ControlSource::AUTO};
    IntentKind kind{IntentKind::CONTROL};
    RejectReason reason{RejectReason::NONE};
};

// 实际发射授权：始终严格为 SBUS > WEB > 自动流程（Interface §3.2、§6.1）
struct FireAuthorization {
    bool allowed{false};
    ControlSource requesting_source{ControlSource::NONE};
    ControlSource authorizing_source{ControlSource::NONE};
    ControlSource control_source{ControlSource::AUTO};
    RejectReason reason{RejectReason::NONE};
    bool estop{false};
    bool fault{false};
    bool stop_fire{false};
    bool yaw_not_arrived{false};
    // ATOM02-BUG-002 方案 B：授权函数只做前置条件复核，优先级不在其中判定，
    // 因此本字段恒为 false（保留以兼容结果类型；优先级见 EvaluateIntent）。
    bool priority_conflict{false};
};

// ---------------------------------------------------------------------------
// 状态事件（Interface §7.2 第 6 条、ATOM-02 原子操作 3/13）
// ---------------------------------------------------------------------------
enum class EventKind : std::uint8_t {
    STATE_TRANSITION,
    AUTO_FIRE_TRANSITION,
    CONTROL_SOURCE_CHANGE,
    INTENT_ACCEPTED,
    INTENT_REJECTED,
    FIRE_DECISION,
    ESTOP_RELEASED
};

struct StateEvent {
    EventKind kind{EventKind::STATE_TRANSITION};
    TransitionReason reason{TransitionReason::NONE};
    RejectReason reject_reason{RejectReason::NONE};
    SystemMode from_mode{SystemMode::UNINITIALIZED};
    SystemMode to_mode{SystemMode::UNINITIALIZED};
    AutoFireState from_auto_fire{AutoFireState::IDLE};
    AutoFireState to_auto_fire{AutoFireState::IDLE};
    ControlSource from_source{ControlSource::NONE};
    ControlSource to_source{ControlSource::NONE};
    std::uint32_t request_id{0};
    std::uint64_t run_id{0};
    std::uint64_t timestamp_us{0};
    std::uint64_t sequence{0};
};

} // namespace state_arbitration
} // namespace marshland
