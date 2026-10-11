#include "atom03_SBUS/lifecycle_adapter.hpp"

namespace marshland::atom03 {

marshland::runtime::ModuleInitResult LifecycleAdapter::initialize(
    const marshland::runtime::RuntimeContext&) {
    marshland::runtime::ModuleInitResult result{};
    result.module_name = name();
    SbusConfig config{};
    const auto loaded = LoadSbusConfig(config_, config);
    if (!loaded.success) {
        result.error_message = loaded.error_message;
        return result;
    }
    if (!core_.start(config)) {
        result.error_message = "UART10 SBUS worker failed to start";
        return result;
    }
    result.success = true;
    return result;
}

void LifecycleAdapter::shutdown() noexcept
{
    core_.stop();
}

} // namespace marshland::atom03
