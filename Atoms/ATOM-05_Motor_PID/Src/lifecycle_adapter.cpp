#include "atom05_motor_pid/lifecycle_adapter.hpp"

#include <string>

namespace marshland::atom05 {

const char* LifecycleAdapter::name() const noexcept
{
    return stage_ == LifecycleStage::LOAD_PID ? "atom-05-load-pid" : "atom-05-motor-channels";
}

runtime::ModuleInitResult LifecycleAdapter::initialize(const runtime::RuntimeContext&)
{
    runtime::ModuleInitResult result{};
    result.module_name = name();
    MotorStatus status = MotorStatus::INVALID_CONFIG;
    if (stage_ == LifecycleStage::LOAD_PID) {
        MotorRuntimeConfig loaded{};
        const auto read = LoadMotorRuntimeConfig(config_, loaded);
        if (!read.success) {
            result.error_message = "motor config unusable: ";
            result.error_message += read.key;
            result.error_message += " [index=" + std::to_string(read.motor_index) + "] ";
            result.error_message += config::ToString(read.status);
            return result;
        }
        status = core_.configure(loaded);
    } else if (stage_ == LifecycleStage::INITIALIZE_CHANNELS) {
        if (!config_.isLoaded() || config_.configVersion() != core_.configurationVersion()) {
            result.error_message = "configuration changed between PID loading and channel startup";
            return result;
        }
        status = core_.start();
    }
    result.success = status == MotorStatus::OK;
    if (!result.success) {
        result.error_message = "motor lifecycle operation failed, status=";
        result.error_message += std::to_string(static_cast<unsigned>(status));
    }
    return result;
}

void LifecycleAdapter::shutdown() noexcept
{
    core_.stop();
}

} // namespace marshland::atom05
