#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace marshland::runtime {

enum class RuntimeState {
    Created,
    Starting,
    Standby,
    InitFailed,
    ShuttingDown,
    Stopped,
};

enum class RuntimeStep {
    CreateDiagnostics,
    LoadConfiguration,
    ValidateConfiguration,
    LoadPidParameters,
    InitializeSocketCan,
    InitializeMotorAndEncoderChannels,
    InitializeGpioAndLimitInputs,
    InitializeIicPca9685,
    InitializeServoOutputs,
    InitializeSbusAndVision,
    StartHttpAndWebSocket,
};

const char* toString(RuntimeState state) noexcept;
const char* toString(RuntimeStep step) noexcept;

struct RuntimeContext {
    // The coordinator deliberately does not parse configuration. Adapters may
    // use this path and the opaque values supplied by the owning application.
    std::string configuration_path;
    std::unordered_map<std::string, std::string> values;
};

struct ModuleInitResult {
    bool success{false};
    std::string module_name;
    std::string error_message;
};

struct StepResult {
    RuntimeStep step{RuntimeStep::CreateDiagnostics};
    bool required{true};
    bool attempted{false};
    ModuleInitResult result{};
};

struct RuntimeStartResult {
    RuntimeState state{RuntimeState::Created};
    std::vector<StepResult> steps;

    bool succeeded() const noexcept;
};

class ILifecycleModule {
public:
    virtual ~ILifecycleModule() = default;

    virtual ModuleInitResult initialize(const RuntimeContext& context) = 0;
    virtual void shutdown() noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

struct ModuleRegistration {
    RuntimeStep step;
    bool required;
    std::unique_ptr<ILifecycleModule> module;
};

class RuntimeCoordinator final {
public:
    explicit RuntimeCoordinator(RuntimeContext context = {});
    ~RuntimeCoordinator();

    RuntimeCoordinator(const RuntimeCoordinator&) = delete;
    RuntimeCoordinator& operator=(const RuntimeCoordinator&) = delete;
    RuntimeCoordinator(RuntimeCoordinator&&) = delete;
    RuntimeCoordinator& operator=(RuntimeCoordinator&&) = delete;

    bool addModule(ModuleRegistration registration);
    RuntimeStartResult start();
    void shutdown() noexcept;

    RuntimeState state() const noexcept;
    bool canExecuteActuatorCommands() const noexcept;
    const std::vector<StepResult>& stepResults() const noexcept;
    const RuntimeContext& context() const noexcept;

private:
    struct RegisteredModule {
        RuntimeStep step;
        bool required;
        std::unique_ptr<ILifecycleModule> module;
        bool started{false};
    };

    static ModuleInitResult exceptionResult(const char* module_name,
                                            const char* message) noexcept;
    void failStart() noexcept;

    RuntimeContext context_;
    RuntimeState state_{RuntimeState::Created};
    std::vector<RegisteredModule> modules_;
    std::vector<StepResult> step_results_;
};

} // namespace marshland::runtime
