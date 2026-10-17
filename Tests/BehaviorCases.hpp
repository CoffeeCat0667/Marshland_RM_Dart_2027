// Included inside the test runner's private namespace; no production dependency.

void TestEnumsAndDefaults()
{
    for (int i = 0; i <= static_cast<int>(SystemMode::FAULT); ++i)
        Check(std::strcmp(ToString(static_cast<SystemMode>(i)), "UNKNOWN") != 0, "all system modes");
    for (int i = 0; i <= static_cast<int>(ControlSource::AUTO); ++i)
        Check(std::strcmp(ToString(static_cast<ControlSource>(i)), "UNKNOWN") != 0, "all control sources");
    for (int i = 0; i <= static_cast<int>(AutoFireState::AUTO_TERMINATED); ++i)
        Check(std::strcmp(ToString(static_cast<AutoFireState>(i)), "UNKNOWN") != 0, "all auto states");
    for (int i = 0; i <= static_cast<int>(FaultSeverity::ESTOP); ++i)
        Check(std::strcmp(ToString(static_cast<FaultSeverity>(i)), "UNKNOWN") != 0, "all fault severities");
    for (int i = 0; i <= static_cast<int>(RuntimeState::Stopped); ++i)
        Check(std::strcmp(toString(static_cast<RuntimeState>(i)), "UNKNOWN") != 0, "all runtime states");
    for (int i = 0; i <= static_cast<int>(RuntimeStep::StartHttpAndWebSocket); ++i)
        Check(std::strcmp(toString(static_cast<RuntimeStep>(i)), "unknown-step") != 0, "all runtime steps");
    for (int i = 0; i <= static_cast<int>(ModuleRegistrationError::NullModule); ++i)
        Check(std::strcmp(toString(static_cast<ModuleRegistrationError>(i)), "unknown") != 0, "all registration errors");
    for (int i = 0; i <= static_cast<int>(RunGateReason::NOT_INITIALIZED); ++i)
        Check(std::strcmp(ToString(static_cast<RunGateReason>(i)), "UNKNOWN") != 0, "all gate strings");
    Check(std::strcmp(ToString(static_cast<SystemMode>(255)), "UNKNOWN") == 0, "invalid system mode");
    Check(std::strcmp(ToString(static_cast<ControlSource>(255)), "UNKNOWN") == 0, "invalid source");
    Check(std::strcmp(ToString(static_cast<AutoFireState>(255)), "UNKNOWN") == 0, "invalid auto state");
    Check(std::strcmp(ToString(static_cast<FaultSeverity>(255)), "UNKNOWN") == 0, "invalid fault");
    Check(std::strcmp(toString(static_cast<RuntimeState>(255)), "UNKNOWN") == 0, "invalid runtime state");
    Check(std::strcmp(toString(static_cast<RuntimeStep>(255)), "unknown-step") == 0, "invalid runtime step");
    Check(std::strcmp(toString(static_cast<ModuleRegistrationError>(255)), "unknown") == 0, "invalid registration error");
    Check(std::strcmp(ToString(static_cast<RunGateReason>(255)), "UNKNOWN") == 0, "invalid gate");
    Check(ControlSourceRank(ControlSource::NONE) == 0 && ControlSourceRank(static_cast<ControlSource>(255)) == 0,
          "none and invalid priority");
    const ExternalInputs inputs;
    Check(inputs.stop_fire && !inputs.yaw_arrived && !inputs.software_estop &&
              inputs.fault_severity == FaultSeverity::SEVERE, "fail-safe input defaults");
    Check(!IntentDecision{}.accepted && !FireAuthorization{}.allowed && !ModuleInitResult{}.success,
          "default results deny");
    Check(StepResult{}.required && !StepResult{}.attempted && ModuleRegistration{}.required,
          "required startup defaults");
    Check(!RuntimeStartResult{}.succeeded(), "default runtime result not ready");
}

void TestSystemClock()
{
    SystemMonotonicClock clock;
    IMonotonicClock& port = clock;
    const auto first = port.nowMicros();
    const auto second = port.nowMicros();
    Check(first >= 0 && second >= first, "steady clock conversion and monotonicity");
}

void TestRuntimeContextAndOrder()
{
    RuntimeContext context{"./test-only.nlohmann", {{"test-key", "test-value"}}};
    int initializes = 0, stops = 0;
    std::vector<int> order;
    RuntimeCoordinator runtime{context};
    context.values["test-key"] = "changed";
    Check(runtime.context().values.at("test-key") == "test-value", "runtime owns its context");
    for (int index : {2, 0, 1}) {
        const auto step = index == 2 ? RuntimeStep::StartHttpAndWebSocket : RuntimeStep::InitializeSocketCan;
        Check(runtime.addModule({step, true, std::make_unique<TestLifecycle>("order", true,
              initializes, stops, [&, index] { order.push_back(index); },
              [&, index] { order.push_back(index + 10); })}), "order registration");
    }
    Check(runtime.start().succeeded(), "order startup");
    runtime.shutdown();
    Check(order == std::vector<int>({0, 1, 2, 12, 11, 10}), "startup order and reverse release");
    Check(!runtime.start().succeeded() && initializes == 3, "no stopped restart");
}

void TestRuntimeOptionalFailure()
{
    int initializes = 0, stops = 0;
    RuntimeCoordinator runtime;
    Check(runtime.addModule({RuntimeStep::CreateDiagnostics, false,
          std::make_unique<TestLifecycle>("optional", false, initializes, stops)}), "optional registration");
    Check(runtime.addModule({RuntimeStep::InitializeSocketCan, true,
          std::make_unique<TestLifecycle>("required", true, initializes, stops)}), "required registration");
    const auto result = runtime.start();
    Check(result.succeeded() && result.steps.size() == 2 && !result.steps[0].result.success,
          "optional failure preserved without stopping sequence");
    Check(runtime.start().succeeded() && initializes == 2, "repeat start returns snapshot");
    runtime.shutdown();
    Check(stops == 1, "only successful resources stopped");
}

class NamelessLifecycle final : public ILifecycleModule {
public:
    explicit NamelessLifecycle(bool null_name) : null_name_(null_name) {}
    ModuleInitResult initialize(const RuntimeContext&) override { return {true, "", ""}; }
    void shutdown() noexcept override {}
    const char* name() const noexcept override { return null_name_ ? nullptr : "fallback-name"; }
private:
    bool null_name_;
};

void TestRuntimeFallbackNames()
{
    RuntimeCoordinator runtime;
    Check(runtime.addModule({RuntimeStep::CreateDiagnostics, true, std::make_unique<NamelessLifecycle>(false)}), "fallback registration");
    Check(runtime.addModule({RuntimeStep::CreateDiagnostics, true, std::make_unique<NamelessLifecycle>(true)}), "null-name registration");
    const auto result = runtime.start();
    Check(result.steps[0].result.module_name == "fallback-name" &&
          result.steps[1].result.module_name == "unknown-module", "name fallback results");
}

void TestRuntimeUnknownException()
{
    int initializes = 0, stops = 0;
    RuntimeCoordinator runtime;
    Check(runtime.addModule({RuntimeStep::CreateDiagnostics, true,
          std::make_unique<TestLifecycle>("unknown", true, initializes, stops, [] { throw 42; })}),
          "unknown exception registration");
    const auto result = runtime.start();
    Check(result.state == RuntimeState::InitFailed &&
          result.steps.front().result.error_message.find("unknown exception") != std::string::npos,
          "non-standard exception converted");
}

void TestRuntimeEmptyAndDestructor()
{
    RuntimeCoordinator empty;
    const auto result = empty.start();
    Check(result.succeeded() && result.steps.empty(), "runtime empty baseline, app must guard");
    RuntimeCoordinator never_started;
    never_started.shutdown();
    Check(never_started.state() == RuntimeState::Stopped, "shutdown before start");
    int initializes = 0, stops = 0;
    {
        RuntimeCoordinator runtime;
        Check(runtime.addModule({RuntimeStep::CreateDiagnostics, true,
              std::make_unique<TestLifecycle>("destructor", true, initializes, stops)}), "destructor registration");
        Check(runtime.start().succeeded(), "destructor startup");
    }
    Check(stops == 1, "destructor releases exactly once");
}

void TestFireMatrix()
{
    for (int source = 0; source <= 3; ++source) {
        for (int holder = 0; holder <= 3; ++holder) {
            for (unsigned mask = 0; mask < 16; ++mask) {
                ArbitrationContext context{};
                context.control_source = static_cast<ControlSource>(holder);
                context.inputs.software_estop = (mask & 1) != 0;
                context.inputs.fault_active = (mask & 2) != 0;
                context.inputs.stop_fire = (mask & 4) != 0;
                context.inputs.yaw_arrived = (mask & 8) == 0;
                RejectReason expected = RejectReason::NONE;
                if (source == 0) expected = RejectReason::INVALID_INTENT;
                else if (mask & 1) expected = RejectReason::SOFTWARE_ESTOP;
                else if (mask & 2) expected = RejectReason::FAULT_ACTIVE;
                else if (mask & 4) expected = RejectReason::STOP_FIRE_SET;
                else if (mask & 8) expected = RejectReason::YAW_NOT_ARRIVED;
                else if (holder == 0) expected = RejectReason::NO_CONTROL_AUTHORITY;
                else if (ControlSourceRank(static_cast<ControlSource>(source)) <
                         ControlSourceRank(static_cast<ControlSource>(holder))) expected = RejectReason::LOWER_PRIORITY_CONFLICT;
                const auto result = EvaluateFireAuthorization(context, static_cast<ControlSource>(source),
                                                               static_cast<ControlSource>(holder));
                Check(result.reason == expected && result.allowed == (expected == RejectReason::NONE),
                      "fire authorization matrix");
            }
        }
    }
}

void TestSafetyIntentMatrix()
{
    for (int mode = 0; mode <= static_cast<int>(SystemMode::FAULT); ++mode) {
        ArbitrationContext context{};
        context.system_mode = static_cast<SystemMode>(mode);
        context.inputs.software_estop = true;
        context.inputs.fault_active = true;
        Check(EvaluateIntent(context, {true, ControlSource::AUTO, IntentKind::SAFETY}).accepted,
              "zero-output safety is allowed");
        const auto recovery = EvaluateIntent(context, {true, ControlSource::AUTO, IntentKind::SAFETY_RECOVERY});
        Check(recovery.accepted == (context.system_mode != SystemMode::SOFTWARE_ESTOP),
              "recovery safety phase rule");
        Check(!EvaluateIntent(context, {true, ControlSource::NONE, IntentKind::SAFETY}).accepted,
              "invalid safety source rejected");
    }
}

void TestStopClearMatrix()
{
    ArbitrationContext context{};
    context.system_mode = SystemMode::MANUAL;
    context.control_source = ControlSource::SBUS;
    ControlIntent request{true, ControlSource::WEB, IntentKind::STOP_FIRE_CLEAR};
    Check(EvaluateIntent(context, request).reason == RejectReason::LOWER_PRIORITY_CONFLICT,
          "WEB cannot clear under SBUS");
    context.inputs.sbus_online = true;
    context.inputs.sbus_auto_mode = true;
    context.control_source = ControlSource::AUTO;
    Check(EvaluateIntent(context, request).accepted, "SBUS auto mode does not falsely block WEB");
    request.source = ControlSource::SBUS;
    context.control_source = ControlSource::SBUS;
    Check(EvaluateIntent(context, request).accepted, "SBUS clear accepted");
    request.source = ControlSource::WEB;
    context.control_source = ControlSource::WEB;
    for (auto severity : {FaultSeverity::INFO, FaultSeverity::WARNING, FaultSeverity::SEVERE, FaultSeverity::ESTOP}) {
        context.inputs.fault_active = true;
        context.inputs.fault_severity = severity;
        const auto result = EvaluateIntent(context, request);
        Check(result.accepted == (severity == FaultSeverity::INFO || severity == FaultSeverity::WARNING),
              "stop-clear fault matrix");
    }
    context.inputs.fault_active = false;
    context.auto_fire_state = AutoFireState::CHARGE;
    Check(EvaluateIntent(context, request).reason == RejectReason::AUTO_FLOW_ACTIVE, "active flow blocks clear");
    context.auto_fire_state = AutoFireState::AUTO_TERMINATED;
    context.system_mode = SystemMode::DEBUG;
    Check(EvaluateIntent(context, request).accepted, "debug terminal clear accepted");
    context.inputs.software_estop = true;
    Check(EvaluateIntent(context, request).reason == RejectReason::SOFTWARE_ESTOP, "estop blocks clear");
}

void TestOrdinaryIntents()
{
    for (auto mode : {SystemMode::STANDBY, SystemMode::MANUAL, SystemMode::AUTO_FIRE,
                      SystemMode::DEBUG, SystemMode::CALIBRATION}) {
        ArbitrationContext context{};
        context.system_mode = mode;
        context.control_source = ControlSource::WEB;
        Check(EvaluateIntent(context, {true, ControlSource::WEB, IntentKind::AUTO_STOP}).accepted,
              "auto_stop ordinary mode");
        Check(EvaluateIntent(context, {true, ControlSource::WEB, IntentKind::CONTROL}).accepted,
              "ordinary control");
        Check(EvaluateIntent(context, {true, ControlSource::AUTO, IntentKind::CONTROL}).reason ==
              RejectReason::LOWER_PRIORITY_CONFLICT, "priority conflict");
        Check(EvaluateIntent(context, {true, ControlSource::WEB, IntentKind::AUTO_START}).accepted ==
              (mode == SystemMode::STANDBY), "auto_start mode rule");
        context.inputs.software_estop = true;
        Check(!EvaluateIntent(context, {true, ControlSource::WEB, IntentKind::CONTROL}).accepted, "estop ordinary block");
        context.inputs.software_estop = false;
        context.inputs.fault_active = true;
        Check(!EvaluateIntent(context, {true, ControlSource::WEB, IntentKind::CONTROL}).accepted, "fault ordinary block");
    }
    ArbitrationContext uninitialized{};
    Check(EvaluateIntent(uninitialized, {true, ControlSource::WEB, IntentKind::CONTROL}).reason == RejectReason::NOT_READY,
          "uninitialized ordinary denied");
}

void TestModeMatrix()
{
    struct Edge { SystemMode from, to; TransitionReason reason; };
    const Edge edges[]{
        {SystemMode::UNINITIALIZED, SystemMode::INITIALIZING, TransitionReason::START},
        {SystemMode::INITIALIZING, SystemMode::STANDBY, TransitionReason::INIT_SUCCESS},
        {SystemMode::INITIALIZING, SystemMode::INIT_FAILED, TransitionReason::INIT_FAILURE},
        {SystemMode::STANDBY, SystemMode::MANUAL, TransitionReason::ENTER_MANUAL},
        {SystemMode::STANDBY, SystemMode::AUTO_FIRE, TransitionReason::AUTO_START},
        {SystemMode::AUTO_FIRE, SystemMode::MANUAL, TransitionReason::SBUS_TAKEOVER},
        {SystemMode::AUTO_FIRE, SystemMode::MANUAL, TransitionReason::WEB_MANUAL_TAKEOVER},
        {SystemMode::SOFTWARE_ESTOP, SystemMode::MANUAL, TransitionReason::ESTOP_CONFIRM},
        {SystemMode::FAULT, SystemMode::RECOVERY, TransitionReason::RECOVERY_START},
        {SystemMode::RECOVERY, SystemMode::MANUAL, TransitionReason::RECOVERY_COMPLETE},
        {SystemMode::STANDBY, SystemMode::DEBUG, TransitionReason::DEBUG_ENTER},
        {SystemMode::MANUAL, SystemMode::DEBUG, TransitionReason::DEBUG_ENTER},
        {SystemMode::DEBUG, SystemMode::STANDBY, TransitionReason::DEBUG_EXIT},
        {SystemMode::DEBUG, SystemMode::MANUAL, TransitionReason::DEBUG_EXIT},
        {SystemMode::STANDBY, SystemMode::CALIBRATION, TransitionReason::CALIBRATION_ENTER},
        {SystemMode::MANUAL, SystemMode::CALIBRATION, TransitionReason::CALIBRATION_ENTER},
        {SystemMode::CALIBRATION, SystemMode::STANDBY, TransitionReason::CALIBRATION_EXIT},
        {SystemMode::CALIBRATION, SystemMode::MANUAL, TransitionReason::CALIBRATION_EXIT}};
    for (int from = 0; from <= 10; ++from) for (int to = 0; to <= 10; ++to) for (int reason = 0; reason <= 22; ++reason) {
        const auto f = static_cast<SystemMode>(from), t = static_cast<SystemMode>(to);
        const auto r = static_cast<TransitionReason>(reason);
        bool expected = false;
        for (auto edge : edges) expected = expected || (edge.from == f && edge.to == t && edge.reason == r);
        if (r == TransitionReason::SOFTWARE_ESTOP_TRIGGER) expected = t == SystemMode::SOFTWARE_ESTOP && f != t;
        if (r == TransitionReason::FAULT_DETECTED) expected = t == SystemMode::FAULT &&
            f != SystemMode::FAULT && f != SystemMode::SOFTWARE_ESTOP && f != SystemMode::UNINITIALIZED &&
            f != SystemMode::INITIALIZING && f != SystemMode::INIT_FAILED;
        Check(IsModeTransitionAllowed(f, t, r) == expected, "mode transition matrix");
    }
}

void TestAutoEdgeMatrix()
{
    for (int from = 0; from <= 14; ++from) for (int to = 0; to <= 14; ++to) {
        const auto f = static_cast<AutoFireState>(from), t = static_cast<AutoFireState>(to);
        const bool first = IsAutoFireTransitionAllowed(f, t, ShotStage::FIRST);
        const bool next = IsAutoFireTransitionAllowed(f, t, ShotStage::SUBSEQUENT);
        Check(IsAutoFireTransitionAllowed(f, t) == (first || next), "auto edge overload union");
        if (f == AutoFireState::FORCE && t == AutoFireState::SHOT_1_YAW) Check(first && !next, "first force edge");
        if (f == AutoFireState::FORCE && t == AutoFireState::SHOT_N_YAW) Check(!first && next, "next force edge");
    }
}

void TestAllStateStrings()
{
    const std::array transition_reasons{
        TransitionReason::NONE, TransitionReason::START, TransitionReason::INIT_SUCCESS,
        TransitionReason::INIT_FAILURE, TransitionReason::ENTER_MANUAL, TransitionReason::AUTO_START,
        TransitionReason::SBUS_TAKEOVER, TransitionReason::WEB_MANUAL_TAKEOVER,
        TransitionReason::SOFTWARE_ESTOP_TRIGGER, TransitionReason::ESTOP_CONFIRM,
        TransitionReason::ESTOP_RELEASED, TransitionReason::FAULT_DETECTED,
        TransitionReason::RECOVERY_START, TransitionReason::RECOVERY_COMPLETE,
        TransitionReason::DEBUG_ENTER, TransitionReason::DEBUG_EXIT,
        TransitionReason::CALIBRATION_ENTER, TransitionReason::CALIBRATION_EXIT,
        TransitionReason::AUTO_FLOW_RUN, TransitionReason::AUTO_FLOW_STEP,
        TransitionReason::AUTO_FLOW_DONE, TransitionReason::AUTO_FLOW_ABORT,
        TransitionReason::CONTROL_SOURCE_REFRESH};
    for (const auto value : transition_reasons)
        Check(std::strcmp(ToString(value), "UNKNOWN") != 0, "reason string table");

    const std::array reject_reasons{
        RejectReason::NONE, RejectReason::INVALID_INTENT, RejectReason::UNKNOWN_TRANSITION,
        RejectReason::ALREADY_IN_TARGET, RejectReason::NOT_READY, RejectReason::NOT_AUTO_FIRE_MODE,
        RejectReason::AUTO_FLOW_ACTIVE, RejectReason::AUTO_FLOW_TERMINATED,
        RejectReason::AUTO_FLOW_NOT_STARTED, RejectReason::NOT_SBUS_TAKEOVER,
        RejectReason::SHOT_STAGE_MISMATCH, RejectReason::SOFTWARE_ESTOP,
        RejectReason::ESTOP_NOT_CONFIRMED, RejectReason::FAULT_ACTIVE,
        RejectReason::STOP_FIRE_SET, RejectReason::YAW_NOT_ARRIVED,
        RejectReason::LOWER_PRIORITY_CONFLICT, RejectReason::NO_CONTROL_AUTHORITY,
        RejectReason::FIRE_NOT_AUTHORIZED};
    for (const auto value : reject_reasons)
        Check(std::strcmp(ToString(value), "UNKNOWN") != 0, "reject string table");

    const std::array intent_kinds{IntentKind::CONTROL, IntentKind::FIRE, IntentKind::SAFETY,
                                  IntentKind::SAFETY_RECOVERY, IntentKind::STOP_FIRE_SET,
                                  IntentKind::STOP_FIRE_CLEAR, IntentKind::AUTO_START, IntentKind::AUTO_STOP};
    for (const auto value : intent_kinds)
        Check(std::strcmp(ToString(value), "UNKNOWN") != 0, "intent string table");

    const std::array event_kinds{EventKind::STATE_TRANSITION, EventKind::AUTO_FIRE_TRANSITION,
                                 EventKind::CONTROL_SOURCE_CHANGE, EventKind::INTENT_ACCEPTED,
                                 EventKind::INTENT_REJECTED, EventKind::FIRE_DECISION,
                                 EventKind::ESTOP_RELEASED};
    for (const auto value : event_kinds)
        Check(std::strcmp(ToString(value), "UNKNOWN") != 0, "event string table");

    const std::array shot_stages{ShotStage::FIRST, ShotStage::SUBSEQUENT, ShotStage::ANY};
    for (const auto value : shot_stages)
        Check(std::strcmp(ToString(value), "UNKNOWN") != 0, "stage string table");
    Check(std::strcmp(ToString(static_cast<TransitionReason>(255)), "UNKNOWN") == 0, "bad reason string");
    Check(std::strcmp(ToString(static_cast<RejectReason>(255)), "UNKNOWN") == 0, "bad reject string");
    Check(std::strcmp(ToString(static_cast<IntentKind>(255)), "UNKNOWN") == 0, "bad intent string");
    Check(std::strcmp(ToString(static_cast<EventKind>(255)), "UNKNOWN") == 0, "bad event string");
    Check(std::strcmp(ToString(static_cast<ShotStage>(255)), "UNKNOWN") == 0, "bad stage string");
}

void EnterStandby(StateArbitrator& authority)
{
    Check(authority.RequestModeTransition(SystemMode::INITIALIZING, TransitionReason::START).accepted, "state start");
    Check(authority.RequestModeTransition(SystemMode::STANDBY, TransitionReason::INIT_SUCCESS).accepted, "state standby");
}

void TestResetAndUpdate()
{
    StateArbitrator authority;
    authority.SetFault(true, FaultSeverity::WARNING);
    authority.SetYawArrived(true);
    authority.SetStopFire(false);
    ExternalInputs inputs{};
    inputs.software_estop = true;
    inputs.web_online = true;
    authority.UpdateInputs(inputs);
    inputs.software_estop = false;
    authority.UpdateInputs(inputs);
    Check(authority.is_estop_latched() && authority.inputs().software_estop, "bulk update preserves latch");
    Check(authority.TriggerSoftwareEstop().reason == RejectReason::ALREADY_IN_TARGET, "duplicate estop trigger");
    Check(!authority.ReleaseSoftwareEstopLatch(), "release before confirmation rejected");
    authority.Reset(123);
    Check(!authority.is_estop_latched() && authority.inputs().timestamp_us == 123 &&
          authority.event_count() == 0, "reset clears state");
    Check(authority.TriggerSoftwareEstop().accepted, "explicit estop trigger");
    authority.Reset();
    EnterStandby(authority);
    Check(authority.RequestModeTransition(SystemMode::STANDBY, TransitionReason::INIT_SUCCESS).reason ==
          RejectReason::ALREADY_IN_TARGET, "same mode result");
    Check(authority.RequestModeTransition(SystemMode::INITIALIZING, TransitionReason::START).reason ==
          RejectReason::UNKNOWN_TRANSITION, "invalid mode result");
}

void TestEventBuffer()
{
    StateArbitrator authority;
    authority.SetClock(999, 1);
    for (unsigned i = 1; i <= 70; ++i)
        authority.SubmitIntent({false, ControlSource::WEB, IntentKind::CONTROL, i, i == 1 ? 88u : 0u});
    Check(authority.event_count() == 64 && authority.dropped_event_count() == 6, "exact event overflow");
    std::array<StateEvent, 64> output{};
    Check(authority.CopyEvents(output.data(), output.size()) == 64, "full ring copy");
    Check(output.front().request_id == 7 && output.back().request_id == 70 &&
          output.back().timestamp_us == 999, "event copy oldest newest");
    Check(authority.CopyEvents(output.data(), 1) == 1 && output[0].request_id == 7, "short event copy");
    authority.ClearEvents();
    Check(authority.event_count() == 0 && authority.dropped_event_count() == 0, "event clear");
}

void TestFourShotRun()
{
    StateArbitrator authority;
    EnterStandby(authority);
    Check(authority.RequestModeTransition(SystemMode::AUTO_FIRE, TransitionReason::AUTO_START).accepted, "auto mode");
    Check(authority.BeginNewAutoFireRun().accepted, "new auto run");
    for (unsigned shot = 1; shot <= 4; ++shot) {
        Check(authority.RequestAutoFireState(AutoFireState::CHARGE).accepted, "charge");
        if (shot > 1) for (auto state : {AutoFireState::RELOAD_LIFT, AutoFireState::FEED,
                                         AutoFireState::FEED_WAIT, AutoFireState::RELOAD_LOWER})
            Check(authority.RequestAutoFireState(state).accepted, "reload path");
        Check(authority.RequestAutoFireState(AutoFireState::FORCE).accepted, "force");
        Check(authority.RequestAutoFireState(shot == 1 ? AutoFireState::SHOT_1_YAW : AutoFireState::SHOT_N_YAW).accepted,
              "yaw stage");
        Check(authority.RequestAutoFireState(AutoFireState::FIRE_READY).accepted, "fire ready");
        authority.SetStopFire(false);
        authority.SetYawArrived(true);
        Check(authority.RequestAutoFireState(AutoFireState::FIRING).accepted, "firing");
        Check(authority.RequestAutoFireState(AutoFireState::FIRE_COMPLETED).accepted, "fire complete");
        Check(authority.current_shot() == shot, "shot counter");
    }
    Check(authority.RequestAutoFireState(AutoFireState::CHARGE).reason == RejectReason::SHOT_STAGE_MISMATCH,
          "fifth shot rejected");
    Check(authority.RequestAutoFireState(AutoFireState::AUTO_COMPLETED).accepted, "auto terminal");
    Check(authority.RequestAutoFireState(AutoFireState::CHARGE).reason == RejectReason::AUTO_FLOW_TERMINATED,
          "completed flow cannot resume");
    Check(authority.BeginNewAutoFireRun().accepted && authority.run_id() == 2 && authority.current_shot() == 1,
          "new run has new identity");
}

void TestAutoRejections()
{
    StateArbitrator authority;
    Check(authority.BeginNewAutoFireRun().reason == RejectReason::NOT_AUTO_FIRE_MODE, "new run mode rejection");
    Check(authority.TerminateAutoFireOnSbusTakeover().reason == RejectReason::AUTO_FLOW_NOT_STARTED, "idle takeover");
    Check(authority.RequestAutoFireState(AutoFireState::AUTO_TERMINATED).reason == RejectReason::ALREADY_IN_TARGET,
          "idle stop idempotent");
    EnterStandby(authority);
    authority.RequestModeTransition(SystemMode::AUTO_FIRE, TransitionReason::AUTO_START);
    authority.BeginNewAutoFireRun();
    Check(authority.RequestAutoFireState(AutoFireState::AUTO_PREPARE).reason == RejectReason::ALREADY_IN_TARGET,
          "same auto state");
    Check(authority.RequestAutoFireState(AutoFireState::FEED).reason == RejectReason::UNKNOWN_TRANSITION,
          "illegal auto state");
    for (auto state : {AutoFireState::CHARGE, AutoFireState::FORCE, AutoFireState::SHOT_1_YAW,
                       AutoFireState::FIRE_READY}) authority.RequestAutoFireState(state);
    Check(authority.RequestAutoFireState(AutoFireState::FIRING).reason == RejectReason::STOP_FIRE_SET,
          "stop flag blocks firing");
    authority.SetStopFire(false);
    Check(authority.RequestAutoFireState(AutoFireState::FIRING).reason == RejectReason::YAW_NOT_ARRIVED,
          "yaw blocks firing");
    Check(authority.RequestAutoFireState(AutoFireState::AUTO_TERMINATED).accepted, "active auto abort");
    Check(authority.TerminateAutoFireOnSbusTakeover().reason == RejectReason::ALREADY_IN_TARGET,
          "terminated takeover idempotent");
}

void TestManualFireModes()
{
    StateArbitrator authority;
    EnterStandby(authority);
    authority.SetYawArrived(true);
    authority.SetStopFire(false);
    Check(!authority.AuthorizeManualFire(ControlSource::SBUS, ControlSource::AUTO).allowed,
          "standby manual fire denied");
    authority.RequestModeTransition(SystemMode::MANUAL, TransitionReason::ENTER_MANUAL);
    Check(authority.AuthorizeManualFire(ControlSource::SBUS, ControlSource::AUTO).allowed, "manual fire");
    Check(!authority.AuthorizeManualFire(ControlSource::AUTO, ControlSource::AUTO).allowed, "auto manual fire denied");
    authority.RequestModeTransition(SystemMode::DEBUG, TransitionReason::DEBUG_ENTER);
    Check(authority.AuthorizeManualFire(ControlSource::WEB).allowed, "debug manual fire");
    authority.RequestModeTransition(SystemMode::MANUAL, TransitionReason::DEBUG_EXIT);
}

void RegisterAppModule(AppFixture& fixture, int& initializes, int& stops, bool success = true,
                       std::function<void()> initialize_hook = {})
{
    Check(fixture.runtime.addModule({RuntimeStep::InitializeSocketCan, true,
          std::make_unique<TestLifecycle>("test", success, initializes, stops, std::move(initialize_hook))}),
          "app module registration");
}

void TestAppStartupRejections()
{
    AppFixture empty;
    ModuleInitResult info;
    Check(empty.app.Start(&info) == AppCoordinator::kRuntimeStartFailed && !info.success,
          "empty registry refusal");
    Check(empty.app.Start() == AppCoordinator::kRestartNotSupported, "failed attempt cannot restart");
    AppFixture closed;
    closed.app.Shutdown();
    Check(closed.app.Start() == AppCoordinator::kRestartNotSupported, "shutdown before start");
    AppFixture prestarted;
    prestarted.runtime.start();
    Check(prestarted.app.Start() == AppCoordinator::kRestartNotSupported, "prestarted runtime refused");
}

void TestAppStartupFailures()
{
    {
        int initializes = 0, stops = 0;
        AppFixture required;
        RegisterAppModule(required, initializes, stops, false);
        ModuleInitResult info;
        Check(required.app.Start(&info) == AppCoordinator::kRuntimeStartFailed &&
              info.module_name == "test" && info.error_message == "module rejected", "required failure detail");
        {
            const auto view = required.app.ReadState();
            Check(view.context().system_mode == SystemMode::INIT_FAILED, "failed state mode");
        }
        Check(!required.app.CanRunActuators(), "failed state gate");
    }

    {
        int initializes = 0, stops = 0;
        AppFixture optional;
        Check(optional.runtime.addModule({RuntimeStep::CreateDiagnostics, false,
              std::make_unique<TestLifecycle>("optional", false, initializes, stops)}), "optional module");
        Check(optional.app.Start() == AppCoordinator::kOk, "optional failure does not block");
        Check(optional.app.startup().steps.size() == 1 && !optional.app.startup().steps.front().result.success,
              "optional failure remains observable");
    }
}

void TestAppStartupInterrupt()
{
    int initializes = 0, stops = 0;
    AppFixture fixture;
    RegisterAppModule(fixture, initializes, stops, true, [&] { fixture.app.ReportSoftwareEstop(true); });
    ModuleInitResult info;
    Check(fixture.app.Start(&info) == AppCoordinator::kPreconditionFailed, "startup transition refusal");
    Check(stops == 1 && !fixture.app.hardware_ready() && fixture.app.RunGate() == RunGateReason::ESTOP_LATCHED,
          "startup interruption releases hardware and preserves estop");
}

void TestAppGateModes()
{
    int initializes = 0, stops = 0;
    AppFixture fixture;
    Check(fixture.app.RunGate() == RunGateReason::HARDWARE_NOT_READY, "not ready gate");
    RegisterAppModule(fixture, initializes, stops);
    Check(fixture.app.Start() == AppCoordinator::kOk && fixture.app.CanRunActuators(), "ready gate");
    fixture.app.ReportFault(true, FaultSeverity::SEVERE);
    Check(fixture.app.RunGate() == RunGateReason::FAULT_ACTIVE, "fault gate");
    fixture.app.ReportSoftwareEstop(true);
    Check(fixture.app.RunGate() == RunGateReason::ESTOP_LATCHED, "estop dominates fault");
    fixture.app.Shutdown();
    Check(fixture.app.RunGate() == RunGateReason::ESTOP_LATCHED &&
          fixture.app.Start() == AppCoordinator::kRestartNotSupported, "shutdown gate");

    AppFixture ordinary_shutdown;
    int ordinary_initializes = 0, ordinary_stops = 0;
    RegisterAppModule(ordinary_shutdown, ordinary_initializes, ordinary_stops);
    Check(ordinary_shutdown.app.Start() == AppCoordinator::kOk, "ordinary shutdown setup");
    ordinary_shutdown.app.Shutdown();
    Check(ordinary_shutdown.app.RunGate() == RunGateReason::HARDWARE_NOT_READY,
          "ordinary shutdown closes hardware gate");
}

void TestAppAllPorts()
{
    AppFixture fixture;
    fixture.EnterManual();
    IInputReporter& input = fixture.app;
    IControlAuthority& control = fixture.app;
    ISafetyStateControl& safety = fixture.app;
    IStateReader& reader = fixture.app;
    input.ReportYawArrived(true);
    input.ReportStopFire(false);
    input.ReportFault(false, FaultSeverity::INFO);
    input.ReportClock();
    Check(control.AuthorizeManualFire(ControlSource::SBUS).allowed, "manual auth port");
    Check(control.AuthorizeFire(ControlSource::SBUS).allowed, "fire auth port");
    Check(control.SubmitIntent({true, ControlSource::SBUS, IntentKind::CONTROL}).accepted, "intent port");
    Check(control.BeginNewAutoFireRun().reason == RejectReason::NOT_AUTO_FIRE_MODE, "run port rejection");
    Check(control.TerminateAutoFireOnSbusTakeover().reason == RejectReason::AUTO_FLOW_NOT_STARTED, "takeover port");
    Check(!safety.ReleaseSoftwareEstopLatch(), "release absent latch");
    input.ReportSoftwareEstop(true);
    Check(safety.ConfirmSoftwareEstop().accepted && safety.ReleaseSoftwareEstopLatch(), "estop ports");
    const auto view = reader.ReadState();
    Check(!view.estop_latched() && view.run_id() == 0 && view.current_shot() == 0, "read metadata");
}

void TestAppStopFlagCommit()
{
    AppFixture fixture;
    fixture.EnterManual();
    fixture.app.ReportWebOnline(true);
    const ControlIntent clear{true, ControlSource::WEB, IntentKind::STOP_FIRE_CLEAR};
    Check(fixture.app.SubmitIntent(clear).accepted && fixture.app.lastInputs().stop_fire,
          "authorization alone does not commit");
    fixture.app.ReportSbus(true, false);
    Check(!fixture.app.SubmitStopFireIntent(clear) .accepted && fixture.app.lastInputs().stop_fire,
          "rejected clear does not commit");
    fixture.app.ReportSbus(true, true);
    Check(fixture.app.SubmitStopFireIntent(clear).accepted && !fixture.app.lastInputs().stop_fire,
          "accepted clear commits");
    Check(fixture.app.SubmitStopFireIntent({true, ControlSource::WEB, IntentKind::STOP_FIRE_SET}).accepted &&
          fixture.app.lastInputs().stop_fire, "set commits");
    fixture.app.ReportFault(true, FaultSeverity::SEVERE);
    Check(!fixture.app.SubmitStopFireIntent(clear).accepted && fixture.app.lastInputs().stop_fire,
          "fault clear does not commit");
}

void TestAppReadViewMove()
{
    AppFixture fixture;
    auto source = fixture.app.ReadState();
    auto destination = std::move(source);
    Check(&destination.context() == &fixture.authority.context() &&
          destination.run_gate() == RunGateReason::HARDWARE_NOT_READY, "moved read view");
}

void TestAppClockAndEvents()
{
    AppFixture fixture;
    fixture.clock.value = -1;
    fixture.app.ReportClock();
    Check(fixture.app.lastInputs().timestamp_us == 0, "negative clock bounded");
    fixture.clock.value = 55;
    fixture.app.ReportSbus(true, false);
    std::array<StateEvent, StateArbitrator::kMaxEvents> events{};
    Check(fixture.app.CopyEvents(nullptr, 1) == 0 && fixture.app.CopyEvents(events.data(), 0) == 0,
          "invalid event copy");
    for (int i = 0; i < 100; ++i) fixture.app.SubmitIntent({false, ControlSource::WEB, IntentKind::CONTROL});
    const auto copied = fixture.app.CopyEvents(events.data(), events.size());
    Check(fixture.app.dropped_event_count() > 0 && copied == events.size() && events.back().timestamp_us == 55,
          "app event history");
    auto inputs = fixture.app.lastInputs();
    inputs.sbus_online = false;
    Check(fixture.app.lastInputs().sbus_online && !inputs.sbus_online, "input copy is independent");
}
