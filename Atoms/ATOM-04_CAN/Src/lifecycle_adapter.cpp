#include "atom04_can/lifecycle_adapter.hpp"

#include <string>

namespace marshland::atom04 {

runtime::ModuleInitResult LifecycleAdapter::initialize(const runtime::RuntimeContext& context)
{
    runtime::ModuleInitResult result{};
    result.module_name = name();
    (void)context;  // 配置一律经 IConfigView 查询，不自解析 JSON。

    CanRuntimeConfig config{};
    const config::ConfigStatus status = loadCanRuntimeConfig(config_, config);
    if (status != config::ConfigStatus::OK) {
        result.success = false;
        result.error_message = "CAN config unusable: ";
        result.error_message += config::ToString(status);
        return result;
    }

    std::string detail;
    if (!core_.start(config, detail)) {
        result.success = false;
        result.error_message = detail.empty() ? "CanCore::start failed" : detail;
        return result;
    }

    result.success = true;
    return result;
}

void LifecycleAdapter::shutdown() noexcept
{
    core_.stop();
}

} // namespace marshland::atom04
