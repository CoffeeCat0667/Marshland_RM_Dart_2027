#include "AppCoordinator.hpp"

#include <utility>

#include "RuntimeRegistrationQuery.hpp"

namespace marshland::app {
namespace {

void SetDetail(runtime::ModuleInitResult* detail, bool success,
               const char* module, const char* message) noexcept
{
    if (detail == nullptr) {
        return;
    }
    detail->success = success;
    try {
        detail->module_name = module;
        detail->error_message = message;
    } catch (...) {
        detail->module_name.clear();
        detail->error_message.clear();
    }
}

} // namespace

AppCoordinator::AppCoordinator(runtime::RuntimeCoordinator& runtime,
                               state_arbitration::StateArbitrator& authority,
                               IMonotonicClock& clock) noexcept
    : runtime_(runtime), authority_(authority), clock_(clock)
{
}

AppCoordinator::~AppCoordinator() noexcept
{
    Shutdown();
}

int AppCoordinator::Start(runtime::ModuleInitResult* detail) noexcept
{
    std::unique_lock lifecycle_lock(lifecycle_mutex_, std::defer_lock);
    try {
        lifecycle_lock.lock();
        std::unique_lock state_lock(state_mutex_);

        if (started_) {
            SetDetail(detail, false, "app-coordinator", "already started; restart the process");
            return kAlreadyStarted;
        }
        if (start_attempted_ || shutdown_requested_ ||
            authority_.system_mode() != SystemMode::UNINITIALIZED ||
            runtime_.state() != runtime::RuntimeState::Created) {
            SetDetail(detail, false, "app-coordinator", "in-process restart or pre-started runtime is not supported");
            return kRestartNotSupported;
        }
        start_attempted_ = true;

        const auto count = marshland::app::detail::RegisteredModuleCount(runtime_);
        if (!count.has_value()) {
            SetDetail(detail, false, "app-coordinator",
                      "registration query unavailable: implement ATOM01-BUG-011 before startup");
            return kRegistrationInfoUnavailable;
        }
        if (*count == 0) {
            SetDetail(detail, false, "app-coordinator", "refusing to start: no lifecycle module is registered");
            return kRuntimeStartFailed;
        }

        reportClockLocked();
        const auto initializing = authority_.RequestModeTransition(
            SystemMode::INITIALIZING, state_arbitration::TransitionReason::START);
        if (!initializing.accepted) {
            SetDetail(detail, false, "app-coordinator", "state machine rejected startup");
            return kPreconditionFailed;
        }
        started_ = true;
        state_lock.unlock();

        // Device initialization may report inputs, so it must run without the state lock.
        auto result = runtime_.start();

        state_lock.lock();
        startup_ = std::move(result);
        const bool hardware_start_ok = startup_.succeeded();
        const bool mapped = applyStartupOutcomeLocked(hardware_start_ok, detail);
        state_lock.unlock();

        if (hardware_start_ok && !mapped) {
            runtime_.shutdown();
            state_lock.lock();
            started_ = false;
            state_lock.unlock();
            return kPreconditionFailed;
        }
        return hardware_start_ok ? kOk : kRuntimeStartFailed;
    } catch (...) {
        if (lifecycle_lock.owns_lock()) {
            lifecycle_lock.unlock();
        }
        Shutdown();
        SetDetail(detail, false, "app-coordinator", "startup bridge failed; hardware was shut down");
        return kRuntimeStartFailed;
    }
}

bool AppCoordinator::applyStartupOutcomeLocked(bool hardware_start_ok,
                                              runtime::ModuleInitResult* detail) noexcept
{
    hardware_ready_ = false;
    reportClockLocked();
    const auto transition = authority_.RequestModeTransition(
        hardware_start_ok ? SystemMode::STANDBY : SystemMode::INIT_FAILED,
        hardware_start_ok ? state_arbitration::TransitionReason::INIT_SUCCESS
                          : state_arbitration::TransitionReason::INIT_FAILURE);

    if (hardware_start_ok) {
        if (!transition.accepted) {
            SetDetail(detail, false, "app-coordinator",
                      "hardware started but the business state rejected startup; run gate remains closed");
            return false;
        }
        hardware_ready_ = true;
        SetDetail(detail, true, "runtime", "");
        return true;
    }

    for (const auto& step : startup_.steps) {
        if (step.required && step.attempted && !step.result.success) {
            SetDetail(detail, false, step.result.module_name.c_str(), step.result.error_message.c_str());
            return transition.accepted;
        }
    }
    SetDetail(detail, false, "runtime", "hardware startup failed; no module detail is available");
    return transition.accepted;
}

void AppCoordinator::Shutdown() noexcept
{
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    bool stop_runtime = false;
    {
        std::lock_guard state_lock(state_mutex_);
        shutdown_requested_ = true;
        hardware_ready_ = false;
        stop_runtime = started_;
        started_ = false;
    }
    if (stop_runtime) {
        runtime_.shutdown();
    }
}

RunGateReason AppCoordinator::runGateLocked() const noexcept
{
    if (authority_.is_estop_latched()) {
        return RunGateReason::ESTOP_LATCHED;
    }
    if (!hardware_ready_) {
        return RunGateReason::HARDWARE_NOT_READY;
    }
    if (state_arbitration::IsFaultBlocking(authority_.inputs())) {
        return RunGateReason::FAULT_ACTIVE;
    }
    if (authority_.system_mode() == SystemMode::UNINITIALIZED) {
        return RunGateReason::NOT_INITIALIZED;
    }
    if (!state_arbitration::IsOperationalMode(authority_.system_mode())) {
        return RunGateReason::MODE_NOT_OPERATIONAL;
    }
    return RunGateReason::ALLOWED;
}

RunGateReason AppCoordinator::RunGate() const noexcept
{
    std::lock_guard lock(state_mutex_);
    return runGateLocked();
}

bool AppCoordinator::CanRunActuators() const noexcept
{
    return RunGate() == RunGateReason::ALLOWED;
}

bool AppCoordinator::hardware_ready() const noexcept
{
    std::lock_guard lock(state_mutex_);
    return hardware_ready_;
}

runtime::RuntimeStartResult AppCoordinator::startup() const
{
    std::lock_guard lock(state_mutex_);
    return startup_;
}

std::vector<runtime::StepResult> AppCoordinator::stepResults() const
{
    std::lock_guard lock(state_mutex_);
    return startup_.steps;
}

state_arbitration::ExternalInputs AppCoordinator::lastInputs() const noexcept
{
    std::lock_guard lock(state_mutex_);
    return authority_.inputs();
}

StateReadView AppCoordinator::ReadState() const
{
    std::unique_lock lock(state_mutex_);
    return StateReadView(std::move(lock), authority_.context(), runGateLocked(),
                         hardware_ready_, authority_.is_estop_latched(),
                         authority_.run_id(), authority_.current_shot());
}

std::size_t AppCoordinator::CopyEvents(state_arbitration::StateEvent* output,
                                      std::size_t capacity) const noexcept
{
    std::lock_guard lock(state_mutex_);
    return authority_.CopyEvents(output, capacity);
}

std::size_t AppCoordinator::dropped_event_count() const noexcept
{
    std::lock_guard lock(state_mutex_);
    return authority_.dropped_event_count();
}

void AppCoordinator::reportClockLocked() noexcept
{
    const auto now = clock_.nowMicros();
    authority_.SetClock(now < 0 ? 0 : static_cast<std::uint64_t>(now), ++report_sequence_);
}

void AppCoordinator::reportStopFireLocked(bool set) noexcept
{
    authority_.SetStopFire(set);
}

void AppCoordinator::ReportSbus(bool online, bool auto_mode) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    authority_.SetSbusStatus(online, auto_mode);
}

void AppCoordinator::ReportWebOnline(bool online) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    authority_.SetWebOnline(online);
}

void AppCoordinator::ReportYawArrived(bool arrived) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    authority_.SetYawArrived(arrived);
}

void AppCoordinator::ReportStopFire(bool set) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    reportStopFireLocked(set);
}

void AppCoordinator::ReportFault(bool active, FaultSeverity severity) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    authority_.SetFault(active, severity);
}

void AppCoordinator::ReportSoftwareEstop(bool active) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    authority_.SetSoftwareEstopInput(active);
}

void AppCoordinator::ReportClock() noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
}

state_arbitration::IntentDecision AppCoordinator::SubmitIntent(
    const state_arbitration::ControlIntent& intent) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    return authority_.SubmitIntent(intent);
}

state_arbitration::IntentDecision AppCoordinator::SubmitStopFireIntent(
    const state_arbitration::ControlIntent& intent) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    auto checked = intent;
    if (intent.kind != state_arbitration::IntentKind::STOP_FIRE_SET &&
        intent.kind != state_arbitration::IntentKind::STOP_FIRE_CLEAR) {
        checked.valid = false;
    }
    const auto decision = authority_.SubmitIntent(checked);
    if (decision.accepted) {
        reportStopFireLocked(intent.kind == state_arbitration::IntentKind::STOP_FIRE_SET);
    }
    return decision;
}

state_arbitration::ModeTransitionResult AppCoordinator::RequestModeTransition(
    SystemMode target, state_arbitration::TransitionReason reason) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    return authority_.RequestModeTransition(target, reason);
}

state_arbitration::AutoFireTransitionResult AppCoordinator::RequestAutoFireState(
    AutoFireState target) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    return authority_.RequestAutoFireState(target);
}

state_arbitration::AutoFireTransitionResult AppCoordinator::BeginNewAutoFireRun() noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    return authority_.BeginNewAutoFireRun();
}

state_arbitration::AutoFireTransitionResult AppCoordinator::TerminateAutoFireOnSbusTakeover() noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    return authority_.TerminateAutoFireOnSbusTakeover();
}

state_arbitration::FireAuthorization AppCoordinator::AuthorizeFire(ControlSource source) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    return authority_.AuthorizeFire(source);
}

state_arbitration::FireAuthorization AppCoordinator::AuthorizeManualFire(ControlSource source) noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    return authority_.AuthorizeManualFire(source);
}

state_arbitration::ModeTransitionResult AppCoordinator::ConfirmSoftwareEstop() noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    return authority_.ConfirmSoftwareEstop();
}

bool AppCoordinator::ReleaseSoftwareEstopLatch() noexcept
{
    std::lock_guard lock(state_mutex_);
    reportClockLocked();
    return authority_.ReleaseSoftwareEstopLatch();
}

} // namespace marshland::app
