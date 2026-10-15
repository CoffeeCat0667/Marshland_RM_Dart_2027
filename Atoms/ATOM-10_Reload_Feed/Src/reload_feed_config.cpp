#include "atom10_reload_feed/reload_feed_config.hpp"

namespace marshland::atom10 {
namespace {

bool readInt(const config::IConfigView& view, const char* key, int& out,
             const char*& failed_key, config::ConfigStatus& failed_status) noexcept
{
    std::int64_t value = 0;
    const config::ConfigStatus status = view.getInt(key, value);
    if (status != config::ConfigStatus::OK) {
        failed_key = key;
        failed_status = status;
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool checkRange(bool valid, const char* key,
                const char*& failed_key, config::ConfigStatus& failed_status) noexcept
{
    if (!valid) {
        failed_key = key;
        failed_status = config::ConfigStatus::OUT_OF_RANGE;
        return false;
    }
    return true;
}

} // namespace

bool LoadReloadFeedConfig(const config::IConfigView& view,
                          ReloadFeedConfig& out,
                          const char*& failed_key,
                          config::ConfigStatus& failed_status) noexcept
{
    ReloadFeedConfig parsed;

    if (!readInt(view, "reload.speed_rpm", parsed.speed_rpm, failed_key, failed_status))
        return false;
    if (!readInt(view, "timing.reload_lift_run_s", parsed.lift_run_s, failed_key, failed_status))
        return false;
    if (!readInt(view, "timing.reload_lower_run_s", parsed.lower_run_s, failed_key, failed_status))
        return false;
    if (!readInt(view, "feed.hold_ms", parsed.feed_hold_ms, failed_key, failed_status))
        return false;
    if (!readInt(view, "timing.feed_max_action_ms", parsed.feed_max_action_ms, failed_key, failed_status))
        return false;

    if (!checkRange(parsed.speed_rpm > 0, "reload.speed_rpm", failed_key, failed_status))
        return false;
    if (!checkRange(parsed.lift_run_s > 0, "timing.reload_lift_run_s", failed_key, failed_status))
        return false;
    if (!checkRange(parsed.lower_run_s > 0, "timing.reload_lower_run_s", failed_key, failed_status))
        return false;
    if (!checkRange(parsed.feed_hold_ms >= 0, "feed.hold_ms", failed_key, failed_status))
        return false;
    if (!checkRange(parsed.feed_max_action_ms > 0, "timing.feed_max_action_ms", failed_key, failed_status))
        return false;

    out = parsed;
    return true;
}

} // namespace marshland::atom10
