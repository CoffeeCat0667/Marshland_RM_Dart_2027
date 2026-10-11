#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "AppCoordinator.hpp"
#include "ProcessExitSignals.hpp"
#include "RuntimeRegistrationQuery.hpp"
#include "SystemMonotonicClock.hpp"

namespace {

using namespace marshland;
using namespace marshland::app;
using namespace marshland::runtime;
using namespace marshland::state_arbitration;
using namespace std::chrono_literals;

void Check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TestClock final : public IMonotonicClock {
public:
    std::int64_t nowMicros() const noexcept override { return value.load(); }
    std::atomic<std::int64_t> value{100};
};

class TestLifecycle final : public ILifecycleModule {
public:
    TestLifecycle(std::string_view module_name, bool succeeds, int& initializes, int& shutdowns,
                  std::function<void()> initialize_hook = {},
                  std::function<void()> shutdown_hook = {},
                  bool throw_initialize = false)
        : module_name_(module_name), succeeds_(succeeds), initializes_(initializes)
        , shutdowns_(shutdowns), initialize_hook_(std::move(initialize_hook))
        , shutdown_hook_(std::move(shutdown_hook)), throw_initialize_(throw_initialize)
    {
    }

    ModuleInitResult initialize(const RuntimeContext&) override
    {
        ++initializes_;
        if (initialize_hook_) {
            initialize_hook_();
        }
        if (throw_initialize_) {
            throw std::runtime_error("initialize exception");
        }
        return {succeeds_, std::string(module_name_), succeeds_ ? "" : "module rejected"};
    }

    void shutdown() noexcept override
    {
        ++shutdowns_;
        if (shutdown_hook_) {
            shutdown_hook_();
        }
    }

    const char* name() const noexcept override { return module_name_.data(); }

private:
    std::string module_name_;
    bool succeeds_;
    int& initializes_;
    int& shutdowns_;
    std::function<void()> initialize_hook_;
    std::function<void()> shutdown_hook_;
    bool throw_initialize_;
};

struct AppFixture {
    StateArbitrator authority;
    TestClock clock;
    RuntimeCoordinator runtime;
    AppCoordinator app{runtime, authority, clock};

    void EnterManual()
    {
        Check(app.RequestModeTransition(SystemMode::INITIALIZING, TransitionReason::START).accepted,
              "UNINITIALIZED -> INITIALIZING");
        Check(app.RequestModeTransition(SystemMode::STANDBY, TransitionReason::INIT_SUCCESS).accepted,
              "INITIALIZING -> STANDBY");
        Check(app.RequestModeTransition(SystemMode::MANUAL, TransitionReason::ENTER_MANUAL).accepted,
              "STANDBY -> MANUAL");
    }
};

void TestPublicTypes()
{
    Check(std::strcmp(ToString(SystemMode::FAULT), "FAULT") == 0, "system mode string");
    Check(std::strcmp(ToString(ControlSource::SBUS), "SBUS") == 0, "control source string");
    Check(std::strcmp(ToString(AutoFireState::FIRING), "FIRING") == 0, "auto state string");
    Check(std::strcmp(ToString(FaultSeverity::ESTOP), "ESTOP") == 0, "fault severity string");
    Check(ControlSourceRank(ControlSource::SBUS) > ControlSourceRank(ControlSource::WEB),
          "SBUS priority");
    Check(ControlSourceRank(ControlSource::WEB) > ControlSourceRank(ControlSource::AUTO),
          "WEB priority");
    Check(IsHigherPriority(ControlSource::SBUS, ControlSource::AUTO), "priority helper");
    Check(!IsHigherPriority(ControlSource::AUTO, ControlSource::WEB), "priority helper reverse");

    Check(std::strcmp(toString(RuntimeState::Created), "CREATED") == 0, "runtime state string");
    Check(std::strcmp(toString(RuntimeStep::InitializeSocketCan), "initialize-socketcan") == 0,
          "runtime step string");
    Check(std::strcmp(toString(ModuleRegistrationError::NullModule), "null-module") == 0,
          "registration error string");

    Check(std::strcmp(ToString(RunGateReason::ALLOWED), "ALLOWED") == 0, "gate string");
    Check(std::strcmp(ToString(RunGateReason::NOT_INITIALIZED), "NOT_INITIALIZED") == 0,
          "gate not initialized string");
}

void TestRegistrationQueryAndRuntimeRegistration()
{
    struct Available {
        std::size_t registeredModuleCount() const noexcept { return 3; }
    };
    struct Missing {};
    struct WrongType {
        int registeredModuleCount() const noexcept { return 3; }
    };
    Check(detail::RegisteredModuleCount(Available{}) == 3, "registration query forwards");
    Check(!detail::RegisteredModuleCount(Missing{}), "missing registration query fails closed");
    Check(!detail::RegisteredModuleCount(WrongType{}), "wrong registration query type fails closed");

    RuntimeCoordinator runtime;
    int initializes = 0;
    int shutdowns = 0;
    Check(runtime.registeredModuleCount() == 0, "runtime starts empty");
    Check(!runtime.addModule({RuntimeStep::InitializeSocketCan, true, nullptr}), "null module rejected");
    Check(runtime.lastRegistrationError() == ModuleRegistrationError::NullModule,
          "null module error recorded");
    Check(runtime.addModule({RuntimeStep::StartHttpAndWebSocket, true,
          std::make_unique<TestLifecycle>("later", true, initializes, shutdowns)}),
          "later module registered");
    Check(runtime.addModule({RuntimeStep::InitializeSocketCan, true,
          std::make_unique<TestLifecycle>("first", true, initializes, shutdowns)}),
          "first module registered");
    Check(runtime.addModule({RuntimeStep::InitializeSocketCan, false,
          std::make_unique<TestLifecycle>("same-step", true, initializes, shutdowns)}),
          "same-step module registered");
    Check(runtime.registeredModuleCount() == 3, "registration count includes every module");
    const auto result = runtime.start();
    Check(result.succeeded() && result.steps.size() == 3, "runtime start result");
    Check(result.steps[0].result.module_name == "first" &&
              result.steps[1].result.module_name == "same-step" &&
              result.steps[2].result.module_name == "later",
          "runtime sorts steps and preserves FIFO");
    Check(!runtime.addModule({RuntimeStep::CreateDiagnostics, true,
          std::make_unique<TestLifecycle>("late", true, initializes, shutdowns)}),
          "late registration rejected");
    Check(runtime.lastRegistrationError() == ModuleRegistrationError::IllegalState,
          "late registration error recorded");
    runtime.shutdown();
    Check(shutdowns == 3 && runtime.state() == RuntimeState::Stopped, "reverse shutdown");
    runtime.shutdown();
    Check(shutdowns == 3, "runtime shutdown idempotent");
}

void TestRuntimeFailureAndExceptionPaths()
{
    {
        RuntimeCoordinator runtime;
        int initializes = 0;
        int shutdowns = 0;
        Check(runtime.addModule({RuntimeStep::InitializeSocketCan, true,
              std::make_unique<TestLifecycle>("ok", true, initializes, shutdowns)}), "register ok");
        Check(runtime.addModule({RuntimeStep::InitializeMotorAndEncoderChannels, true,
              std::make_unique<TestLifecycle>("required-fail", false, initializes, shutdowns)}),
              "register required failure");
        Check(runtime.start().state == RuntimeState::InitFailed, "required failure state");
        Check(initializes == 2 && shutdowns == 1, "required failure rolls back successful modules");
        Check(!runtime.start().succeeded() && initializes == 2, "failed runtime cannot restart");
    }
    {
        RuntimeCoordinator runtime;
        int initializes = 0;
        int shutdowns = 0;
        Check(runtime.addModule({RuntimeStep::InitializeSocketCan, true,
              std::make_unique<TestLifecycle>("throw-init", true, initializes, shutdowns,
                                               std::function<void()>{}, std::function<void()>{}, true)}),
              "register throwing module");
        Check(runtime.start().state == RuntimeState::InitFailed, "exception becomes init failure");
        Check(initializes == 1 && shutdowns == 0, "throwing module not marked started");
    }
    {
        RuntimeCoordinator runtime;
        int initializes = 0;
        int shutdowns = 0;
        Check(runtime.addModule({RuntimeStep::InitializeSocketCan, true,
              std::make_unique<TestLifecycle>("shutdown-module", true, initializes, shutdowns)}),
              "register shutdown module");
        Check(runtime.start().succeeded(), "shutdown throw module starts");
        runtime.shutdown();
        Check(shutdowns == 1 && runtime.state() == RuntimeState::Stopped,
              "shutdown exception is contained");
    }
}

void TestStatePolicyAndStringTables()
{
    ArbitrationContext context{};
    Check(SelectControlSource(context) == ControlSource::AUTO, "default control source");
    context.inputs.web_online = true;
    Check(SelectControlSource(context) == ControlSource::WEB, "web control source");
    context.inputs.sbus_online = true;
    context.inputs.sbus_auto_mode = false;
    Check(SelectControlSource(context) == ControlSource::SBUS, "sbus control source");
    context.inputs.sbus_auto_mode = true;
    Check(SelectControlSource(context) == ControlSource::AUTO, "sbus auto control source");
    context.inputs.software_estop = true;
    context.system_mode = SystemMode::SOFTWARE_ESTOP;
    Check(SelectControlSource(context) == ControlSource::NONE, "estop control source");

    Check(IsOperationalMode(SystemMode::MANUAL), "manual operational");
    Check(!IsOperationalMode(SystemMode::RECOVERY), "recovery not ordinary operational");
    Check(IsFaultBlocking(ExternalInputs{.fault_active = true, .fault_severity = FaultSeverity::SEVERE}),
          "severe fault blocks");
    Check(!IsFaultBlocking(ExternalInputs{.fault_active = true, .fault_severity = FaultSeverity::WARNING}),
          "warning fault does not block");
    Check(IsAutoFireStateTerminal(AutoFireState::AUTO_COMPLETED), "completed terminal");
    Check(!IsAutoFireStateTerminal(AutoFireState::FIRING), "firing nonterminal");

    Check(IsModeTransitionAllowed(SystemMode::UNINITIALIZED, SystemMode::INITIALIZING,
                                  TransitionReason::START), "start edge");
    Check(!IsModeTransitionAllowed(SystemMode::STANDBY, SystemMode::FAULT,
                                   TransitionReason::INIT_FAILURE), "invalid mode edge");
    Check(IsAutoFireTransitionAllowed(AutoFireState::FORCE, AutoFireState::SHOT_1_YAW,
                                      ShotStage::FIRST), "first shot edge");
    Check(!IsAutoFireTransitionAllowed(AutoFireState::FORCE, AutoFireState::SHOT_1_YAW,
                                       ShotStage::SUBSEQUENT), "stage disambiguation");

    for (const auto reason : {TransitionReason::NONE, TransitionReason::START,
                              TransitionReason::INIT_SUCCESS, TransitionReason::INIT_FAILURE,
                              TransitionReason::ENTER_MANUAL, TransitionReason::AUTO_START,
                              TransitionReason::SBUS_TAKEOVER, TransitionReason::WEB_MANUAL_TAKEOVER,
                              TransitionReason::SOFTWARE_ESTOP_TRIGGER, TransitionReason::ESTOP_CONFIRM,
                              TransitionReason::ESTOP_RELEASED, TransitionReason::FAULT_DETECTED,
                              TransitionReason::RECOVERY_START, TransitionReason::RECOVERY_COMPLETE,
                              TransitionReason::DEBUG_ENTER, TransitionReason::DEBUG_EXIT,
                              TransitionReason::CALIBRATION_ENTER, TransitionReason::CALIBRATION_EXIT,
                              TransitionReason::AUTO_FLOW_RUN, TransitionReason::AUTO_FLOW_STEP,
                              TransitionReason::AUTO_FLOW_DONE, TransitionReason::AUTO_FLOW_ABORT,
                              TransitionReason::CONTROL_SOURCE_REFRESH}) {
        Check(std::strcmp(ToString(reason), "UNKNOWN") != 0, "transition reason string table");
    }
    for (const auto kind : {IntentKind::CONTROL, IntentKind::FIRE, IntentKind::SAFETY,
                            IntentKind::SAFETY_RECOVERY, IntentKind::STOP_FIRE_SET,
                            IntentKind::STOP_FIRE_CLEAR, IntentKind::AUTO_START, IntentKind::AUTO_STOP}) {
        Check(std::strcmp(ToString(kind), "UNKNOWN") != 0, "intent kind string table");
    }
}

void TestStateInputsEstopAndEvents()
{
    StateArbitrator authority;
    authority.Reset(42);
    Check(authority.inputs().stop_fire && authority.current_shot() == 0, "state defaults");
    authority.SetWebOnline(true);
    authority.SetSbusStatus(true, false);
    Check(authority.control_source() == ControlSource::SBUS, "field setters preserve priority");
    authority.SetSoftwareEstopInput(true);
    Check(authority.is_estop_latched() && authority.system_mode() == SystemMode::SOFTWARE_ESTOP,
          "estop input locks and changes mode");
    authority.SetSoftwareEstopInput(false);
    Check(authority.is_estop_latched() && authority.is_software_estop(),
          "false estop input cannot release latch");
    Check(authority.ConfirmSoftwareEstop().accepted && authority.system_mode() == SystemMode::MANUAL,
          "estop confirmation changes mode");
    Check(authority.ReleaseSoftwareEstopLatch(), "release after confirmation");
    Check(!authority.is_estop_latched() && !authority.is_software_estop(), "estop released");

    authority.ClearEvents();
    authority.SetClock(99, 7);
    authority.SetSbusStatus(false, false);
    std::array<StateEvent, StateArbitrator::kMaxEvents> events{};
    const auto count = authority.CopyEvents(events.data(), events.size());
    Check(count > 0 && events[count - 1].timestamp_us == 99, "event timestamp");
    Check(authority.latest_event() != nullptr, "latest event");
    authority.ClearEvents();
    Check(authority.event_count() == 0 && authority.latest_event() == nullptr, "clear events");
    authority.SetWebOnline(false);
    authority.ClearEvents();

    for (std::size_t i = 0; i < StateArbitrator::kMaxEvents + 5; ++i) {
        authority.SetClock(100 + i, i + 1);
        authority.SetWebOnline((i % 2) == 0);
    }
    Check(authority.event_count() == StateArbitrator::kMaxEvents && authority.dropped_event_count() > 0,
          "event overflow is counted");
    Check(authority.CopyEvents(nullptr, 4) == 0 && authority.CopyEvents(events.data(), 0) == 0,
          "event copy invalid arguments");
}

void TestStateIntentAndFireAuthorization()
{
    StateArbitrator authority;
    authority.RequestModeTransition(SystemMode::INITIALIZING, TransitionReason::START);
    authority.RequestModeTransition(SystemMode::STANDBY, TransitionReason::INIT_SUCCESS);
    authority.RequestModeTransition(SystemMode::MANUAL, TransitionReason::ENTER_MANUAL);
    authority.SetWebOnline(true);
    authority.SetStopFire(false);
    authority.SetYawArrived(true);

    ControlIntent invalid{false, ControlSource::WEB, IntentKind::CONTROL};
    Check(!authority.SubmitIntent(invalid).accepted, "invalid intent rejected");
    Check(authority.SubmitIntent({true, ControlSource::WEB, IntentKind::SAFETY}).accepted,
          "safety intent accepted");
    Check(authority.SubmitIntent({true, ControlSource::WEB, IntentKind::SAFETY_RECOVERY}).accepted,
          "recovery intent after confirmation phase accepted");
    Check(authority.SubmitIntent({true, ControlSource::WEB, IntentKind::STOP_FIRE_SET}).accepted,
          "stop set accepted in manual");
    authority.SetStopFire(false);
    Check(authority.SubmitIntent({true, ControlSource::WEB, IntentKind::STOP_FIRE_CLEAR}).accepted,
          "stop clear accepted in manual");
    Check(authority.AuthorizeFire(ControlSource::WEB).allowed,
          "web fire authorized with all prerequisites satisfied");
    authority.RefreshControlSource();
    Check(authority.AuthorizeManualFire(ControlSource::WEB).allowed,
          "manual fire authorized for web");
    Check(authority.AuthorizeManualFire(ControlSource::AUTO).reason == RejectReason::LOWER_PRIORITY_CONFLICT,
          "auto manual fire rejected");

    authority.SetSbusStatus(true, false);
    authority.SetStopFire(false);
    Check(authority.AuthorizeFire(ControlSource::AUTO, ControlSource::SBUS).reason ==
              RejectReason::LOWER_PRIORITY_CONFLICT,
          "low priority automatic fire rejected");
    authority.SetSbusStatus(false, false);
    authority.SetFault(true, FaultSeverity::SEVERE);
    Check(authority.AuthorizeFire(ControlSource::WEB).reason == RejectReason::FAULT_ACTIVE,
          "fault blocks fire");
    authority.SetFault(false, FaultSeverity::INFO);
    authority.SetYawArrived(false);
    Check(authority.AuthorizeFire(ControlSource::WEB).reason == RejectReason::YAW_NOT_ARRIVED,
          "yaw blocks fire");
}

void TestAutoFlowAndTakeover()
{
    StateArbitrator authority;
    authority.RequestModeTransition(SystemMode::INITIALIZING, TransitionReason::START);
    authority.RequestModeTransition(SystemMode::STANDBY, TransitionReason::INIT_SUCCESS);
    authority.RequestModeTransition(SystemMode::AUTO_FIRE, TransitionReason::AUTO_START);
    Check(authority.BeginNewAutoFireRun().accepted && authority.current_shot() == 1,
          "auto run begins at shot one");
    Check(authority.RequestAutoFireState(AutoFireState::CHARGE).accepted, "charge transition");
    Check(authority.RequestAutoFireState(AutoFireState::FORCE).accepted, "force transition");
    Check(authority.RequestAutoFireState(AutoFireState::SHOT_1_YAW).accepted, "first yaw transition");
    Check(authority.TerminateAutoFireOnSbusTakeover().reason == RejectReason::NOT_SBUS_TAKEOVER,
          "takeover requires sbus state");
    authority.SetSbusStatus(true, false);
    Check(authority.TerminateAutoFireOnSbusTakeover().accepted, "sbus takeover terminates");
    Check(authority.auto_fire_state() == AutoFireState::AUTO_TERMINATED &&
              authority.system_mode() == SystemMode::MANUAL,
          "takeover leaves manual terminal state");
    Check(authority.BeginNewAutoFireRun().reason == RejectReason::NOT_AUTO_FIRE_MODE,
          "cannot restart run outside auto mode");

    StateArbitrator completed;
    completed.RequestModeTransition(SystemMode::INITIALIZING, TransitionReason::START);
    completed.RequestModeTransition(SystemMode::STANDBY, TransitionReason::INIT_SUCCESS);
    completed.RequestModeTransition(SystemMode::AUTO_FIRE, TransitionReason::AUTO_START);
    Check(completed.BeginNewAutoFireRun().accepted, "completed flow starts");
    const std::array sequence{
        AutoFireState::CHARGE, AutoFireState::FORCE, AutoFireState::SHOT_1_YAW,
        AutoFireState::FIRE_READY, AutoFireState::FIRING, AutoFireState::FIRE_COMPLETED,
        AutoFireState::CHARGE, AutoFireState::FORCE, AutoFireState::SHOT_N_YAW,
        AutoFireState::FIRE_READY, AutoFireState::FIRING, AutoFireState::FIRE_COMPLETED};
    for (const auto state : sequence) {
        if (state == AutoFireState::FIRING) {
            completed.SetStopFire(false);
            completed.SetYawArrived(true);
        }
        Check(completed.RequestAutoFireState(state, ControlSource::AUTO).accepted,
              "subsequent auto state transition");
    }
    Check(completed.current_shot() == 2, "shot counter increments");
    Check(completed.RequestAutoFireState(AutoFireState::AUTO_COMPLETED).reason ==
              RejectReason::SHOT_STAGE_MISMATCH,
          "cannot complete before shot four");
    Check(completed.RequestAutoFireState(AutoFireState::AUTO_TERMINATED).accepted,
          "auto stop terminates active flow");
    Check(completed.RequestAutoFireState(AutoFireState::AUTO_TERMINATED).reason ==
              RejectReason::ALREADY_IN_TARGET,
          "terminal stop is idempotent");
}

void TestAppCoordinatorAndPorts()
{
    AppFixture fixture;
    int initializes = 0;
    int shutdowns = 0;
    Check(fixture.runtime.addModule({RuntimeStep::InitializeSocketCan, true,
          std::make_unique<TestLifecycle>("app-module", true, initializes, shutdowns,
              [&] { fixture.app.ReportWebOnline(true); },
              [&] { fixture.app.ReportWebOnline(false); })}), "register app module");
    ModuleInitResult detail;
    Check(fixture.app.Start(&detail) == AppCoordinator::kOk && detail.success,
          "app start");
    Check(initializes == 1 && fixture.app.hardware_ready() &&
              fixture.app.RunGate() == RunGateReason::ALLOWED,
          "app gate after start");
    Check(fixture.app.Start() == AppCoordinator::kAlreadyStarted, "app duplicate start");

    fixture.app.ReportSbus(true, false);
    Check(fixture.app.lastInputs().sbus_online && fixture.app.lastInputs().web_online,
          "field report preserves other inputs");
    fixture.app.ReportFault(true, FaultSeverity::WARNING);
    Check(fixture.app.RunGate() == RunGateReason::ALLOWED, "warning fault does not block gate");
    fixture.app.ReportFault(true, FaultSeverity::SEVERE);
    Check(fixture.app.RunGate() == RunGateReason::FAULT_ACTIVE, "severe fault blocks gate");
    fixture.app.ReportSoftwareEstop(true);
    Check(fixture.app.RunGate() == RunGateReason::ESTOP_LATCHED, "estop dominates gate");
    fixture.app.ReportSoftwareEstop(false);
    Check(fixture.app.RunGate() == RunGateReason::ESTOP_LATCHED, "estop false report cannot release");

    const auto startup = fixture.app.startup();
    const auto steps = fixture.app.stepResults();
    const auto inputs = fixture.app.lastInputs();
    Check(startup.succeeded() && steps.size() == 1 && inputs.sbus_online,
          "diagnostic copies");
    std::array<StateEvent, StateArbitrator::kMaxEvents> events{};
    Check(fixture.app.CopyEvents(events.data(), events.size()) > 0, "event copy through app");
    Check(fixture.app.dropped_event_count() == 0, "event drop query");

    IControlAuthority& control = fixture.app;
    fixture.app.ReportStopFire(true);
    const auto clear = control.SubmitStopFireIntent({true, ControlSource::WEB, IntentKind::STOP_FIRE_CLEAR});
    Check(!clear.accepted && fixture.app.lastInputs().stop_fire, "rejected stop clear has no side effect");
    fixture.app.ReportSoftwareEstop(false);
    fixture.app.Shutdown();
    Check(shutdowns == 1 && !fixture.app.hardware_ready(), "app shutdown");
    fixture.app.Shutdown();
    Check(shutdowns == 1, "app shutdown idempotent");
}

void TestReadLeaseAndConcurrency()
{
    AppFixture fixture;
    fixture.clock.value = 777;
    fixture.app.ReportSbus(true, false);
    std::array<StateEvent, StateArbitrator::kMaxEvents> events{};
    Check(fixture.app.CopyEvents(events.data(), events.size()) > 0 &&
              events[fixture.app.CopyEvents(events.data(), events.size()) - 1].timestamp_us == 777,
          "clock precedes event");

    std::future<void> writer;
    bool was_blocked = false;
    {
        auto lease = fixture.app.ReadState();
        Check(&lease.inputs() == &fixture.authority.inputs(), "read view is zero copy");
        std::promise<void> started;
        auto ready = started.get_future();
        writer = std::async(std::launch::async, [&] {
            started.set_value();
            fixture.app.ReportYawArrived(true);
        });
        ready.wait();
        was_blocked = writer.wait_for(20ms) == std::future_status::timeout;
    }
    writer.get();
    Check(was_blocked, "read lease blocks writer");
    Check(fixture.app.lastInputs().yaw_arrived, "writer resumes after lease");

    std::atomic<bool> begin{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            while (!begin.load()) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < 250; ++iteration) {
                const bool value = (iteration % 2) != 0;
                if (worker == 0) fixture.app.ReportSbus(value, false);
                if (worker == 1) fixture.app.ReportWebOnline(value);
                if (worker == 2) fixture.app.ReportYawArrived(value);
                if (worker == 3) fixture.app.ReportStopFire(value);
            }
        });
    }
    begin = true;
    for (auto& worker : workers) worker.join();
    Check(fixture.app.lastInputs().sequence == 1002, "serialized input sequence");
    const auto copied = fixture.app.CopyEvents(events.data(), events.size());
    for (std::size_t i = 1; i < copied; ++i) {
        Check(events[i].sequence > events[i - 1].sequence, "event sequence monotonic");
    }
}

void TestExitSignals()
{
    ProcessExitSignals signals;
    int signal = 0;
    Check(signals.Wait(signal) == EINVAL, "unprepared wait rejected");
    Check(signals.Prepare() == 0 && signals.Prepare() == 0, "signal prepare idempotent");
    Check(kill(getpid(), SIGINT) == 0, "send pending SIGINT");
    Check(signals.Wait(signal) == 0 && signal == SIGINT, "SIGINT delivered to sigwait");
    std::thread sender([] { kill(getpid(), SIGTERM); });
    Check(signals.Wait(signal) == 0 && signal == SIGTERM, "SIGTERM delivered to sigwait");
    sender.join();
}

#include "BehaviorCases.hpp"

using TestFunction = void (*)();

const std::pair<std::string_view, TestFunction> kTests[]{
    {"public.types", &TestPublicTypes},
    {"runtime.registration", &TestRegistrationQueryAndRuntimeRegistration},
    {"runtime.failure_exception", &TestRuntimeFailureAndExceptionPaths},
    {"state.policy_tables", &TestStatePolicyAndStringTables},
    {"state.inputs_events", &TestStateInputsEstopAndEvents},
    {"state.intent_fire", &TestStateIntentAndFireAuthorization},
    {"state.auto_flow", &TestAutoFlowAndTakeover},
    {"app.coordinator_ports", &TestAppCoordinatorAndPorts},
    {"app.read_lease_concurrency", &TestReadLeaseAndConcurrency},
    {"process.exit_signals", &TestExitSignals},
    {"public.enum_defaults", &TestEnumsAndDefaults},
    {"public.monotonic_clock", &TestSystemClock},
    {"runtime.context_and_order", &TestRuntimeContextAndOrder},
    {"runtime.optional_failure", &TestRuntimeOptionalFailure},
    {"runtime.fallback_names", &TestRuntimeFallbackNames},
    {"runtime.unknown_exception", &TestRuntimeUnknownException},
    {"runtime.empty_and_destructor", &TestRuntimeEmptyAndDestructor},
    {"state.fire_matrix", &TestFireMatrix},
    {"state.intent_safety_matrix", &TestSafetyIntentMatrix},
    {"state.stop_clear_matrix", &TestStopClearMatrix},
    {"state.ordinary_intents", &TestOrdinaryIntents},
    {"state.mode_matrix", &TestModeMatrix},
    {"state.auto_edge_matrix", &TestAutoEdgeMatrix},
    {"state.all_strings", &TestAllStateStrings},
    {"state.reset_and_update", &TestResetAndUpdate},
    {"state.event_buffer", &TestEventBuffer},
    {"state.four_shot_run", &TestFourShotRun},
    {"state.auto_rejections", &TestAutoRejections},
    {"state.manual_fire_modes", &TestManualFireModes},
    {"app.startup_rejections", &TestAppStartupRejections},
    {"app.startup_failures", &TestAppStartupFailures},
    {"app.startup_interrupt", &TestAppStartupInterrupt},
    {"app.gate_modes", &TestAppGateModes},
    {"app.all_ports", &TestAppAllPorts},
    {"app.stop_flag_atomic_commit", &TestAppStopFlagCommit},
    {"app.read_view_move", &TestAppReadViewMove},
    {"app.clock_and_events", &TestAppClockAndEvents},
};

int Run(std::string_view requested)
{
    for (const auto& [name, test] : kTests) {
        if (name == requested) {
            test();
            std::printf("PASS %.*s\n", static_cast<int>(name.size()), name.data());
            return 0;
        }
    }
    std::fprintf(stderr, "unknown test: %.*s\n", static_cast<int>(requested.size()), requested.data());
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <test-name>\n", argv[0]);
        return 2;
    }
    try {
        return Run(argv[1]);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s: %s\n", argv[1], error.what());
        return 1;
    }
}
