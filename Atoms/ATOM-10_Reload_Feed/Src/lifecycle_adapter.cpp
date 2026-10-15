#include "atom10_reload_feed/lifecycle_adapter.hpp"

#include "atom10_reload_feed/reload_feed_config.hpp"

namespace marshland::atom10 {

ReloadFeedLifecycleAdapter::ReloadFeedLifecycleAdapter(
    ReloadFeedCore& core, const config::IConfigView& config_view) noexcept
    : core_(core)
    , config_view_(config_view)
{
}

runtime::ModuleInitResult ReloadFeedLifecycleAdapter::initialize(
    const runtime::RuntimeContext& /*context*/)
{
    runtime::ModuleInitResult result{};
    result.module_name = name();
    ReloadFeedConfig config;
    const char* failed_key = nullptr;
    config::ConfigStatus failed_status = config::ConfigStatus::OK;
    if (!LoadReloadFeedConfig(config_view_, config, failed_key, failed_status)) {
        result.error_message = "config key '";
        result.error_message += (failed_key != nullptr) ? failed_key : "?";
        result.error_message += "' status=";
        result.error_message += config::ToString(failed_status);
        return result;
    }
    core_.updateConfig(config);
    if (!core_.initializeOutputs()) {
        result.error_message = "failed to submit initial 0 rpm targets to both reload motors";
        return result;
    }
    initialized_ = true;
    result.success = true;
    return result;
}

void ReloadFeedLifecycleAdapter::shutdown() noexcept
{
    if (initialized_) {
        core_.shutdown();
        initialized_ = false;
    }
}

} // namespace marshland::atom10
