// ============================================================================
// SafetyRecovery.cpp — ATOM-14 安全、故障与恢复管理实现
//
// 实现范围：故障管理（Config §14.2 目录 + 1000 条环形历史 + 事件发布）、
// 普通命令安全闸门、软件急停锁定/确认/解除编排、一般故障自动恢复编排、
// 分阶段 CAN 力矩门控调度。编排契约见本目录 Guide.md §3~§6。
// ============================================================================
#include "SafetyRecovery.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "SafetyRecoveryFaultCatalog.hpp"

namespace marshland::safety {
namespace {

void copyText(char* dst, std::size_t capacity, const char* src) noexcept
{
    if (dst == nullptr || capacity == 0U) {
        return;
    }
    std::size_t i = 0;
    if (src != nullptr) {
        while (i + 1U < capacity && src[i] != '\0') {
            dst[i] = src[i];
            ++i;
        }
    }
    dst[i] = '\0';
}

// UTC 展示时间戳（ISO 8601）。仅用于故障事件展示字段；内部等待/超时/周期
// 一律使用注入的单调时钟，不得由本函数推算计时基准。
void formatUtcNow(char* out, std::size_t capacity) noexcept
{
    if (out == nullptr || capacity == 0U) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    std::tm tm_utc{};
    if (gmtime_r(&now, &tm_utc) == nullptr) {
        copyText(out, capacity, "unknown");
        return;
    }
    std::snprintf(out, capacity, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
}

bool roleEquals(const char* role, std::uint64_t role_size, const char* expected) noexcept
{
    if (role == nullptr || expected == nullptr) {
        return false;
    }
    std::uint64_t i = 0;
    while (i < role_size && expected[i] != '\0' && role[i] == expected[i]) {
        ++i;
    }
    return i == role_size && expected[i] == '\0';
}

} // namespace

SafetyRecovery::SafetyRecovery(Ports ports) noexcept
    : ports_(ports)
{
}

bool SafetyRecovery::portsReady() const noexcept
{
    return ports_.safety_state != nullptr &&
           ports_.control_authority != nullptr &&
           ports_.input_reporter != nullptr &&
           ports_.state_reader != nullptr &&
           ports_.clock != nullptr &&
           ports_.config != nullptr;
}

// ---------------------------------------------------------------------------
// 周期推进
// ---------------------------------------------------------------------------
void SafetyRecovery::update() noexcept
{
    if (!portsReady()) {
        return;
    }
    tryLoadConfig();
    drainCanEvents();

    // 借读权威状态：视图释放后才允许调用其他端口（根 PLAN §5.3 约束）。
    SystemMode mode = SystemMode::UNINITIALIZED;
    bool latched = false;
    bool estop_input = false;
    {
        app::StateReadView view = ports_.state_reader->ReadState();
        mode = view.context().system_mode;
        latched = view.estop_latched();
        estop_input = view.inputs().software_estop;
    }
    estop_latched_ = latched;

    if (latched && estop_input) {
        if (phase_ == SafetyPhase::ESTOP_RECOVERY ||
            phase_ == SafetyPhase::GENERAL_RECOVERY) {
            // 再次急停：确认后模式为 MANUAL，重新触发使模式回到 SOFTWARE_ESTOP
            if (mode == SystemMode::SOFTWARE_ESTOP) {
                enterLockdown();
            }
        } else if (phase_ != SafetyPhase::ESTOP_LOCKED &&
                   phase_ != SafetyPhase::FAULT_LOCKED) {
            enterLockdown();
        }
    }

    switch (phase_) {
    case SafetyPhase::ESTOP_RECOVERY:
        advanceEstopRecovery();
        break;
    case SafetyPhase::GENERAL_RECOVERY:
        advanceGeneralRecovery();
        break;
    default:
        break;
    }

    maybeStartGeneralRecovery();
}

// ---------------------------------------------------------------------------
// 软件急停：锁定 -> 人工确认 -> 解除动作 -> 解除锁存（两步，不可合并）
// ---------------------------------------------------------------------------
void SafetyRecovery::enterLockdown() noexcept
{
    // 撤销全部普通及未完成恢复动作的出力目标，全部电机持续零力矩；
    // 换弹不主动抬起或放下。
    if (ports_.recovery_actions != nullptr) {
        ports_.recovery_actions->cancelAll();
    }
    if (ports_.torque_gate != nullptr) {
        ports_.torque_gate->setTorqueOutputMode(can::TorqueOutputMode::ZERO_TORQUE);
    }
    phase_ = SafetyPhase::ESTOP_LOCKED;
    recovery_stage_ = RecoveryStage::NONE;
    registerFault("E-ESTOP-001", "ATOM-14");
}

void SafetyRecovery::onEstopConfirmRequest() noexcept
{
    if (!portsReady() || phase_ != SafetyPhase::ESTOP_LOCKED) {
        return;
    }
    // 第一步：人工确认。仅 SOFTWARE_ESTOP -> MANUAL，急停变量与锁存保持 true。
    const state_arbitration::ModeTransitionResult result =
        ports_.safety_state->ConfirmSoftwareEstop();
    if (!result.accepted) {
        return;
    }
    // 出力型解除动作统一走 SAFETY_RECOVERY；锁定阶段提交会被仲裁拒绝。
    if (!submitSafetyRecoveryIntent()) {
        return;
    }
    // 确认后的安全恢复阶段：获准机构按恢复目标出力，其余电机零力矩；
    // 门控切 CONTROL 后不再发送全零帧，避免覆盖恢复出力。
    if (ports_.torque_gate != nullptr) {
        ports_.torque_gate->setTorqueOutputMode(can::TorqueOutputMode::CONTROL);
    }
    if (ports_.recovery_actions == nullptr) {
        enterFaultLock();
        return;
    }
    // 解除动作：双 M3508 换弹抬起 + 蓄力双电机回零（并行允许）。
    if (ports_.recovery_actions->requestReloadLift() != ActionResult::ACCEPTED) {
        // 人工确认急停恢复中换弹抬起失败：F-RELOAD-002，FAULT 锁定，保留锁存
        registerFault("F-RELOAD-002", "ATOM-14");
        enterFaultLock();
        return;
    }
    ports_.recovery_actions->requestChargeHome();
    phase_ = SafetyPhase::ESTOP_RECOVERY;
    recovery_stage_ = RecoveryStage::NONE;
}

void SafetyRecovery::advanceEstopRecovery() noexcept
{
    // 换弹联动：任一侧离线/反馈异常，两侧零力矩并取消动作，回到锁定阶段
    if (!reloadMotorsOnline()) {
        if (ports_.recovery_actions != nullptr) {
            ports_.recovery_actions->cancelAll();
        }
        if (ports_.torque_gate != nullptr) {
            ports_.torque_gate->setTorqueOutputMode(can::TorqueOutputMode::ZERO_TORQUE);
        }
        registerFault("F-MOTOR-001", "ATOM-14");
        phase_ = SafetyPhase::ESTOP_LOCKED;
        recovery_stage_ = RecoveryStage::NONE;
        return;
    }

    const ActionStatus lift = ports_.recovery_actions->actionStatus(RecoveryAction::RELOAD_LIFT);
    if (lift == ActionStatus::FAILED) {
        // 恢复抬起再次失败：FAULT 锁定，保留尚未解除的急停锁存，不自动重试
        registerFault("F-RELOAD-002", "ATOM-14");
        enterFaultLock();
        return;
    }
    // 解除判据：换弹抬起到位，且两个蓄力限位均稳定低电平
    if (lift == ActionStatus::SUCCEEDED && chargeHomeLimitsLow()) {
        // 第二步：解除生效。仅此时急停变量才置 False。
        if (ports_.safety_state->ReleaseSoftwareEstopLatch()) {
            clearFault("E-ESTOP-001");
            phase_ = SafetyPhase::NORMAL;
            recovery_stage_ = RecoveryStage::NONE;
            // 不重新初始化、不续跑原自动发射流程
        }
    }
}

void SafetyRecovery::enterFaultLock() noexcept
{
    if (ports_.recovery_actions != nullptr) {
        ports_.recovery_actions->cancelAll();
    }
    if (ports_.torque_gate != nullptr) {
        ports_.torque_gate->setTorqueOutputMode(can::TorqueOutputMode::ZERO_TORQUE);
    }
    phase_ = SafetyPhase::FAULT_LOCKED;
    recovery_stage_ = RecoveryStage::FAILED;
    ports_.control_authority->RequestModeTransition(
        SystemMode::FAULT, state_arbitration::TransitionReason::FAULT_DETECTED);
}

// ---------------------------------------------------------------------------
// 一般故障自动恢复：回零 -> 三次发射 -> 换弹抬起 -> MANUAL（顺序不可调换）
// ---------------------------------------------------------------------------
void SafetyRecovery::requestGeneralRecovery() noexcept
{
    recovery_pending_ = true;
}

void SafetyRecovery::maybeStartGeneralRecovery() noexcept
{
    if (!recovery_pending_ || phase_ != SafetyPhase::NORMAL || estop_latched_) {
        return;  // 急停锁定期间不推进一般故障恢复
    }
    if (hasBlockingActiveFault()) {
        return;  // 根因未清除
    }
    // 关联接口恢复有效：机构电机反馈在线（换弹相关故障要求左右两侧均有效）
    if (ports_.motor_feedback == nullptr || ports_.recovery_actions == nullptr ||
        !ports_.motor_feedback->motorOnline(motor_charge_right_) ||
        !ports_.motor_feedback->motorOnline(motor_charge_left_) ||
        !reloadMotorsOnline()) {
        return;
    }
    if (!submitSafetyRecoveryIntent()) {
        return;
    }
    if (ports_.recovery_actions->requestChargeHome() != ActionResult::ACCEPTED) {
        return;
    }
    if (ports_.torque_gate != nullptr) {
        ports_.torque_gate->setTorqueOutputMode(can::TorqueOutputMode::CONTROL);
    }
    recovery_pending_ = false;
    phase_ = SafetyPhase::GENERAL_RECOVERY;
    recovery_stage_ = RecoveryStage::CHARGE_HOME;
    recovery_fires_remaining_ = 0;
}

void SafetyRecovery::advanceGeneralRecovery() noexcept
{
    switch (recovery_stage_) {
    case RecoveryStage::CHARGE_HOME:
        // 完成判据：两个限位开关均为低电平（不允许单限位低或超时判完成）
        if (ports_.recovery_actions->actionStatus(RecoveryAction::CHARGE_HOME) ==
            ActionStatus::FAILED) {
            abortGeneralRecovery();
            return;
        }
        if (chargeHomeLimitsLow()) {
            recovery_fires_remaining_ = recovery_fire_total_;
            if (ports_.recovery_actions->requestRecoveryFire() != ActionResult::ACCEPTED) {
                abortGeneralRecovery();
                return;
            }
            recovery_stage_ = RecoveryStage::RECOVERY_FIRES;
        }
        break;

    case RecoveryStage::RECOVERY_FIRES: {
        // 仅发射机构动作；其他机构电机零力矩、其他舵机角度保持不变（由机构
        // 模块与门控共同保证，本模块不发起任何其他动作请求）
        const ActionStatus fire =
            ports_.recovery_actions->actionStatus(RecoveryAction::RECOVERY_FIRE);
        if (fire == ActionStatus::FAILED) {
            abortGeneralRecovery();
            return;
        }
        if (fire == ActionStatus::SUCCEEDED) {
            if (recovery_fires_remaining_ > 0U) {
                --recovery_fires_remaining_;
            }
            if (recovery_fires_remaining_ == 0U) {
                if (ports_.recovery_actions->requestReloadLift() != ActionResult::ACCEPTED) {
                    registerFault("F-RELOAD-001", "ATOM-14");
                    abortGeneralRecovery();
                    return;
                }
                recovery_stage_ = RecoveryStage::RELOAD_LIFT;
            } else if (ports_.recovery_actions->requestRecoveryFire() !=
                       ActionResult::ACCEPTED) {
                abortGeneralRecovery();
                return;
            }
        }
        break;
    }

    case RecoveryStage::RELOAD_LIFT: {
        // 换弹联动：任一侧离线/反馈异常，两侧零力矩并取消动作与计时；
        // 普通换弹故障根因清除后可沿用一般恢复（abandon 由故障登记重新挂起）
        if (!reloadMotorsOnline()) {
            ports_.recovery_actions->cancelAll();
            registerFault("F-MOTOR-001", "ATOM-14");
            abortGeneralRecovery();
            return;
        }
        const ActionStatus lift =
            ports_.recovery_actions->actionStatus(RecoveryAction::RELOAD_LIFT);
        if (lift == ActionStatus::FAILED) {
            // 恢复抬起再次失败：FAULT 锁定，不自动循环重试
            registerFault("F-RELOAD-002", "ATOM-14");
            enterFaultLock();
            return;
        }
        if (lift == ActionStatus::SUCCEEDED) {
            recovery_stage_ = RecoveryStage::COMPLETED;
            ports_.control_authority->RequestModeTransition(
                SystemMode::MANUAL, state_arbitration::TransitionReason::RECOVERY_COMPLETE);
            phase_ = SafetyPhase::NORMAL;
            recovery_stage_ = RecoveryStage::NONE;
            // 完成后进入 MANUAL，不恢复旧自动发射上下文
        }
        break;
    }

    default:
        break;
    }
}

void SafetyRecovery::abortGeneralRecovery() noexcept
{
    if (ports_.recovery_actions != nullptr) {
        ports_.recovery_actions->cancelAll();
    }
    if (ports_.torque_gate != nullptr) {
        ports_.torque_gate->setTorqueOutputMode(can::TorqueOutputMode::ZERO_TORQUE);
    }
    phase_ = SafetyPhase::NORMAL;
    recovery_stage_ = RecoveryStage::NONE;
    // 失败根因由归属模块登记故障；带 pef_recovery 的登记会重新挂起恢复意图，
    // 本函数不自行重试，避免无根因清除的循环。
    recovery_pending_ = false;
}

// ---------------------------------------------------------------------------
// 安全闸门
// ---------------------------------------------------------------------------
bool SafetyRecovery::isOrdinaryCommandAllowed() const noexcept
{
    return phase_ == SafetyPhase::NORMAL && !estop_latched_;
}

// ---------------------------------------------------------------------------
// 故障管理
// ---------------------------------------------------------------------------
bool SafetyRecovery::registerFault(const char* code, const char* source) noexcept
{
    const FaultCatalogEntry* entry = findFaultCatalogEntry(code);
    if (entry == nullptr || source == nullptr) {
        return false;  // 故障码必须命中 Config §14.2 目录，禁止自造
    }

    FaultRecord snapshot{};
    {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        FaultRecord* record = nullptr;
        for (std::size_t i = 0; i < active_fault_count_; ++i) {
            if (std::strncmp(active_faults_[i].code, code,
                             sizeof(active_faults_[i].code)) == 0) {
                record = &active_faults_[i];
                break;
            }
        }
        if (record == nullptr) {
            if (active_fault_count_ >= kMaxActiveFaults) {
                return false;
            }
            record = &active_faults_[active_fault_count_++];
            *record = FaultRecord{};
            copyText(record->code, sizeof(record->code), code);
            record->severity = entry->severity;
        }
        copyText(record->source, sizeof(record->source), source);
        formatUtcNow(record->utc_text, sizeof(record->utc_text));
        record->monotonic_us = ports_.clock != nullptr ? ports_.clock->nowMicros() : 0;
        record->active = true;
        record->recovery_in_progress = false;
        record->acknowledged = false;

        history_[history_head_] = *record;
        history_head_ = (history_head_ + 1U) % kFaultHistoryCapacity;
        if (history_count_ < kFaultHistoryCapacity) {
            ++history_count_;
        }
        snapshot = *record;
    }

    if (ports_.fault_events != nullptr) {
        ports_.fault_events->onFaultEvent(snapshot);
    }
    if (entry->pef_recovery) {
        recovery_pending_ = true;
    }
    reportFaultRollup();
    return true;
}

bool SafetyRecovery::clearFault(const char* code) noexcept
{
    if (code == nullptr) {
        return false;
    }
    FaultRecord snapshot{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        for (std::size_t i = 0; i < active_fault_count_; ++i) {
            if (std::strncmp(active_faults_[i].code, code,
                             sizeof(active_faults_[i].code)) != 0) {
                continue;
            }
            active_faults_[i].active = false;
            formatUtcNow(active_faults_[i].utc_text, sizeof(active_faults_[i].utc_text));
            active_faults_[i].monotonic_us =
                ports_.clock != nullptr ? ports_.clock->nowMicros() : 0;
            snapshot = active_faults_[i];
            history_[history_head_] = snapshot;
            history_head_ = (history_head_ + 1U) % kFaultHistoryCapacity;
            if (history_count_ < kFaultHistoryCapacity) {
                ++history_count_;
            }
            active_faults_[i] = active_faults_[active_fault_count_ - 1U];
            --active_fault_count_;
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    if (ports_.fault_events != nullptr) {
        ports_.fault_events->onFaultEvent(snapshot);
    }
    reportFaultRollup();
    return true;
}

bool SafetyRecovery::acknowledgeFault(const char* code) noexcept
{
    if (code == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(fault_mutex_);
    for (std::size_t i = 0; i < active_fault_count_; ++i) {
        if (std::strncmp(active_faults_[i].code, code,
                         sizeof(active_faults_[i].code)) == 0) {
            active_faults_[i].acknowledged = true;
            return true;
        }
    }
    return false;
}

std::size_t SafetyRecovery::copyActiveFaults(FaultRecord* output,
                                             std::size_t capacity) const noexcept
{
    if (output == nullptr || capacity == 0U) {
        return 0U;
    }
    std::lock_guard<std::mutex> lock(fault_mutex_);
    const std::size_t copied =
        active_fault_count_ < capacity ? active_fault_count_ : capacity;
    for (std::size_t i = 0; i < copied; ++i) {
        output[i] = active_faults_[i];
    }
    return copied;
}

std::size_t SafetyRecovery::copyFaultHistory(FaultRecord* output,
                                             std::size_t capacity) const noexcept
{
    if (output == nullptr || capacity == 0U) {
        return 0U;
    }
    std::lock_guard<std::mutex> lock(fault_mutex_);
    const std::size_t copied = history_count_ < capacity ? history_count_ : capacity;
    for (std::size_t i = 0; i < copied; ++i) {
        // 最新优先：output[0] 为最近一次写入
        const std::size_t index =
            (history_head_ + kFaultHistoryCapacity - 1U - i) % kFaultHistoryCapacity;
        output[i] = history_[index];
    }
    return copied;
}

std::size_t SafetyRecovery::activeFaultCount() const noexcept
{
    std::lock_guard<std::mutex> lock(fault_mutex_);
    return active_fault_count_;
}

void SafetyRecovery::reportFaultRollup() noexcept
{
    if (ports_.input_reporter == nullptr) {
        return;
    }
    bool any = false;
    FaultSeverity max_severity = FaultSeverity::INFO;
    {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        for (std::size_t i = 0; i < active_fault_count_; ++i) {
            any = true;
            if (static_cast<std::uint8_t>(active_faults_[i].severity) >
                static_cast<std::uint8_t>(max_severity)) {
                max_severity = active_faults_[i].severity;
            }
        }
    }
    // 只上报活动故障的最高等级；是否全局阻断由仲裁 IsFaultBlocking 判定
    ports_.input_reporter->ReportFault(any, max_severity);
}

bool SafetyRecovery::hasBlockingActiveFault() const noexcept
{
    std::lock_guard<std::mutex> lock(fault_mutex_);
    for (std::size_t i = 0; i < active_fault_count_; ++i) {
        if (static_cast<std::uint8_t>(active_faults_[i].severity) >=
            static_cast<std::uint8_t>(FaultSeverity::SEVERE)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// CAN 事件入口（CAN 线程，仅置标志，update() 消费）
// ---------------------------------------------------------------------------
void SafetyRecovery::onZeroTorqueSendFailed(const can::CanIoResult& /*result*/) noexcept
{
    pending_zero_torque_failed_.store(true);
}

void SafetyRecovery::onCanEvent(const can::CanEvent& event) noexcept
{
    switch (event.kind) {
    case can::CanEventKind::BUS_ERROR:
        pending_can_bus_error_.store(true);
        break;
    case can::CanEventKind::ZERO_TORQUE_FAILED:
        pending_zero_torque_failed_.store(true);
        break;
    case can::CanEventKind::NODE_STATE_CHANGE:
        // 当前 CanEvent 不携带目标状态，无法区分离线/恢复；按离线/异常登记，
        // 状态恢复由 motor_feedback 在线查询在恢复触发判据中确认。
        if (event.motor_id >= 1 && event.motor_id <= 6) {
            pending_motor_offline_mask_.fetch_or(1U << (event.motor_id - 1));
        }
        break;
    }
}

void SafetyRecovery::drainCanEvents() noexcept
{
    if (pending_zero_torque_failed_.exchange(false)) {
        // 零力矩帧发送失败：进入错误状态并全机静默，不静默忽略
        registerFault("F-CAN-002", "ATOM-04");
        if (ports_.recovery_actions != nullptr) {
            ports_.recovery_actions->cancelAll();
        }
        if (ports_.torque_gate != nullptr) {
            ports_.torque_gate->setTorqueOutputMode(can::TorqueOutputMode::ZERO_TORQUE);
        }
    }
    if (pending_can_bus_error_.exchange(false)) {
        registerFault("F-CAN-001", "ATOM-04");
    }
    if (pending_motor_offline_mask_.exchange(0U) != 0U) {
        registerFault("F-MOTOR-001", "ATOM-04");
    }
}

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------
bool SafetyRecovery::submitSafetyRecoveryIntent() noexcept
{
    state_arbitration::ControlIntent intent{};
    intent.valid = true;
    intent.kind = state_arbitration::IntentKind::SAFETY_RECOVERY;
    intent.source = ControlSource::AUTO;  // 安全动作不参与优先级排序
    intent.request_id = ++request_id_;
    intent.timestamp_us = ports_.clock != nullptr ? ports_.clock->nowMicros() : 0;
    return ports_.control_authority->SubmitIntent(intent).accepted;
}

bool SafetyRecovery::reloadMotorsOnline() const noexcept
{
    // 未绑定反馈端口时按离线处理（fail-safe），恢复出力不会放行
    return ports_.motor_feedback != nullptr &&
           ports_.motor_feedback->motorOnline(motor_reload_right_) &&
           ports_.motor_feedback->motorOnline(motor_reload_left_);
}

bool SafetyRecovery::chargeHomeLimitsLow() const noexcept
{
    return ports_.limit_switches != nullptr &&
           ports_.limit_switches->chargeLimit1Low() &&
           ports_.limit_switches->chargeLimit2Low();
}

void SafetyRecovery::tryLoadConfig() noexcept
{
    if (config_loaded_ || ports_.config == nullptr || !ports_.config->isLoaded()) {
        return;
    }

    std::int64_t value = 0;
    if (ports_.config->getInt("mode.initial.recovery_fire_count", value) ==
            config::ConfigStatus::OK &&
        value > 0) {
        recovery_fire_total_ = static_cast<std::uint32_t>(value);
    }

    // 电机映射：按 dart_control.json motor[].role -> motor[].id 读取，
    // 不写死节点 ID；读取失败保留与当前配置一致的默认值
    std::uint64_t count = 0;
    if (ports_.config->getArraySize("motor", count) == config::ConfigStatus::OK) {
        for (std::uint64_t i = 0; i < count; ++i) {
            const char* role = nullptr;
            std::uint64_t role_size = 0;
            std::int64_t id = 0;
            if (ports_.config->getStringAt("motor", i, "role", role, role_size) !=
                    config::ConfigStatus::OK ||
                ports_.config->getIntAt("motor", i, "id", id) != config::ConfigStatus::OK) {
                continue;
            }
            const int motor_id = static_cast<int>(id);
            if (roleEquals(role, role_size, "charge_right")) {
                motor_charge_right_ = motor_id;
            } else if (roleEquals(role, role_size, "charge_left")) {
                motor_charge_left_ = motor_id;
            } else if (roleEquals(role, role_size, "reload_right")) {
                motor_reload_right_ = motor_id;
            } else if (roleEquals(role, role_size, "reload_left")) {
                motor_reload_left_ = motor_id;
            }
        }
    }
    config_loaded_ = true;
}

} // namespace marshland::safety
