// ============================================================================
// AppCoordinator.cpp — 顶层装配与桥接层实现
//
// 本文件是**唯一**同时包含 RuntimeCoordinator（底层）与 StateArbitrator（上层）
// 的地方。整个工程中 "启动结果 -> 业务模式" 与 "外部输入 -> 状态机" 的转换
// 都收敛在这里，任何其他地方出现同类映射都属于设计漂移。
//
// 实现依据：
//   Design/Interface.md  §4.3 启动顺序、§6.2 系统模式、§7.1 状态转换总表、
//                        §7.2 状态机约束、§3.3 单调时钟、§17.3 安全动作例外
//   Design/Config.md     §2 运行基线（停止发射标志初值 True）、§3.1 功能模式、
//                        §3.2 初始状态、§7.2 控制源选择、§14.2 故障等级、§15 并发
//   Design/Atomic/ATOM-01_Runtime.md
//   Design/Atomic/ATOM-02_State_Arbitration.md
// ============================================================================
#include "AppCoordinator.hpp"

namespace marshland::app {

namespace {

// Interface §7.1 + ATOM-02 的 IsOperationalMode() 语义。
//
// 刻意**不**包含 RECOVERY 与 SOFTWARE_ESTOP：恢复期与急停期的安全动作走
// IntentKind::SAFETY 豁免路径，而不是放宽本闸门（ATOM02-BUG-007）。
//
// 已知漂移风险：ATOM-02 中的同名函数位于其 .cpp 的匿名命名空间内，无法导出
// 复用，故此处保留一份等价副本。若 ATOM-02 调整该模式集合，本副本必须同步
// （见 Atoms/ATOM-02_State_Arbitration/BugLists.md）。
bool IsOperationalMode(SystemMode mode) noexcept
{
    return mode == SystemMode::STANDBY || mode == SystemMode::MANUAL ||
           mode == SystemMode::AUTO_FIRE || mode == SystemMode::DEBUG ||
           mode == SystemMode::CALIBRATION;
}

} // namespace

AppCoordinator::AppCoordinator(runtime::RuntimeCoordinator& runtime,
                               state_arbitration::StateArbitrator& authority,
                               IMonotonicClock& clock) noexcept
    : runtime_(runtime)
    , authority_(authority)
    , clock_(clock)
{
    // 不再需要在本层预置停止发射标志：ATOM-02 的 ExternalInputs 已按
    // Design/Config.md §2 将 stop_fire 初值改为 true（ATOM02-BUG-005 修复）。
    // 本层不持有输入副本，因此也没有副本漂移问题。
}

AppCoordinator::~AppCoordinator() noexcept
{
    Shutdown();
}

int AppCoordinator::Start(runtime::ModuleInitResult* detail) noexcept
{
    if (started_) {
        if (detail != nullptr) {
            detail->success = false;
            detail->module_name = "app-coordinator";
            detail->error_message =
                "Start() rejected: already started; call Shutdown() before restarting";
        }
        return kAlreadyStarted;
    }

    // 运行中重启不支持（产品裁决 2026-09-27）：SystemMode 一旦离开 UNINITIALIZED
    // 就不可回到可初始化状态，且一次 Shutdown() 之后业务模式仍停留在
    // STANDBY / MANUAL。此处显式拒绝并给出可诊断原因，而不是让
    // RequestModeTransition 失败后被误报成初始化失败，也不是让底层单独重启硬件
    // 而产生"硬件起来了、业务侧没重新初始化"的半初始化状态。
    if (authority_.system_mode() != SystemMode::UNINITIALIZED) {
        if (detail != nullptr) {
            detail->success = false;
            detail->module_name = "app-coordinator";
            detail->error_message =
                "in-process restart is not supported; restart the process. current mode: ";
            // SystemMode 的 ToString 声明在 marshland 命名空间（MarshlandTypes.hpp）；
            // RejectReason 等仲裁类型的是 state_arbitration::ToString。
            detail->error_message += marshland::ToString(authority_.system_mode());
        }
        return kRestartNotSupported;
    }

    // 1) 业务侧进入"初始化中"。
    const auto initializing = authority_.RequestModeTransition(
        SystemMode::INITIALIZING, state_arbitration::TransitionReason::START);
    if (!initializing.accepted) {
        if (detail != nullptr) {
            detail->success = false;
            detail->module_name = "app-coordinator";
            detail->error_message = "state machine rejected UNINITIALIZED -> INITIALIZING, reason: ";
            detail->error_message += state_arbitration::ToString(initializing.reason);
        }
        return kPreconditionFailed;
    }

    // 2) 底层拉起硬件与服务。本层不关心内部步骤，只消费最终就绪结论。
    startup_ = runtime_.start();
    const bool hardware_start_ok = startup_.succeeded();

    // 3) 唯一的启动结果 -> 业务模式映射
    applyStartupOutcome(hardware_start_ok, detail);
    started_ = true;
    return hardware_start_ok ? kOk : kRuntimeStartFailed;
}

void AppCoordinator::applyStartupOutcome(bool hardware_start_ok,
                                         runtime::ModuleInitResult* detail) noexcept
{
    if (hardware_start_ok) {
        hardware_ready_ = true;
        authority_.RequestModeTransition(SystemMode::STANDBY,
                                         state_arbitration::TransitionReason::INIT_SUCCESS);
        if (detail != nullptr) {
            detail->success = true;
            detail->module_name = "runtime";
            detail->error_message.clear();
        }
        return;
    }

    hardware_ready_ = false;
    authority_.RequestModeTransition(SystemMode::INIT_FAILED,
                                     state_arbitration::TransitionReason::INIT_FAILURE);

    if (detail == nullptr) {
        return;
    }

    // 失败原因取自底层步骤结果中第一个失败的必需步骤。
    // 对应 ATOM-01_Runtime.md 安全约束："启动失败须提供模块名和错误原因"。
    detail->success = false;
    detail->module_name.clear();
    detail->error_message.clear();
    for (const runtime::StepResult& step : startup_.steps) {
        if (step.required && step.attempted && !step.result.success) {
            detail->module_name = step.result.module_name;
            detail->error_message = step.result.error_message;
            break;
        }
    }
}

void AppCoordinator::Shutdown() noexcept
{
    // 只停硬件与服务。业务模式的清理由上层决定：
    // 本层不擅自把 SystemMode 拨到任何"安全模式"，避免与 ATOM-14 的安全编排冲突。
    if (!started_) {
        return;   // 幂等：析构路径与显式调用重叠时不会重复停止
    }
    runtime_.shutdown();
    hardware_ready_ = false;
    started_ = false;
}

RunGateReason AppCoordinator::RunGate() const noexcept
{
    // 顺序即安全优先级：急停锁存 > 硬件就绪 > 阻断故障 > 初始化/模式状态。
    //
    // 急停锁存单独判定，而不是依赖 IsOperationalMode：ATOM02-BUG-004 的修复把
    // 人工确认改为"只做模式迁移、保持锁存为 True"，因此确认之后模式已是 MANUAL
    // 而急停变量与锁存仍为 True。若只看模式，解除动作完成前执行器就会被放行，
    // 违反 Config §15 / PEF §8.3（解除动作完成后才允许恢复）。
    if (authority_.is_estop_latched()) {
        return RunGateReason::ESTOP_LATCHED;
    }
    if (!hardware_ready_) {
        return RunGateReason::HARDWARE_NOT_READY;
    }
    // 复用 ATOM-02 的故障等级归约，而不是在本层再实现一份：
    // 只有 SEVERE / ESTOP 全局阻断，INFO / WARNING 属可运行的降级状态。
    if (state_arbitration::IsFaultBlocking(authority_.inputs())) {
        return RunGateReason::FAULT_ACTIVE;
    }
    if (authority_.system_mode() == SystemMode::UNINITIALIZED) {
        return RunGateReason::NOT_INITIALIZED;
    }
    if (!IsOperationalMode(authority_.system_mode())) {
        return RunGateReason::MODE_NOT_OPERATIONAL;
    }
    return RunGateReason::ALLOWED;
}

bool AppCoordinator::CanRunActuators() const noexcept
{
    return RunGate() == RunGateReason::ALLOWED;
}

// ---------------------------------------------------------------------------
// 外部输入上报：全部委托字段级 setter，本层不持有副本
// ---------------------------------------------------------------------------
void AppCoordinator::ReportSbus(bool online, bool auto_mode) noexcept
{
    authority_.SetSbusStatus(online, auto_mode);
}

void AppCoordinator::ReportWebOnline(bool online) noexcept
{
    authority_.SetWebOnline(online);
}

void AppCoordinator::ReportYawArrived(bool arrived) noexcept
{
    authority_.SetYawArrived(arrived);
}

void AppCoordinator::ReportStopFire(bool set) noexcept
{
    authority_.SetStopFire(set);
}

void AppCoordinator::ReportFault(bool active, FaultSeverity severity) noexcept
{
    authority_.SetFault(active, severity);
}

void AppCoordinator::ReportSoftwareEstop(bool active) noexcept
{
    // 只上报事实。急停是锁存量（Config §15）：一旦置位即锁存，外部写回 false
    // 不会解除；解除只能由 ATOM-14 完成解除动作后调用
    // ReleaseSoftwareEstopLatch()。本层不代为确认或解除。
    authority_.SetSoftwareEstopInput(active);
}

void AppCoordinator::ReportClock() noexcept
{
    // 状态机生成事件时读取输入快照的时间戳（事件时间为 0 会退化为沿用上一次
    // 输入的时间），因此时间需要在事件产生之前落地。各来源完成一轮上报后
    // 调用一次即可。
    authority_.SetClock(static_cast<std::uint64_t>(clock_.nowMicros()), ++report_sequence_);
}

} // namespace marshland::app
