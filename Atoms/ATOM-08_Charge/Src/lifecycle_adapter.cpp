#include "atom08_charge/lifecycle_adapter.hpp"

namespace marshland::atom08 {

marshland::runtime::ModuleInitResult LifecycleAdapter::initialize(
    const marshland::runtime::RuntimeContext&) {
    marshland::runtime::ModuleInitResult result{};
    result.module_name = name();

    ChargeConfig config{};
    const ChargeConfigLoadResult loaded = LoadChargeConfig(config_, config);
    if (!loaded.success) {
        result.error_message = "charge configuration unusable: ";
        result.error_message += loaded.error_message;
        return result;
    }
    if (!core_.configure(config)) {
        result.error_message = "charge configuration rejected by controller";
        return result;
    }
    result.success = true;
    return result;
}

void LifecycleAdapter::shutdown() noexcept
{
    core_.reset();
}

} // namespace marshland::atom08
