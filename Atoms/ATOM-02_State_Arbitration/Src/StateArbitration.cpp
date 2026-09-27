// ============================================================================
// StateArbitration.cpp — ATOM-02 系统状态机与控制权仲裁实现
//
// 修复记录（对应 Atoms/ATOM-02_State_Arbitration/BugLists.md）：
//   ATOM02-BUG-001  补 FORCE -> SHOT_N_YAW 边，并以 ShotStage 消歧
//   ATOM02-BUG-003  新增 AuthorizeManualFire（Config §11.4 fire_once）
//   ATOM02-BUG-004  人工确认与解除生效分离（ConfirmSoftwareEstop / ReleaseSoftwareEstopLatch）
//   ATOM02-BUG-005  外部输入改为字段级写入；stop_fire 初值对齐 Config §2
//   ATOM02-BUG-006  fault_severity 参与判定（仅 SEVERE/ESTOP 全局阻断）
//   ATOM02-BUG-007  新增 IntentKind::SAFETY 安全动作豁免（Config §15、Interface §17.3）
//   ATOM02-BUG-008  run_id 收敛为唯一入口 BeginNewAutoFireRun
//   ATOM02-BUG-009  STOP_FIRE_SET 补模式校验（Config §11.4）
//   ATOM02-BUG-011  FAULT_DETECTED 通配边排除 INIT_FAILED
//   ATOM02-BUG-012  事件覆盖改为可计数的显式丢弃
//   ATOM02-BUG-002  授权函数剥离优先级比较（只做前置条件），
//                   优先级统一由 SubmitIntent/EvaluateIntent 的 FIRE 分支判定（方案 B）
//   ATOM02-BUG-004  遗留点 4：人工确认后控制源按 Config §7.2 正常选择，
//                   不再恒为 NONE（普通命令仍由急停锁存拒绝）
//   ATOM02-BUG-010  IDLE（未启动）收到 auto_stop / SBUS 接管终止为显式幂等空操作
// ============================================================================
#include "StateArbitration.hpp"

namespace marshland {
namespace state_arbitration {
namespace {

struct ModeEdge {
    SystemMode from;
    SystemMode to;
    TransitionReason reason;
};

// Interface §7.1 状态转换总表（逐行）
constexpr ModeEdge kModeEdges[] = {
    {SystemMode::UNINITIALIZED, SystemMode::INITIALIZING,  TransitionReason::START},
    {SystemMode::INITIALIZING,  SystemMode::STANDBY,       TransitionReason::INIT_SUCCESS},
    {SystemMode::INITIALIZING,  SystemMode::INIT_FAILED,   TransitionReason::INIT_FAILURE},
    {SystemMode::STANDBY,       SystemMode::MANUAL,        TransitionReason::ENTER_MANUAL},
    {SystemMode::STANDBY,       SystemMode::AUTO_FIRE,     TransitionReason::AUTO_START},
    {SystemMode::AUTO_FIRE,     SystemMode::MANUAL,        TransitionReason::SBUS_TAKEOVER},
    // Interface §9.2：WEB stop_fire_clear 执行前必须退出自动模式并转入手动模式
    {SystemMode::AUTO_FIRE,     SystemMode::MANUAL,        TransitionReason::WEB_MANUAL_TAKEOVER},
    {SystemMode::SOFTWARE_ESTOP,SystemMode::MANUAL,        TransitionReason::ESTOP_CONFIRM},
    {SystemMode::FAULT,         SystemMode::RECOVERY,      TransitionReason::RECOVERY_START},
    {SystemMode::RECOVERY,      SystemMode::MANUAL,        TransitionReason::RECOVERY_COMPLETE},
    // 以下 DEBUG/CALIBRATION 边为 ATOM-02 设计文档原子操作 14 的补全
    // （Interface §7.1 未单独列出调试/校准进出，需上层确认）
    {SystemMode::STANDBY,       SystemMode::DEBUG,         TransitionReason::DEBUG_ENTER},
    {SystemMode::MANUAL,        SystemMode::DEBUG,         TransitionReason::DEBUG_ENTER},
    {SystemMode::DEBUG,         SystemMode::STANDBY,       TransitionReason::DEBUG_EXIT},
    {SystemMode::DEBUG,         SystemMode::MANUAL,        TransitionReason::DEBUG_EXIT},
    {SystemMode::STANDBY,       SystemMode::CALIBRATION,   TransitionReason::CALIBRATION_ENTER},
    {SystemMode::MANUAL,        SystemMode::CALIBRATION,   TransitionReason::CALIBRATION_ENTER},
    {SystemMode::CALIBRATION,   SystemMode::STANDBY,       TransitionReason::CALIBRATION_EXIT},
    {SystemMode::CALIBRATION,   SystemMode::MANUAL,        TransitionReason::CALIBRATION_EXIT},
};

struct AutoFireEdge {
    AutoFireState from;
    AutoFireState to;
    ShotStage stage;   // ShotStage::ANY 为通配边
};

// Interface §6.3 + §7.1 + §8.1/§8.2 的自动流程推进
// ATOM02-BUG-001：FORCE 的出边按发次阶段区分，SHOT_N_YAW 不再是孤儿节点
constexpr AutoFireEdge kAutoFireEdges[] = {
    {AutoFireState::IDLE,           AutoFireState::AUTO_PREPARE,   ShotStage::ANY},
    {AutoFireState::AUTO_PREPARE,   AutoFireState::CHARGE,         ShotStage::ANY},
    {AutoFireState::CHARGE,         AutoFireState::FORCE,          ShotStage::ANY},
    {AutoFireState::CHARGE,         AutoFireState::RELOAD_LIFT,    ShotStage::ANY},
    {AutoFireState::FORCE,          AutoFireState::SHOT_1_YAW,     ShotStage::FIRST},
    {AutoFireState::FORCE,          AutoFireState::SHOT_N_YAW,     ShotStage::SUBSEQUENT},
    {AutoFireState::FORCE,          AutoFireState::RELOAD_LIFT,    ShotStage::ANY},
    {AutoFireState::SHOT_1_YAW,     AutoFireState::FIRE_READY,     ShotStage::ANY},
    {AutoFireState::SHOT_N_YAW,     AutoFireState::FIRE_READY,     ShotStage::ANY},
    {AutoFireState::RELOAD_LIFT,    AutoFireState::FEED,           ShotStage::ANY},
    {AutoFireState::FEED,           AutoFireState::FEED_WAIT,      ShotStage::ANY},
    {AutoFireState::FEED_WAIT,      AutoFireState::RELOAD_LOWER,   ShotStage::ANY},
    {AutoFireState::RELOAD_LOWER,   AutoFireState::FORCE,          ShotStage::ANY},
    {AutoFireState::FIRE_READY,     AutoFireState::FIRING,         ShotStage::ANY},
    {AutoFireState::FIRING,         AutoFireState::FIRE_COMPLETED, ShotStage::ANY},
    {AutoFireState::FIRE_COMPLETED, AutoFireState::CHARGE,         ShotStage::ANY},
    {AutoFireState::FIRE_COMPLETED, AutoFireState::AUTO_COMPLETED, ShotStage::ANY},
};

// Interface §7.2 第 1 条：未完成初始化不得进入自动发射或手动动作状态。
// 刻意不含 RECOVERY / SOFTWARE_ESTOP：恢复期与急停期的安全动作走
// IntentKind::SAFETY 豁免，而不是放宽本闸门（ATOM02-BUG-007）。
bool IsOperationalMode(SystemMode mode) noexcept
{
    return mode == SystemMode::STANDBY || mode == SystemMode::MANUAL ||
           mode == SystemMode::AUTO_FIRE || mode == SystemMode::DEBUG ||
           mode == SystemMode::CALIBRATION;
}

// Config §11.4：stop_fire_set / stop_fire_clear 允许模式为「手动、调试」
bool IsStopFireSetMode(SystemMode mode) noexcept
{
    return mode == SystemMode::MANUAL || mode == SystemMode::DEBUG;
}

bool StageMatches(ShotStage edge_stage, ShotStage stage) noexcept
{
    return edge_stage == ShotStage::ANY || edge_stage == stage;
}

} // namespace

bool IsAutoFireStateTerminal(AutoFireState state) noexcept
{
    return state == AutoFireState::AUTO_COMPLETED || state == AutoFireState::AUTO_TERMINATED;
}

bool IsAutoFireTransitionAllowed(AutoFireState from, AutoFireState to) noexcept
{
    for (const AutoFireEdge& edge : kAutoFireEdges) {
        if (edge.from == from && edge.to == to) {
            return true;
        }
    }
    return false;
}

bool IsAutoFireTransitionAllowed(AutoFireState from, AutoFireState to, ShotStage stage) noexcept
{
    for (const AutoFireEdge& edge : kAutoFireEdges) {
        if (edge.from == from && edge.to == to && StageMatches(edge.stage, stage)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Interface §7.1 + 急停/故障通配边
// ---------------------------------------------------------------------------
bool IsModeTransitionAllowed(SystemMode from, SystemMode to, TransitionReason reason) noexcept
{
    // 软件急停为最高安全覆盖：任意状态（急停自身除外）可进入急停
    if (reason == TransitionReason::SOFTWARE_ESTOP_TRIGGER) {
        return to == SystemMode::SOFTWARE_ESTOP && from != SystemMode::SOFTWARE_ESTOP;
    }
    // 故障进入：初始化失败路径已由 INIT_FAILURE 承担。
    // ATOM02-BUG-011：INIT_FAILED 也必须排除，否则初始化失败的设备可经
    // FAULT -> RECOVERY -> MANUAL 进入可操作状态，违反 ATOM-01 验收条件与
    // Config §3.1「初始化失败，禁止执行器动作」。
    if (reason == TransitionReason::FAULT_DETECTED) {
        return to == SystemMode::FAULT && from != SystemMode::SOFTWARE_ESTOP &&
               from != SystemMode::FAULT && from != SystemMode::UNINITIALIZED &&
               from != SystemMode::INITIALIZING && from != SystemMode::INIT_FAILED;
    }
    for (const ModeEdge& edge : kModeEdges) {
        if (edge.from == from && edge.to == to && edge.reason == reason) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Config §14.2：故障等级到阻断行为的归约（ATOM02-BUG-006）
// ---------------------------------------------------------------------------
bool IsFaultBlocking(const ExternalInputs& inputs) noexcept
{
    if (!inputs.fault_active) {
        return false;
    }
    // 信息级（如 W-VISION-002 正常回退）与警告级（如 W-SBUS-001、F-VISION-001）
    // 属于可运行的降级状态，不全局阻断；只有严重与急停等级全局阻断。
    return inputs.fault_severity == FaultSeverity::SEVERE ||
           inputs.fault_severity == FaultSeverity::ESTOP;
}

// ---------------------------------------------------------------------------
// Config §7.2 控制源选择规则
// ---------------------------------------------------------------------------
ControlSource SelectControlSource(const ArbitrationContext& ctx) noexcept
{
    // 软件急停覆盖全部普通控制源，不参与普通控制源排序（Config §7.2 末行）。
    // ATOM02-BUG-004 遗留点 4：覆盖只在"急停尚未解除"期间成立，即模式仍是
    // SOFTWARE_ESTOP 时。人工确认（SOFTWARE_ESTOP -> MANUAL）之后系统已回到
    // 手动控制，控制源按 Config §7.2 正常选择；普通命令仍由急停锁存拒绝，
    // 直到 ATOM-14 完成解除动作并调用 ReleaseSoftwareEstopLatch()。
    if (ctx.inputs.software_estop && ctx.system_mode == SystemMode::SOFTWARE_ESTOP) {
        return ControlSource::NONE;
    }
    if (ctx.inputs.sbus_online) {
        return ctx.inputs.sbus_auto_mode ? ControlSource::AUTO : ControlSource::SBUS;
    }
    if (ctx.inputs.web_online) {
        return ControlSource::WEB;
    }
    return ControlSource::AUTO;
}

// ---------------------------------------------------------------------------
// 实际发射授权：SBUS > WEB > 自动流程（Interface §3.2、§6.1、§7.2 第 3 条）
// ---------------------------------------------------------------------------
FireAuthorization EvaluateFireAuthorization(const ArbitrationContext& ctx,
                                            ControlSource requesting_source) noexcept
{
    FireAuthorization auth{};
    auth.requesting_source = requesting_source;
    auth.control_source = ctx.control_source;
    auth.authorizing_source = ctx.control_source;
    auth.estop = ctx.inputs.software_estop;
    auth.fault = IsFaultBlocking(ctx.inputs);
    auth.stop_fire = ctx.inputs.stop_fire;
    auth.yaw_not_arrived = !ctx.inputs.yaw_arrived;

    if (requesting_source == ControlSource::NONE) {
        auth.reason = RejectReason::INVALID_INTENT;
        auth.authorizing_source = ControlSource::NONE;
        return auth;
    }
    // 软件急停高于所有普通控制源（Interface §3.2）
    if (auth.estop) {
        auth.reason = RejectReason::SOFTWARE_ESTOP;
        auth.authorizing_source = ControlSource::NONE;
        return auth;
    }
    if (auth.fault) {
        auth.reason = RejectReason::FAULT_ACTIVE;
        return auth;
    }
    if (auth.stop_fire) {
        auth.reason = RejectReason::STOP_FIRE_SET;
        return auth;
    }
    // Interface §7.2 第 2 条：YAW 到位不为 True 时不得进入实际发射动作
    if (auth.yaw_not_arrived) {
        auth.reason = RejectReason::YAW_NOT_ARRIVED;
        return auth;
    }
    // ATOM02-BUG-002（方案 B）：本函数只做前置条件复核（急停 / 故障 / 停止发射 /
    // YAW 到位）。「实际发射授权优先级 SBUS > WEB > 自动流程」不在本函数判定，
    // 统一由意图路径承担：SubmitIntent -> EvaluateIntent 的 FIRE 分支先做优先级
    // 仲裁，再调用本函数。故 priority_conflict 恒为 false（字段保留以兼容类型）。
    auth.allowed = true;
    auth.authorizing_source = requesting_source;
    auth.reason = RejectReason::NONE;
    return auth;
}

// ---------------------------------------------------------------------------
// 普通控制意图仲裁（ATOM-02 原子操作 6/7/8/12）
// ---------------------------------------------------------------------------
IntentDecision EvaluateIntent(const ArbitrationContext& ctx, const ControlIntent& intent) noexcept
{
    IntentDecision decision{};
    decision.source = intent.source;
    decision.kind = intent.kind;
    decision.control_source = ctx.control_source;

    if (!intent.valid || intent.source == ControlSource::NONE) {
        decision.reason = RejectReason::INVALID_INTENT;
        return decision;
    }

    // Config §15「安全动作例外」+ Interface §17.3 第 7 条（ATOM02-BUG-007）：
    // 零力矩、换弹抬起、急停人工确认后的蓄力机构回零、一般故障恢复动作
    // 不受普通命令拒绝规则影响，故在急停/故障/模式闸门之前直接放行。
    if (intent.kind == IntentKind::SAFETY) {
        decision.accepted = true;
        return decision;
    }

    // 置停止发射标志是安全单调动作：只收紧约束，不受优先级与急停阻断；
    // 但允许模式仍受 Config §11.4 约束（ATOM02-BUG-009）。
    // 注意：SBUS CH7 / 视觉的停止发射标志走 ExternalInputs 输入路径，不经此处。
    if (intent.kind == IntentKind::STOP_FIRE_SET) {
        if (!IsStopFireSetMode(ctx.system_mode)) {
            decision.reason = RejectReason::NOT_READY;
            return decision;
        }
        decision.accepted = true;
        return decision;
    }

    // 原子操作 12 / Config §15：急停为 True 时拒绝普通控制意图
    if (ctx.inputs.software_estop) {
        decision.reason = RejectReason::SOFTWARE_ESTOP;
        return decision;
    }

    // Interface §9.2 第三条停止发射标志清除路径：仅手动/调试模式，且必须已退出自动流程
    if (intent.kind == IntentKind::STOP_FIRE_CLEAR) {
        if (intent.source != ControlSource::WEB && intent.source != ControlSource::SBUS) {
            decision.reason = RejectReason::LOWER_PRIORITY_CONFLICT;
            return decision;
        }
        if (!IsStopFireSetMode(ctx.system_mode)) {
            decision.reason = RejectReason::NOT_READY;
            return decision;
        }
        if (ctx.auto_fire_state != AutoFireState::IDLE &&
            !IsAutoFireStateTerminal(ctx.auto_fire_state)) {
            decision.reason = RejectReason::AUTO_FLOW_ACTIVE;
            return decision;
        }
        decision.accepted = true;
        return decision;
    }

    if (IsFaultBlocking(ctx.inputs)) {
        decision.reason = RejectReason::FAULT_ACTIVE;
        return decision;
    }

    if (!IsOperationalMode(ctx.system_mode)) {
        decision.reason = RejectReason::NOT_READY;
        return decision;
    }

    if (intent.kind == IntentKind::AUTO_START && ctx.system_mode != SystemMode::STANDBY) {
        decision.reason = RejectReason::NOT_READY;
        return decision;
    }
    if (intent.kind == IntentKind::AUTO_STOP && ctx.system_mode != SystemMode::AUTO_FIRE) {
        decision.reason = RejectReason::NOT_READY;
        return decision;
    }

    // 发射意图：先按 SBUS > WEB > 自动流程 仲裁优先级（原子操作 8），
    // 再走发射前复核（ATOM02-BUG-002 方案 B：优先级只在意图路径判定）
    if (intent.kind == IntentKind::FIRE) {
        if (ControlSourceRank(intent.source) < ControlSourceRank(ctx.control_source)) {
            decision.reason = RejectReason::LOWER_PRIORITY_CONFLICT;
            return decision;
        }
        const FireAuthorization auth = EvaluateFireAuthorization(ctx, intent.source);
        decision.accepted = auth.allowed;
        decision.reason = auth.reason;
        return decision;
    }

    // 原子操作 8：低优先级冲突命令被拒绝，且不修改控制目标
    if (ControlSourceRank(intent.source) < ControlSourceRank(ctx.control_source)) {
        decision.reason = RejectReason::LOWER_PRIORITY_CONFLICT;
        return decision;
    }

    decision.accepted = true;
    return decision;
}

// ---------------------------------------------------------------------------
// 日志辅助
// ---------------------------------------------------------------------------
const char* ToString(TransitionReason reason) noexcept
{
    switch (reason) {
    case TransitionReason::NONE:                    return "NONE";
    case TransitionReason::START:                   return "START";
    case TransitionReason::INIT_SUCCESS:            return "INIT_SUCCESS";
    case TransitionReason::INIT_FAILURE:            return "INIT_FAILURE";
    case TransitionReason::ENTER_MANUAL:            return "ENTER_MANUAL";
    case TransitionReason::AUTO_START:              return "AUTO_START";
    case TransitionReason::SBUS_TAKEOVER:           return "SBUS_TAKEOVER";
    case TransitionReason::WEB_MANUAL_TAKEOVER:     return "WEB_MANUAL_TAKEOVER";
    case TransitionReason::SOFTWARE_ESTOP_TRIGGER:  return "SOFTWARE_ESTOP_TRIGGER";
    case TransitionReason::ESTOP_CONFIRM:           return "ESTOP_CONFIRM";
    case TransitionReason::ESTOP_RELEASED:          return "ESTOP_RELEASED";
    case TransitionReason::FAULT_DETECTED:          return "FAULT_DETECTED";
    case TransitionReason::RECOVERY_START:          return "RECOVERY_START";
    case TransitionReason::RECOVERY_COMPLETE:       return "RECOVERY_COMPLETE";
    case TransitionReason::DEBUG_ENTER:             return "DEBUG_ENTER";
    case TransitionReason::DEBUG_EXIT:              return "DEBUG_EXIT";
    case TransitionReason::CALIBRATION_ENTER:       return "CALIBRATION_ENTER";
    case TransitionReason::CALIBRATION_EXIT:        return "CALIBRATION_EXIT";
    case TransitionReason::AUTO_FLOW_RUN:           return "AUTO_FLOW_RUN";
    case TransitionReason::AUTO_FLOW_STEP:          return "AUTO_FLOW_STEP";
    case TransitionReason::AUTO_FLOW_DONE:          return "AUTO_FLOW_DONE";
    case TransitionReason::AUTO_FLOW_ABORT:         return "AUTO_FLOW_ABORT";
    case TransitionReason::CONTROL_SOURCE_REFRESH:  return "CONTROL_SOURCE_REFRESH";
    }
    return "UNKNOWN";
}

const char* ToString(RejectReason reason) noexcept
{
    switch (reason) {
    case RejectReason::NONE:                    return "NONE";
    case RejectReason::INVALID_INTENT:          return "INVALID_INTENT";
    case RejectReason::UNKNOWN_TRANSITION:      return "UNKNOWN_TRANSITION";
    case RejectReason::ALREADY_IN_TARGET:       return "ALREADY_IN_TARGET";
    case RejectReason::NOT_READY:               return "NOT_READY";
    case RejectReason::NOT_AUTO_FIRE_MODE:      return "NOT_AUTO_FIRE_MODE";
    case RejectReason::AUTO_FLOW_ACTIVE:        return "AUTO_FLOW_ACTIVE";
    case RejectReason::AUTO_FLOW_TERMINATED:    return "AUTO_FLOW_TERMINATED";
    case RejectReason::AUTO_FLOW_NOT_STARTED:   return "AUTO_FLOW_NOT_STARTED";
    case RejectReason::NOT_SBUS_TAKEOVER:       return "NOT_SBUS_TAKEOVER";
    case RejectReason::SHOT_STAGE_MISMATCH:     return "SHOT_STAGE_MISMATCH";
    case RejectReason::SOFTWARE_ESTOP:          return "SOFTWARE_ESTOP";
    case RejectReason::ESTOP_NOT_CONFIRMED:     return "ESTOP_NOT_CONFIRMED";
    case RejectReason::FAULT_ACTIVE:            return "FAULT_ACTIVE";
    case RejectReason::STOP_FIRE_SET:           return "STOP_FIRE_SET";
    case RejectReason::YAW_NOT_ARRIVED:         return "YAW_NOT_ARRIVED";
    case RejectReason::LOWER_PRIORITY_CONFLICT: return "LOWER_PRIORITY_CONFLICT";
    case RejectReason::FIRE_NOT_AUTHORIZED:     return "FIRE_NOT_AUTHORIZED";
    }
    return "UNKNOWN";
}

const char* ToString(IntentKind kind) noexcept
{
    switch (kind) {
    case IntentKind::CONTROL:         return "CONTROL";
    case IntentKind::FIRE:            return "FIRE";
    case IntentKind::SAFETY:          return "SAFETY";
    case IntentKind::STOP_FIRE_SET:   return "STOP_FIRE_SET";
    case IntentKind::STOP_FIRE_CLEAR: return "STOP_FIRE_CLEAR";
    case IntentKind::AUTO_START:      return "AUTO_START";
    case IntentKind::AUTO_STOP:       return "AUTO_STOP";
    }
    return "UNKNOWN";
}

const char* ToString(EventKind kind) noexcept
{
    switch (kind) {
    case EventKind::STATE_TRANSITION:      return "STATE_TRANSITION";
    case EventKind::AUTO_FIRE_TRANSITION:  return "AUTO_FIRE_TRANSITION";
    case EventKind::CONTROL_SOURCE_CHANGE: return "CONTROL_SOURCE_CHANGE";
    case EventKind::INTENT_ACCEPTED:       return "INTENT_ACCEPTED";
    case EventKind::INTENT_REJECTED:       return "INTENT_REJECTED";
    case EventKind::FIRE_DECISION:         return "FIRE_DECISION";
    case EventKind::ESTOP_RELEASED:        return "ESTOP_RELEASED";
    }
    return "UNKNOWN";
}

const char* ToString(ShotStage stage) noexcept
{
    switch (stage) {
    case ShotStage::FIRST:      return "FIRST";
    case ShotStage::SUBSEQUENT: return "SUBSEQUENT";
    case ShotStage::ANY:        return "ANY";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// StateArbitrator
// ---------------------------------------------------------------------------
void StateArbitrator::Reset(std::uint64_t timestamp_us) noexcept
{
    ctx_ = ArbitrationContext{};
    ctx_.inputs.timestamp_us = timestamp_us;
    ctx_.system_mode = SystemMode::UNINITIALIZED;
    ctx_.auto_fire_state = AutoFireState::IDLE;
    ctx_.control_source = ControlSource::AUTO; // Config §7.2 初始控制源
    events_.fill(StateEvent{});
    event_count_ = 0;
    dropped_event_count_ = 0;
    head_ = 0;
    sequence_ = 0;
    run_id_ = 0;
    current_shot_ = 0;
    estop_latched_ = false;
}

void StateArbitrator::PushEvent(StateEvent event) noexcept
{
    event.sequence = ++sequence_;
    if (event.timestamp_us == 0) {
        event.timestamp_us = ctx_.inputs.timestamp_us;
    }
    if (event.run_id == 0) {
        event.run_id = run_id_;
    }
    // ATOM02-BUG-012：覆盖最旧事件时显式计数，避免静默丢失
    if (event_count_ == kMaxEvents) {
        ++dropped_event_count_;
    } else {
        ++event_count_;
    }
    events_[head_] = event;
    head_ = (head_ + 1u) % kMaxEvents;
}

bool StateArbitrator::is_auto_fire_active() const noexcept
{
    return ctx_.auto_fire_state != AutoFireState::IDLE &&
           !IsAutoFireStateTerminal(ctx_.auto_fire_state);
}

ShotStage StateArbitrator::shot_stage() const noexcept
{
    return (current_shot_ <= 1u) ? ShotStage::FIRST : ShotStage::SUBSEQUENT;
}

ControlSource StateArbitrator::EvaluateControlSource() const noexcept
{
    return SelectControlSource(ctx_);
}

ControlSourceChange StateArbitrator::RefreshControlSourceInternal(TransitionReason reason) noexcept
{
    ControlSourceChange change{};
    change.from = ctx_.control_source;
    change.reason = reason;

    const ControlSource next = SelectControlSource(ctx_);
    change.to = next;
    if (next == ctx_.control_source) {
        return change;
    }

    ctx_.control_source = next;
    change.changed = true;

    StateEvent event{};
    event.kind = EventKind::CONTROL_SOURCE_CHANGE;
    event.reason = reason;
    event.from_source = change.from;
    event.to_source = change.to;
    PushEvent(event);
    return change;
}

ControlSourceChange StateArbitrator::RefreshControlSource() noexcept
{
    return RefreshControlSourceInternal(TransitionReason::CONTROL_SOURCE_REFRESH);
}

ControlSourceChange StateArbitrator::SyncEstopAndRefresh() noexcept
{
    // 急停置位时同步进入 SOFTWARE_ESTOP（Interface §7.1 通配边）
    if (ctx_.inputs.software_estop && ctx_.system_mode != SystemMode::SOFTWARE_ESTOP &&
        IsModeTransitionAllowed(ctx_.system_mode, SystemMode::SOFTWARE_ESTOP,
                                TransitionReason::SOFTWARE_ESTOP_TRIGGER)) {
        const ControlSource before = ctx_.control_source;
        RequestModeTransition(SystemMode::SOFTWARE_ESTOP,
                              TransitionReason::SOFTWARE_ESTOP_TRIGGER);
        ControlSourceChange change{};
        change.from = before;
        change.to = ctx_.control_source;
        change.changed = (change.from != change.to);
        change.reason = TransitionReason::CONTROL_SOURCE_REFRESH;
        return change;
    }
    return RefreshControlSourceInternal(TransitionReason::CONTROL_SOURCE_REFRESH);
}

// ---- 字段级外部输入（ATOM02-BUG-005） -------------------------------------
ControlSourceChange StateArbitrator::SetSbusStatus(bool online, bool auto_mode) noexcept
{
    ctx_.inputs.sbus_online = online;
    ctx_.inputs.sbus_auto_mode = auto_mode;
    return RefreshControlSourceInternal(TransitionReason::CONTROL_SOURCE_REFRESH);
}

ControlSourceChange StateArbitrator::SetWebOnline(bool online) noexcept
{
    ctx_.inputs.web_online = online;
    return RefreshControlSourceInternal(TransitionReason::CONTROL_SOURCE_REFRESH);
}

ControlSourceChange StateArbitrator::SetSoftwareEstopInput(bool active) noexcept
{
    // Config §15：急停为锁定变量，置位后只有解除动作完成才能清
    if (active) {
        estop_latched_ = true;
    }
    ctx_.inputs.software_estop = estop_latched_;
    return SyncEstopAndRefresh();
}

void StateArbitrator::SetStopFire(bool active) noexcept
{
    ctx_.inputs.stop_fire = active;
}

void StateArbitrator::SetYawArrived(bool arrived) noexcept
{
    ctx_.inputs.yaw_arrived = arrived;
}

void StateArbitrator::SetFault(bool active, FaultSeverity severity) noexcept
{
    ctx_.inputs.fault_active = active;
    ctx_.inputs.fault_severity = severity;
}

void StateArbitrator::SetClock(std::uint64_t timestamp_us, std::uint64_t sequence) noexcept
{
    ctx_.inputs.timestamp_us = timestamp_us;
    ctx_.inputs.sequence = sequence;
}

ControlSourceChange StateArbitrator::UpdateInputs(const ExternalInputs& inputs) noexcept
{
    if (inputs.software_estop) {
        estop_latched_ = true;
    }
    ctx_.inputs = inputs;
    if (estop_latched_) {
        ctx_.inputs.software_estop = true;
    }
    return SyncEstopAndRefresh();
}

void StateArbitrator::TerminateAutoFlowInternal(TransitionReason reason) noexcept
{
    if (!is_auto_fire_active()) {
        return;
    }
    StateEvent event{};
    event.kind = EventKind::AUTO_FIRE_TRANSITION;
    event.reason = reason;
    event.from_auto_fire = ctx_.auto_fire_state;
    event.to_auto_fire = AutoFireState::AUTO_TERMINATED;
    ctx_.auto_fire_state = AutoFireState::AUTO_TERMINATED;
    PushEvent(event);
}

ModeTransitionResult StateArbitrator::RequestModeTransition(SystemMode target,
                                                            TransitionReason reason) noexcept
{
    ModeTransitionResult result{};
    result.from = ctx_.system_mode;
    result.to = target;
    result.transition_reason = reason;

    if (target == ctx_.system_mode) {
        result.reason = RejectReason::ALREADY_IN_TARGET;
        return result;
    }

    if (!IsModeTransitionAllowed(ctx_.system_mode, target, reason)) {
        result.reason = RejectReason::UNKNOWN_TRANSITION;
        StateEvent event{};
        event.kind = EventKind::INTENT_REJECTED;
        event.reason = reason;
        event.reject_reason = result.reason;
        event.from_mode = ctx_.system_mode;
        event.to_mode = target;
        PushEvent(event);
        return result;
    }

    const SystemMode from = ctx_.system_mode;
    ctx_.system_mode = target;
    result.accepted = true;

    // 进入急停：置位并锁存（Config §15）。
    // ATOM02-BUG-004：ESTOP_CONFIRM 只做模式迁移，**不清零**急停变量与锁存；
    // 必须等 ATOM-14 完成换弹抬起 + 蓄力双电机回零 + 双限位低电平后，
    // 由 ReleaseSoftwareEstopLatch() 解除。
    if (target == SystemMode::SOFTWARE_ESTOP) {
        ctx_.inputs.software_estop = true;
        estop_latched_ = true;
    }

    // 离开自动发射 / 进入急停 / 进入故障：终止原自动流程，不暂停、不恢复
    if ((from == SystemMode::AUTO_FIRE && target == SystemMode::MANUAL) ||
        target == SystemMode::SOFTWARE_ESTOP || target == SystemMode::FAULT) {
        TerminateAutoFlowInternal(reason);
    }

    RefreshControlSourceInternal(TransitionReason::CONTROL_SOURCE_REFRESH);

    StateEvent event{};
    event.kind = EventKind::STATE_TRANSITION;
    event.reason = reason;
    event.from_mode = from;
    event.to_mode = target;
    PushEvent(event);
    return result;
}

ModeTransitionResult StateArbitrator::TriggerSoftwareEstop() noexcept
{
    if (ctx_.system_mode == SystemMode::SOFTWARE_ESTOP) {
        estop_latched_ = true;
        ctx_.inputs.software_estop = true;
        ModeTransitionResult result{};
        result.from = SystemMode::SOFTWARE_ESTOP;
        result.to = SystemMode::SOFTWARE_ESTOP;
        result.transition_reason = TransitionReason::SOFTWARE_ESTOP_TRIGGER;
        result.reason = RejectReason::ALREADY_IN_TARGET;
        return result;
    }
    return RequestModeTransition(SystemMode::SOFTWARE_ESTOP,
                                 TransitionReason::SOFTWARE_ESTOP_TRIGGER);
}

ModeTransitionResult StateArbitrator::ConfirmSoftwareEstop() noexcept
{
    return RequestModeTransition(SystemMode::MANUAL, TransitionReason::ESTOP_CONFIRM);
}

bool StateArbitrator::ReleaseSoftwareEstopLatch() noexcept
{
    // 前置：锁定仍在，且人工确认已完成（模式已离开 SOFTWARE_ESTOP）
    if (!estop_latched_) {
        return false;
    }
    if (ctx_.system_mode == SystemMode::SOFTWARE_ESTOP) {
        return false; // 尚未人工确认
    }

    estop_latched_ = false;
    ctx_.inputs.software_estop = false;

    StateEvent event{};
    event.kind = EventKind::ESTOP_RELEASED;
    event.reason = TransitionReason::ESTOP_RELEASED;
    event.from_mode = ctx_.system_mode;
    event.to_mode = ctx_.system_mode;
    PushEvent(event);

    RefreshControlSourceInternal(TransitionReason::CONTROL_SOURCE_REFRESH);
    return true;
}

AutoFireTransitionResult StateArbitrator::RequestAutoFireState(AutoFireState target) noexcept
{
    AutoFireTransitionResult result{};
    result.from = ctx_.auto_fire_state;
    result.to = target;

    if (target == ctx_.auto_fire_state) {
        result.reason = RejectReason::ALREADY_IN_TARGET;
        return result;
    }
    // ATOM02-BUG-008：AUTO_PREPARE 只有 BeginNewAutoFireRun 一条路径，
    // 避免两条 run_id 自增入口；也保证 AUTO_COMPLETED/AUTO_TERMINATED 后
    // 开启新一轮时行为可预测（而不是 UNKNOWN_TRANSITION）。
    if (target == AutoFireState::AUTO_PREPARE) {
        return BeginNewAutoFireRun();
    }
    // 原子操作 11：已终止/已完成的自动流程不得续跑
    if (IsAutoFireStateTerminal(ctx_.auto_fire_state)) {
        result.reason = RejectReason::AUTO_FLOW_TERMINATED;
        return result;
    }
    if (ctx_.system_mode != SystemMode::AUTO_FIRE) {
        result.reason = RejectReason::NOT_AUTO_FIRE_MODE;
        return result;
    }

    TransitionReason reason = TransitionReason::AUTO_FLOW_STEP;
    bool accepted = false;

    if (target == AutoFireState::AUTO_TERMINATED) {
        // ATOM02-BUG-010：IDLE 表示"未启动"，等价于已停止（Config §11.4 的
        // auto_stop 允许模式为任意普通模式），此时为幂等空操作，不改变状态。
        if (!is_auto_fire_active()) {
            result.reason = RejectReason::AUTO_FLOW_NOT_STARTED;
            return result;
        }
        // 主动终止（如 WEB auto_stop）：只终止正在运行的流程，不暂停、不恢复
        accepted = true;
        reason = TransitionReason::AUTO_FLOW_ABORT;
    } else if (target == AutoFireState::AUTO_COMPLETED) {
        accepted = IsAutoFireTransitionAllowed(ctx_.auto_fire_state, target, shot_stage());
        reason = TransitionReason::AUTO_FLOW_DONE;
        // Interface §6.6 total_shots = 4：只有第 4 发完成后才能进入 AUTO_COMPLETED
        if (accepted && current_shot_ < kTotalShots) {
            result.reason = RejectReason::SHOT_STAGE_MISMATCH;
            return result;
        }
    } else if (target == AutoFireState::FIRING) {
        // 发射前待命 -> 发射中：停止标志为 False 且授权通过（Interface §7.1、§7.2）
        const FireAuthorization auth = EvaluateFireAuthorization(ctx_, ControlSource::AUTO);
        if (!auth.allowed) {
            result.reason = (auth.reason == RejectReason::NONE) ? RejectReason::FIRE_NOT_AUTHORIZED
                                                                : auth.reason;
            StateEvent denied{};
            denied.kind = EventKind::FIRE_DECISION;
            denied.reject_reason = result.reason;
            denied.reason = TransitionReason::AUTO_FLOW_STEP;
            denied.from_auto_fire = ctx_.auto_fire_state;
            denied.to_auto_fire = target;
            PushEvent(denied);
            return result;
        }
        accepted = IsAutoFireTransitionAllowed(ctx_.auto_fire_state, target, shot_stage());
    } else {
        accepted = IsAutoFireTransitionAllowed(ctx_.auto_fire_state, target, shot_stage());
        // Interface §6.6：4 发打完后 FIRE_COMPLETED 只能进 AUTO_COMPLETED
        if (accepted && ctx_.auto_fire_state == AutoFireState::FIRE_COMPLETED &&
            target == AutoFireState::CHARGE && current_shot_ >= kTotalShots) {
            result.reason = RejectReason::SHOT_STAGE_MISMATCH;
            return result;
        }
    }

    if (!accepted) {
        result.reason = RejectReason::UNKNOWN_TRANSITION;
        return result;
    }

    const AutoFireState from = ctx_.auto_fire_state;
    // FIRE_COMPLETED -> CHARGE 表示进入下一发（发次计数推进）
    if (from == AutoFireState::FIRE_COMPLETED && target == AutoFireState::CHARGE &&
        current_shot_ < kTotalShots) {
        ++current_shot_;
    }
    ctx_.auto_fire_state = target;
    result.accepted = true;
    result.transition_reason = reason;

    StateEvent event{};
    event.kind = EventKind::AUTO_FIRE_TRANSITION;
    event.reason = reason;
    event.from_auto_fire = from;
    event.to_auto_fire = target;
    PushEvent(event);
    return result;
}

AutoFireTransitionResult StateArbitrator::TerminateAutoFireOnSbusTakeover() noexcept
{
    AutoFireTransitionResult result{};
    result.from = ctx_.auto_fire_state;
    result.to = AutoFireState::AUTO_TERMINATED;

    if (ctx_.auto_fire_state == AutoFireState::AUTO_TERMINATED) {
        result.reason = RejectReason::ALREADY_IN_TARGET;
        return result;
    }
    // AUTO_COMPLETED 已完成（不可再终止）；IDLE 未启动（幂等空操作，ATOM02-BUG-010）
    if (ctx_.auto_fire_state == AutoFireState::AUTO_COMPLETED) {
        result.reason = RejectReason::AUTO_FLOW_TERMINATED;
        return result;
    }
    if (ctx_.auto_fire_state == AutoFireState::IDLE) {
        result.reason = RejectReason::AUTO_FLOW_NOT_STARTED;
        return result;
    }
    // SBUS 接管条件：SBUS 在线且自动模式标志为 False（Config §7.2 第 4 行）
    if (!ctx_.inputs.sbus_online || ctx_.inputs.sbus_auto_mode) {
        result.reason = RejectReason::NOT_SBUS_TAKEOVER;
        return result;
    }

    // 允许当前硬件动作组完成由调用方保证（原子操作 10）：本模块只置终止位
    const AutoFireState from = ctx_.auto_fire_state;
    ctx_.auto_fire_state = AutoFireState::AUTO_TERMINATED;
    result.accepted = true;
    result.transition_reason = TransitionReason::SBUS_TAKEOVER;

    StateEvent event{};
    event.kind = EventKind::AUTO_FIRE_TRANSITION;
    event.reason = TransitionReason::SBUS_TAKEOVER;
    event.from_auto_fire = from;
    event.to_auto_fire = AutoFireState::AUTO_TERMINATED;
    PushEvent(event);

    // 任意自动状态 SBUS 接管 -> 手动（Interface §7.1）
    if (ctx_.system_mode == SystemMode::AUTO_FIRE) {
        RequestModeTransition(SystemMode::MANUAL, TransitionReason::SBUS_TAKEOVER);
    }
    return result;
}

AutoFireTransitionResult StateArbitrator::BeginNewAutoFireRun() noexcept
{
    AutoFireTransitionResult result{};
    result.from = ctx_.auto_fire_state;
    result.to = AutoFireState::AUTO_PREPARE;

    if (ctx_.system_mode != SystemMode::AUTO_FIRE) {
        result.reason = RejectReason::NOT_AUTO_FIRE_MODE;
        return result;
    }
    if (is_auto_fire_active()) {
        result.reason = RejectReason::AUTO_FLOW_ACTIVE;
        return result;
    }

    // 唯一允许离开 AUTO_COMPLETED / AUTO_TERMINATED / IDLE 的入口（新 run_id、第 1 发）
    const AutoFireState from = ctx_.auto_fire_state;
    ++run_id_;
    current_shot_ = 1u;
    ctx_.auto_fire_state = AutoFireState::AUTO_PREPARE;
    result.accepted = true;
    result.transition_reason = TransitionReason::AUTO_FLOW_RUN;

    StateEvent event{};
    event.kind = EventKind::AUTO_FIRE_TRANSITION;
    event.reason = TransitionReason::AUTO_FLOW_RUN;
    event.from_auto_fire = from;
    event.to_auto_fire = AutoFireState::AUTO_PREPARE;
    event.run_id = run_id_;
    PushEvent(event);
    return result;
}

IntentDecision StateArbitrator::SubmitIntent(const ControlIntent& intent) noexcept
{
    const IntentDecision decision = EvaluateIntent(ctx_, intent);

    StateEvent event{};
    event.kind = decision.accepted ? EventKind::INTENT_ACCEPTED : EventKind::INTENT_REJECTED;
    event.reject_reason = decision.reason;
    event.from_source = intent.source;
    event.to_source = ctx_.control_source; // 控制目标不被修改
    event.request_id = intent.request_id;
    event.timestamp_us = intent.timestamp_us;
    PushEvent(event);
    return decision;
}

FireAuthorization StateArbitrator::AuthorizeFire(ControlSource requesting_source) noexcept
{
    const FireAuthorization auth = EvaluateFireAuthorization(ctx_, requesting_source);

    StateEvent event{};
    event.kind = EventKind::FIRE_DECISION;
    event.reject_reason = auth.reason;
    event.from_source = requesting_source;
    event.to_source = auth.authorizing_source;
    PushEvent(event);
    return auth;
}

FireAuthorization StateArbitrator::AuthorizeManualFire(ControlSource requesting_source) noexcept
{
    FireAuthorization auth = EvaluateFireAuthorization(ctx_, requesting_source);

    // Config §11.4 fire_once：允许模式为「手动、调试」，请求源为 SBUS 或 WEB
    if (auth.allowed && ctx_.system_mode != SystemMode::MANUAL &&
        ctx_.system_mode != SystemMode::DEBUG) {
        auth.allowed = false;
        auth.reason = RejectReason::NOT_READY;
    } else if (auth.allowed && requesting_source != ControlSource::SBUS &&
               requesting_source != ControlSource::WEB) {
        auth.allowed = false;
        auth.reason = RejectReason::LOWER_PRIORITY_CONFLICT;
    }

    StateEvent event{};
    event.kind = EventKind::FIRE_DECISION;
    event.reject_reason = auth.reason;
    event.from_source = requesting_source;
    event.to_source = auth.allowed ? requesting_source : auth.authorizing_source;
    PushEvent(event);
    return auth;
}

void StateArbitrator::ClearEvents() noexcept
{
    events_.fill(StateEvent{});
    event_count_ = 0;
    dropped_event_count_ = 0;
    head_ = 0;
}

std::size_t StateArbitrator::CopyEvents(StateEvent* out, std::size_t capacity) const noexcept
{
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    const std::size_t count = (event_count_ < capacity) ? event_count_ : capacity;
    const std::size_t start = (head_ + kMaxEvents - event_count_) % kMaxEvents;
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = events_[(start + i) % kMaxEvents];
    }
    return count;
}

const StateEvent* StateArbitrator::latest_event() const noexcept
{
    if (event_count_ == 0) {
        return nullptr;
    }
    return &events_[(head_ + kMaxEvents - 1u) % kMaxEvents];
}

} // namespace state_arbitration
} // namespace marshland
