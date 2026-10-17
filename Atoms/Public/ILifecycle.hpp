#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace marshland::runtime {

// Hardware/service lifecycle state, distinct from the business SystemMode.
enum class RuntimeState {
    Created,
    Starting,
    Standby,
    InitFailed,
    ShuttingDown,
    Stopped,
};

// Declaration order is the initialization order for hardware/service modules.
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

inline const char* toString(RuntimeState state) noexcept
{
    switch (state) {
    case RuntimeState::Created:      return "CREATED";
    case RuntimeState::Starting:     return "STARTING";
    case RuntimeState::Standby:      return "STANDBY";
    case RuntimeState::InitFailed:   return "INIT_FAILED";
    case RuntimeState::ShuttingDown: return "SHUTTING_DOWN";
    case RuntimeState::Stopped:      return "STOPPED";
    }
    return "UNKNOWN";
}

inline const char* toString(RuntimeStep step) noexcept
{
    switch (step) {
    case RuntimeStep::CreateDiagnostics:                return "create-diagnostics";
    case RuntimeStep::LoadConfiguration:                return "load-configuration";
    case RuntimeStep::ValidateConfiguration:            return "validate-configuration";
    case RuntimeStep::LoadPidParameters:                return "load-pid-parameters";
    case RuntimeStep::InitializeSocketCan:              return "initialize-socketcan";
    case RuntimeStep::InitializeMotorAndEncoderChannels:return "initialize-motor-encoder-channels";
    case RuntimeStep::InitializeGpioAndLimitInputs:     return "initialize-gpio-limit-inputs";
    case RuntimeStep::InitializeIicPca9685:             return "initialize-iic-pca9685";
    case RuntimeStep::InitializeServoOutputs:           return "initialize-servo-outputs";
    case RuntimeStep::InitializeSbusAndVision:          return "initialize-sbus-vision";
    case RuntimeStep::StartHttpAndWebSocket:            return "start-http-websocket";
    }
    return "unknown-step";
}

// The coordinator passes configuration through without parsing it.
struct RuntimeContext {
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

// Common interface for modules managed by RuntimeCoordinator.
class ILifecycleModule {
public:
    virtual ~ILifecycleModule() = default;

    virtual ModuleInitResult initialize(const RuntimeContext& context) = 0;
    virtual void shutdown() noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

} // namespace marshland::runtime
