#include "atom10_reload_feed/reload_feed_core.hpp"

namespace marshland::atom10 {
namespace {

constexpr std::int64_t kMicrosPerSecond = 1000000;
constexpr std::int64_t kMicrosPerMillisecond = 1000;

} // namespace

ReloadFeedCore::ReloadFeedCore(Dependencies deps, const ReloadFeedConfig& config) noexcept
    : deps_(deps)
    , config_(config)
{
}

void ReloadFeedCore::bindControlAuthority(app::IControlAuthority& authority) noexcept
{
    authority_ = &authority;
}

void ReloadFeedCore::bindFaultReporter(app::IInputReporter* reporter) noexcept
{
    reporter_ = reporter;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool ReloadFeedCore::initializeOutputs() noexcept
{
    cancelActiveAction();
    const bool left_ok = deps_.motors.submitVelocityRpm(ReloadSide::Left, 0);
    const bool right_ok = deps_.motors.submitVelocityRpm(ReloadSide::Right, 0);
    if (!left_ok || !right_ok) {
        zeroTorqueBoth();
        reload_state_ = ReloadState::Fault;
        return false;
    }
    outputs_active_ = true;
    reload_state_ = ReloadState::Lifted;  // 逻辑默认抬起，不主动执行抬起
    return true;
}

void ReloadFeedCore::shutdown() noexcept
{
    cancelActiveAction();
    zeroTorqueBoth();          // 内部按 outputs_active_ 判定，幂等、对未初始化安全
    outputs_active_ = false;
    reload_state_ = ReloadState::Stopped;
}

void ReloadFeedCore::updateConfig(const ReloadFeedConfig& config) noexcept
{
    config_ = config;
}

// ---------------------------------------------------------------------------
// 换弹命令
// ---------------------------------------------------------------------------

ReloadCommandResult ReloadFeedCore::requestLift(ControlSource source,
                                                state_arbitration::IntentKind kind,
                                                std::uint32_t request_id) noexcept
{
    return requestReloadAction(ReloadAction::Lift, source, kind, request_id);
}

ReloadCommandResult ReloadFeedCore::requestLower(ControlSource source,
                                                 std::uint32_t request_id) noexcept
{
    return requestReloadAction(ReloadAction::Lower, source,
                               state_arbitration::IntentKind::CONTROL, request_id);
}

ReloadCommandResult ReloadFeedCore::requestReloadAction(
    ReloadAction action, ControlSource source,
    state_arbitration::IntentKind kind, std::uint32_t request_id) noexcept
{
    // 自身拒绝：急停锁定阶段禁止普通出力命令；SAFETY_RECOVERY 交仲裁裁决。
    if (estop_locked_ && kind == state_arbitration::IntentKind::CONTROL)
        return {false, ReloadReject::EstopLocked};
    // 机构故障态拒绝普通命令；恢复抬起（SAFETY_RECOVERY）允许尝试。
    if (reload_state_ == ReloadState::Fault && kind == state_arbitration::IntentKind::CONTROL)
        return {false, ReloadReject::FaultLatched};
    if (active_action_ != ReloadAction::None)
        return {false, ReloadReject::Busy};
    if (!deps_.motors.isSideUsable(ReloadSide::Left) ||
        !deps_.motors.isSideUsable(ReloadSide::Right))
        return {false, ReloadReject::MotorUnavailable};

    const state_arbitration::IntentDecision decision = submitIntent(source, kind, request_id);
    if (!decision.accepted)
        return {false, ReloadReject::IntentRejected};

    // 方向约定：抬起左正右负，放下左负右正（ConfigSchema.md v1.9 §2.7.1）
    const int speed = config_.speed_rpm;
    const int left_target = (action == ReloadAction::Lift) ? speed : -speed;
    const int right_target = (action == ReloadAction::Lift) ? -speed : speed;

    // 两台命令分别提交，不是事务；任一失败联动两侧零力矩。
    if (!deps_.motors.submitVelocityRpm(ReloadSide::Left, left_target))
        return failActionStart(action);
    if (!deps_.motors.submitVelocityRpm(ReloadSide::Right, right_target))
        return failActionStart(action);

    // 两台目标均成功提交后才开始计时，不以反馈到目标为计时起点。
    active_action_ = action;
    ++action_generation_;
    const int run_s = (action == ReloadAction::Lift) ? config_.lift_run_s
                                                     : config_.lower_run_s;
    run_deadline_micros_ = deps_.clock.nowMicros() + run_s * kMicrosPerSecond;
    reload_state_ = ReloadState::Running;
    return {true, ReloadReject::None};
}

ReloadCommandResult ReloadFeedCore::failActionStart(ReloadAction action) noexcept
{
    zeroTorqueBoth();
    reload_state_ = ReloadState::Fault;
    last_failed_action_ = action;
    reportFault();  // F-RELOAD-001：运行目标任一提交失败
    return {false, ReloadReject::CommandFailed};
}

// ---------------------------------------------------------------------------
// 供弹命令
// ---------------------------------------------------------------------------

FeedCommandResult ReloadFeedCore::requestFeedOnce(ControlSource source,
                                                  std::uint32_t request_id) noexcept
{
    if (estop_locked_)
        return {false, FeedReject::EstopLocked};
    if (feed_state_ == FeedState::Holding)
        return {false, FeedReject::Busy};

    const state_arbitration::IntentDecision decision =
        submitIntent(source, state_arbitration::IntentKind::CONTROL, request_id);
    if (!decision.accepted)
        return {false, FeedReject::IntentRejected};

    if (!deps_.feed.moveToReloadPosition()) {
        feed_state_ = FeedState::Fault;   // 写入失败不得报告供弹完成
        last_cycle_failed_ = true;
        return {false, FeedReject::CommandFailed};
    }

    const std::int64_t now = deps_.clock.nowMicros();
    feed_trigger_micros_ = now;  // 触发时刻：ATOM-13/PEFExecutor 的 2 s 等待自此起算
    feed_hold_deadline_micros_ = now + config_.feed_hold_ms * kMicrosPerMillisecond;
    feed_state_ = FeedState::Holding;
    last_cycle_failed_ = false;
    return {true, FeedReject::None};
}

// ---------------------------------------------------------------------------
// 停止与安全
// ---------------------------------------------------------------------------

void ReloadFeedCore::requestManualStop(ControlSource source) noexcept
{
    // 零力矩属不出力安全动作，按类别提交 SAFETY；但其结果为记录性质，
    // 零力矩命令本身无条件发出（fail-safe）。
    static_cast<void>(submitIntent(source, state_arbitration::IntentKind::SAFETY, 0));
    cancelActiveAction();
    zeroTorqueBoth();
    reload_state_ = ReloadState::Stopped;
}

void ReloadFeedCore::notifySoftwareEstopLocked() noexcept
{
    estop_locked_ = true;
    cancelActiveAction();
    zeroTorqueBoth();
    reload_state_ = ReloadState::Stopped;
    if (feed_state_ == FeedState::Holding) {
        // 中止本次供弹；急停锁定期间不再向执行器发送任何位置命令。
        feed_state_ = FeedState::Standby;
        last_cycle_failed_ = true;
    }
}

void ReloadFeedCore::notifySoftwareEstopReleased() noexcept
{
    estop_locked_ = false;
}

void ReloadFeedCore::notifySideFault(ReloadSide /*side*/) noexcept
{
    const ReloadAction failed = active_action_;
    cancelActiveAction();  // 取消动作与计时，拒绝迟到完成
    zeroTorqueBoth();      // 单侧故障联动两侧零力矩
    reload_state_ = ReloadState::Fault;
    if (failed != ReloadAction::None)
        last_failed_action_ = failed;
    reportFault();
}

// ---------------------------------------------------------------------------
// 周期推进
// ---------------------------------------------------------------------------

void ReloadFeedCore::poll() noexcept
{
    const std::int64_t now = deps_.clock.nowMicros();

    // 换弹：运行计时到期 -> 提交两侧 0 rpm -> 均成功且未取消才判定到位。
    if (active_action_ != ReloadAction::None && now >= run_deadline_micros_) {
        const ReloadAction finished = active_action_;
        const std::uint32_t generation = action_generation_;
        bool ok = deps_.motors.submitVelocityRpm(ReloadSide::Left, 0);
        ok = deps_.motors.submitVelocityRpm(ReloadSide::Right, 0) && ok;
        if (!ok) {
            cancelActiveAction();
            zeroTorqueBoth();
            reload_state_ = ReloadState::Fault;
            last_failed_action_ = finished;
            reportFault();  // F-RELOAD-001：结束 0 rpm 目标任一提交失败
        } else if (generation == action_generation_) {
            active_action_ = ReloadAction::None;
            reload_state_ = (finished == ReloadAction::Lift) ? ReloadState::Lifted
                                                             : ReloadState::Lowered;
            ++completed_actions_;  // 到位后 0 rpm 目标保持于 ATOM-05，维持速度闭环
        }
    }

    // 供弹：最大动作时间优先于保持到期检查。
    if (feed_state_ == FeedState::Holding) {
        if (now - feed_trigger_micros_ >
            config_.feed_max_action_ms * kMicrosPerMillisecond) {
            feed_state_ = FeedState::Fault;
            last_cycle_failed_ = true;
            reportFault();  // 供弹动作超时（timing.feed_max_action_ms）
        } else if (now >= feed_hold_deadline_micros_) {
            if (deps_.feed.moveToStandbyPosition()) {
                feed_state_ = FeedState::Standby;
                ++completed_cycles_;  // 回到待机位目标后才标记本次供弹完成
                last_cycle_failed_ = false;
            } else {
                feed_state_ = FeedState::Fault;  // 写入失败不得报告供弹完成
                last_cycle_failed_ = true;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 状态查询
// ---------------------------------------------------------------------------

ReloadStatus ReloadFeedCore::reloadStatus() const noexcept
{
    ReloadStatus status;
    status.state = reload_state_;
    status.active_action = active_action_;
    status.completed_actions = completed_actions_;
    status.last_failed_action = last_failed_action_;
    return status;
}

FeedStatus ReloadFeedCore::feedStatus() const noexcept
{
    FeedStatus status;
    status.state = feed_state_;
    status.completed_cycles = completed_cycles_;
    status.last_cycle_failed = last_cycle_failed_;
    status.last_trigger_micros = feed_trigger_micros_;
    return status;
}

// ---------------------------------------------------------------------------
// 内部辅助
// ---------------------------------------------------------------------------

state_arbitration::IntentDecision ReloadFeedCore::submitIntent(
    ControlSource source, state_arbitration::IntentKind kind, std::uint32_t request_id) noexcept
{
    state_arbitration::IntentDecision decision{};
    decision.accepted = false;
    decision.source = source;
    decision.kind = kind;
    if (authority_ == nullptr) {
        // 控制权尚未绑定（AppCoordinator 未就绪）：fail-safe 拒绝
        decision.reason = state_arbitration::RejectReason::NOT_READY;
        return decision;
    }
    state_arbitration::ControlIntent intent{};
    intent.valid = true;
    intent.source = source;
    intent.kind = kind;
    intent.request_id = request_id;
    intent.timestamp_us = static_cast<std::uint64_t>(deps_.clock.nowMicros());
    return authority_->SubmitIntent(intent);
}

void ReloadFeedCore::cancelActiveAction() noexcept
{
    if (active_action_ != ReloadAction::None) {
        active_action_ = ReloadAction::None;
        ++action_generation_;  // 使进行中的计时结果失效，拒绝迟到完成
    }
}

void ReloadFeedCore::zeroTorqueBoth() noexcept
{
    if (!outputs_active_)
        return;
    static_cast<void>(deps_.motors.requestZeroTorque(ReloadSide::Left));
    static_cast<void>(deps_.motors.requestZeroTorque(ReloadSide::Right));
}

void ReloadFeedCore::reportFault() noexcept
{
    if (reporter_ != nullptr)
        reporter_->ReportFault(true, FaultSeverity::SEVERE);
}

} // namespace marshland::atom10
