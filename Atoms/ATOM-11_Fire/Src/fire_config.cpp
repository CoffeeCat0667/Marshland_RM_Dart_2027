// ============================================================================
// fire_config.cpp — ATOM-11 配置加载实现
//
// 定位方式：servo.channels 数组按 mechanism == "FIRE" 定位槽位（发射舵机
// 无 id，禁止硬编码下标；id 为 null 时 getIntAt 返回 INVALID_TYPE，本模块
// 不读取 id）。任何键查询非 OK 或取值越界即失败，不臆造默认值。
// ============================================================================
#include "atom11_fire/fire_config.hpp"

#include <string>

namespace marshland::atom11 {
namespace {

constexpr const char* kKeyChannels = "servo.channels";
constexpr const char* kKeyHoldMs = "timing.fire_angle_hold_ms";
constexpr const char* kKeyMaxActionMs = "timing.fire_max_action_ms";
constexpr const char* kMechanismFire = "FIRE";

constexpr std::int64_t kAngleMin = 0;
constexpr std::int64_t kAngleMax = 180;

FireConfigLoadResult fail(const char* key, marshland::config::ConfigStatus status)
{
    FireConfigLoadResult result{};
    result.error_message = std::string(key) + ": config query failed (" +
                           marshland::config::ToString(status) + ")";
    return result;
}

FireConfigLoadResult failRange(const char* key, std::int64_t value)
{
    FireConfigLoadResult result{};
    result.error_message = std::string(key) + ": value " + std::to_string(value) +
                           " out of range";
    return result;
}

} // namespace

FireConfigLoadResult LoadFireConfig(const marshland::config::IConfigView& view,
                                    FireConfig& out)
{
    using marshland::config::ConfigStatus;

    std::uint64_t channel_count = 0;
    const ConfigStatus size_status = view.getArraySize(kKeyChannels, channel_count);
    if (size_status != ConfigStatus::OK) {
        return fail(kKeyChannels, size_status);
    }

    bool fire_slot_found = false;
    std::int64_t standby_deg = 0;
    std::int64_t action_deg = 0;
    for (std::uint64_t index = 0; index < channel_count; ++index) {
        const char* mechanism_data = nullptr;
        std::uint64_t mechanism_size = 0;
        const ConfigStatus mechanism_status = view.getStringAt(
            kKeyChannels, index, "mechanism", mechanism_data, mechanism_size);
        if (mechanism_status != ConfigStatus::OK || mechanism_data == nullptr) {
            continue;
        }
        const std::string mechanism(mechanism_data,
                                    static_cast<std::size_t>(mechanism_size));
        if (mechanism != kMechanismFire) {
            continue;
        }
        fire_slot_found = true;

        const ConfigStatus standby_status =
            view.getIntAt(kKeyChannels, index, "standby_deg", standby_deg);
        if (standby_status != ConfigStatus::OK) {
            return fail("servo.channels[FIRE].standby_deg", standby_status);
        }
        const ConfigStatus action_status =
            view.getIntAt(kKeyChannels, index, "action_deg", action_deg);
        if (action_status != ConfigStatus::OK) {
            return fail("servo.channels[FIRE].action_deg", action_status);
        }
        break;
    }
    if (!fire_slot_found) {
        return fail("servo.channels[FIRE]", ConfigStatus::MISSING);
    }
    if (standby_deg < kAngleMin || standby_deg > kAngleMax) {
        return failRange("servo.channels[FIRE].standby_deg", standby_deg);
    }
    if (action_deg < kAngleMin || action_deg > kAngleMax) {
        return failRange("servo.channels[FIRE].action_deg", action_deg);
    }

    std::int64_t hold_ms = 0;
    const ConfigStatus hold_status = view.getInt(kKeyHoldMs, hold_ms);
    if (hold_status != ConfigStatus::OK) {
        return fail(kKeyHoldMs, hold_status);
    }
    if (hold_ms <= 0) {
        return failRange(kKeyHoldMs, hold_ms);
    }

    std::int64_t max_action_ms = 0;
    const ConfigStatus max_status = view.getInt(kKeyMaxActionMs, max_action_ms);
    if (max_status != ConfigStatus::OK) {
        return fail(kKeyMaxActionMs, max_status);
    }
    if (max_action_ms <= 0) {
        return failRange(kKeyMaxActionMs, max_action_ms);
    }

    out.standby_deg = static_cast<int>(standby_deg);
    out.fire_deg = static_cast<int>(action_deg);
    out.hold_ms = static_cast<int>(hold_ms);
    out.max_action_ms = static_cast<int>(max_action_ms);

    FireConfigLoadResult result{};
    result.success = true;
    return result;
}

} // namespace marshland::atom11
