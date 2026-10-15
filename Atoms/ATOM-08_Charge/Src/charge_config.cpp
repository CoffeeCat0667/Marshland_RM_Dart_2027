#include "atom08_charge/charge_config.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace marshland::atom08 {
namespace {

using marshland::config::ConfigStatus;
using marshland::config::IConfigView;

constexpr const char* kMotorKey = "motor";
constexpr const char* kSbusFunctionsKey = "sbus.functions";
constexpr std::int64_t kMicrosPerMilli = 1000;
constexpr std::int64_t kMicrosPerSecond = 1000000;
constexpr std::int64_t kMaxTargetRange = 32767;

bool nameEquals(const char* data, std::uint64_t size, const char* expected) noexcept
{
    if (data == nullptr) {
        return false;
    }
    return std::string_view(data, static_cast<std::size_t>(size)) == std::string_view(expected);
}

bool readRequiredInt(const IConfigView& view, const char* key, std::int64_t& out) noexcept
{
    return view.getInt(key, out) == ConfigStatus::OK;
}

// "CH2" → 1（0 基下标）；null / 非该格式 / 越界一律视为未映射
bool readChannelIndex(const IConfigView& view, std::uint64_t index, std::int32_t& out) noexcept
{
    const char* data = nullptr;
    std::uint64_t size = 0;
    if (view.getStringAt(kSbusFunctionsKey, index, "channel", data, size) != ConfigStatus::OK) {
        return false;
    }
    if (data == nullptr) {
        return false;
    }
    const std::string_view text(data, static_cast<std::size_t>(size));
    if (text.size() < 3U || text[0] != 'C' || text[1] != 'H') {
        return false;
    }
    std::int32_t number = 0;
    for (std::size_t i = 2; i < text.size(); ++i) {
        const char digit = text[i];
        if (digit < '0' || digit > '9') {
            return false;
        }
        number = number * 10 + static_cast<std::int32_t>(digit - '0');
        if (number > 16) {
            return false;
        }
    }
    if (number < 1) {
        return false;
    }
    out = number - 1;
    return true;
}

void loadSbusFunctionChannels(const IConfigView& view, ChargeConfig& output) noexcept
{
    std::uint64_t count = 0;
    if (view.getArraySize(kSbusFunctionsKey, count) != ConfigStatus::OK) {
        return;
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        const char* name = nullptr;
        std::uint64_t name_size = 0;
        if (view.getStringAt(kSbusFunctionsKey, i, "name", name, name_size) != ConfigStatus::OK) {
            continue;
        }
        std::int32_t channel = kUnmappedChannel;
        if (!readChannelIndex(view, i, channel)) {
            channel = kUnmappedChannel;
        }
        if (nameEquals(name, name_size, "manual_charge")) {
            output.sbus_manual_charge_channel = channel;
        } else if (nameEquals(name, name_size, "manual_charge_left")) {
            output.sbus_manual_charge_left_channel = channel;
        } else if (nameEquals(name, name_size, "manual_charge_right")) {
            output.sbus_manual_charge_right_channel = channel;
        }
    }
}

bool loadMotorIds(const IConfigView& view, ChargeConfig& output) noexcept
{
    std::uint64_t count = 0;
    if (view.getArraySize(kMotorKey, count) != ConfigStatus::OK || count == 0U) {
        return false;
    }
    bool have_left = false;
    bool have_right = false;
    for (std::uint64_t i = 0; i < count; ++i) {
        const char* role = nullptr;
        std::uint64_t role_size = 0;
        if (view.getStringAt(kMotorKey, i, "role", role, role_size) != ConfigStatus::OK) {
            continue;
        }
        std::int64_t id = 0;
        if (view.getIntAt(kMotorKey, i, "id", id) != ConfigStatus::OK) {
            continue;
        }
        if (id < 1 || id > 255) {
            return false;
        }
        if (nameEquals(role, role_size, "charge_left")) {
            output.left_motor_id = static_cast<std::uint8_t>(id);
            have_left = true;
        } else if (nameEquals(role, role_size, "charge_right")) {
            output.right_motor_id = static_cast<std::uint8_t>(id);
            have_right = true;
        }
    }
    return have_left && have_right && output.left_motor_id != output.right_motor_id;
}

bool withinRange(std::int32_t value, std::int32_t range) noexcept
{
    if (range <= 0) {
        return true;
    }
    const std::int64_t magnitude = value < 0
        ? -static_cast<std::int64_t>(value)
        : static_cast<std::int64_t>(value);
    return magnitude <= static_cast<std::int64_t>(range);
}

} // namespace

ChargeConfigLoadResult LoadChargeConfig(const IConfigView& view, ChargeConfig& output) noexcept
{
    ChargeConfig loaded{};
    std::int64_t value = 0;

    if (!readRequiredInt(view, "timing.charge_time_s", value) || value <= 0) {
        return {false, "timing.charge_time_s"};
    }
    loaded.charge_time_us = value * kMicrosPerSecond;

    if (!readRequiredInt(view, "timing.charge_max_action_ms", value) || value <= 0) {
        return {false, "timing.charge_max_action_ms"};
    }
    loaded.charge_max_action_us = value * kMicrosPerMilli;

    if (!readRequiredInt(view, "timing.charge_home_max_action_ms", value) || value <= 0) {
        return {false, "timing.charge_home_max_action_ms"};
    }
    loaded.home_max_action_us = value * kMicrosPerMilli;

    if (!readRequiredInt(view, "charge.charge_speed_rpm", value)) {
        return {false, "charge.charge_speed_rpm"};
    }
    loaded.charge_speed_rpm = static_cast<std::int32_t>(value);

    if (!readRequiredInt(view, "charge.home_speed_rpm", value)) {
        return {false, "charge.home_speed_rpm"};
    }
    loaded.home_speed_rpm = static_cast<std::int32_t>(value);

    if (view.getInt("charge.target_range", value) == ConfigStatus::OK) {
        if (value < 0 || value > kMaxTargetRange) {
            return {false, "charge.target_range"};
        }
        loaded.target_range_rpm = static_cast<std::int32_t>(value);
    }

    if (!loadMotorIds(view, loaded)) {
        return {false, "motor[] charge_left/charge_right mapping"};
    }
    loadSbusFunctionChannels(view, loaded);

    if (!withinRange(loaded.charge_speed_rpm, loaded.target_range_rpm)) {
        return {false, "charge.charge_speed_rpm out of charge.target_range"};
    }
    if (!withinRange(loaded.home_speed_rpm, loaded.target_range_rpm)) {
        return {false, "charge.home_speed_rpm out of charge.target_range"};
    }

    output = loaded;
    return {true, nullptr};
}

} // namespace marshland::atom08
