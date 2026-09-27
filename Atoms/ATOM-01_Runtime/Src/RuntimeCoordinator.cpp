#include "RuntimeCoordinator.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace marshland::runtime {

namespace {

const char* unknownModuleName() noexcept
{
    return "unknown-module";
}

const char* exceptionMessage(const std::exception& exception) noexcept
{
    return exception.what() != nullptr ? exception.what() : "module initialization threw an exception";
}

} // namespace

const char* toString(RuntimeState state) noexcept
{
    switch (state) {
    case RuntimeState::Created: return "CREATED";
    case RuntimeState::Starting: return "STARTING";
    case RuntimeState::Standby: return "STANDBY";
    case RuntimeState::InitFailed: return "INIT_FAILED";
    case RuntimeState::ShuttingDown: return "SHUTTING_DOWN";
    case RuntimeState::Stopped: return "STOPPED";
    }
    return "UNKNOWN";
}

const char* toString(RuntimeStep step) noexcept
{
    switch (step) {
    case RuntimeStep::CreateDiagnostics: return "create-diagnostics";
    case RuntimeStep::LoadConfiguration: return "load-configuration";
    case RuntimeStep::ValidateConfiguration: return "validate-configuration";
    case RuntimeStep::LoadPidParameters: return "load-pid-parameters";
    case RuntimeStep::InitializeSocketCan: return "initialize-socketcan";
    case RuntimeStep::InitializeMotorAndEncoderChannels: return "initialize-motor-encoder-channels";
    case RuntimeStep::InitializeGpioAndLimitInputs: return "initialize-gpio-limit-inputs";
    case RuntimeStep::InitializeIicPca9685: return "initialize-iic-pca9685";
    case RuntimeStep::InitializeServoOutputs: return "initialize-servo-outputs";
    case RuntimeStep::InitializeSbusAndVision: return "initialize-sbus-vision";
    case RuntimeStep::StartHttpAndWebSocket: return "start-http-websocket";
    }
    return "unknown-step";
}

bool RuntimeStartResult::succeeded() const noexcept
{
    return state == RuntimeState::Standby;
}

RuntimeCoordinator::RuntimeCoordinator(RuntimeContext context)
    : context_(std::move(context))
{
}

RuntimeCoordinator::~RuntimeCoordinator()
{
    shutdown();
}

bool RuntimeCoordinator::addModule(ModuleRegistration registration)
{
    if (state_ != RuntimeState::Created || registration.module == nullptr) {
        return false;
    }

    for (const auto& existing : modules_) {
        if (existing.step == registration.step) {
            return false;
        }
    }

    RegisteredModule candidate{
        registration.step,
        registration.required,
        std::move(registration.module),
        false,
    };

    const auto insertion_point = std::find_if(
        modules_.begin(),
        modules_.end(),
        [&candidate](const RegisteredModule& existing) {
            return static_cast<int>(existing.step) > static_cast<int>(candidate.step);
        });
    modules_.insert(insertion_point, std::move(candidate));
    return true;
}

RuntimeStartResult RuntimeCoordinator::start()
{
    if (state_ != RuntimeState::Created) {
        return RuntimeStartResult{state_, step_results_};
    }

    state_ = RuntimeState::Starting;
    step_results_.clear();
    step_results_.reserve(modules_.size());

    for (auto& registered : modules_) {
        StepResult step_result{};
        step_result.step = registered.step;
        step_result.required = registered.required;
        step_result.attempted = true;

        try {
            step_result.result = registered.module->initialize(context_);
            if (step_result.result.module_name.empty()) {
                const char* module_name = registered.module->name();
                step_result.result.module_name = module_name != nullptr
                    ? module_name
                    : unknownModuleName();
            }
        } catch (const std::exception& exception) {
            step_result.result = exceptionResult(registered.module->name(), exceptionMessage(exception));
        } catch (...) {
            step_result.result = exceptionResult(
                registered.module->name(),
                "module initialization threw an unknown exception");
        }

        registered.started = step_result.result.success;
        step_results_.push_back(step_result);

        if (registered.required && !step_result.result.success) {
            failStart();
            return RuntimeStartResult{state_, step_results_};
        }
    }

    state_ = RuntimeState::Standby;
    return RuntimeStartResult{state_, step_results_};
}

void RuntimeCoordinator::shutdown() noexcept
{
    if (state_ == RuntimeState::Stopped || state_ == RuntimeState::Created) {
        if (state_ == RuntimeState::Created) {
            state_ = RuntimeState::Stopped;
        }
        return;
    }

    state_ = RuntimeState::ShuttingDown;
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        if (!it->started || it->module == nullptr) {
            continue;
        }
        try {
            it->module->shutdown();
        } catch (...) {
            // shutdown() is a best-effort, noexcept lifecycle boundary. A
            // later diagnostic layer may report failures from the adapter.
        }
        it->started = false;
    }
    state_ = RuntimeState::Stopped;
}

RuntimeState RuntimeCoordinator::state() const noexcept
{
    return state_;
}

bool RuntimeCoordinator::canExecuteActuatorCommands() const noexcept
{
    return state_ == RuntimeState::Standby;
}

const std::vector<StepResult>& RuntimeCoordinator::stepResults() const noexcept
{
    return step_results_;
}

const RuntimeContext& RuntimeCoordinator::context() const noexcept
{
    return context_;
}

ModuleInitResult RuntimeCoordinator::exceptionResult(const char* module_name,
                                                     const char* message) noexcept
{
    ModuleInitResult result{};
    result.success = false;
    result.module_name = module_name != nullptr ? module_name : unknownModuleName();
    result.error_message = message != nullptr ? message : "module initialization failed";
    return result;
}

void RuntimeCoordinator::failStart() noexcept
{
    state_ = RuntimeState::InitFailed;
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        if (!it->started || it->module == nullptr) {
            continue;
        }
        try {
            it->module->shutdown();
        } catch (...) {
            // Preserve INIT_FAILED and continue releasing other modules.
        }
        it->started = false;
    }
}

} // namespace marshland::runtime


