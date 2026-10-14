#include "atom07_servo_driver/lifecycle_adapter.hpp"

namespace marshland::atom07 {

marshland::runtime::ModuleInitResult LifecycleAdapter::initialize(
    const marshland::runtime::RuntimeContext&)
{
    marshland::runtime::ModuleInitResult result{};
    result.module_name = name();

    ServoConfig config{};
    const ServoConfigLoadResult loaded = LoadServoConfig(config_, config);
    if (!loaded.success) {
        result.error_message = "config '";
        result.error_message += (loaded.failed_key != nullptr) ? loaded.failed_key : "servo";
        result.error_message += "' unusable: ";
        result.error_message += marshland::config::ToString(loaded.status);
        return result;
    }

    if (!core_.start(config)) {
        result.error_message =
            "PCA9685A/IIC servo driver failed to start (IIC init or IIC worker thread)";
        return result;
    }

    result.success = true;
    return result;
}

void LifecycleAdapter::shutdown() noexcept
{
    core_.stop();
}

} // namespace marshland::atom07
