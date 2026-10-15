#include "atom09_force/force_config.hpp"

#include <cstdint>

namespace marshland::atom09 {
namespace {

bool TextEquals(const char* data, std::uint64_t size, const char* literal) noexcept
{
    if (data == nullptr || literal == nullptr) {
        return false;
    }
    std::uint64_t length = 0;
    while (literal[length] != '\0') {
        ++length;
    }
    if (size != length) {
        return false;
    }
    for (std::uint64_t i = 0; i < length; ++i) {
        if (data[i] != literal[i]) {
            return false;
        }
    }
    return true;
}

const char* RequireEnum(const marshland::config::IConfigView& view,
                        const char* key, const char* expected,
                        const char* error) noexcept
{
    const char* data = nullptr;
    std::uint64_t size = 0;
    if (view.getString(key, data, size) != marshland::config::ConfigStatus::OK) {
        return error;
    }
    return TextEquals(data, size, expected) ? nullptr : error;
}

const char* RequireInt(const marshland::config::IConfigView& view,
                       const char* key, std::int64_t minimum, std::int64_t maximum,
                       std::int64_t& out, const char* error) noexcept
{
    std::int64_t value = 0;
    if (view.getInt(key, value) != marshland::config::ConfigStatus::OK) {
        return error;
    }
    if (value < minimum || value > maximum) {
        return error;
    }
    out = value;
    return nullptr;
}

// 按 motor[].role == "force" 唯一匹配解析电机 ID（禁止硬编码，Q-08）。
bool FindForceMotorId(const marshland::config::IConfigView& view, int& out_id) noexcept
{
    std::uint64_t count = 0;
    if (view.getArraySize("motor", count) != marshland::config::ConfigStatus::OK) {
        return false;
    }
    int matched_id = 0;
    std::uint64_t matches = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        const char* role = nullptr;
        std::uint64_t role_size = 0;
        if (view.getStringAt("motor", index, "role", role, role_size) !=
            marshland::config::ConfigStatus::OK) {
            return false;
        }
        if (!TextEquals(role, role_size, "force")) {
            continue;
        }
        std::int64_t id = 0;
        if (view.getIntAt("motor", index, "id", id) != marshland::config::ConfigStatus::OK) {
            return false;
        }
        if (id < 1 || id > 1000) {
            return false;
        }
        matched_id = static_cast<int>(id);
        ++matches;
    }
    if (matches != 1U) {
        return false;
    }
    out_id = matched_id;
    return true;
}

} // namespace

ForceConfigLoadResult LoadForceConfig(const marshland::config::IConfigView& view,
                                      ForceConfig& output) noexcept
{
    ForceConfigLoadResult result{};
    if (!view.isLoaded()) {
        result.error_message = "configuration is not loaded";
        return result;
    }

    const char* error = RequireEnum(view, "force.control_mode", "CLOSED_LOOP_PID",
                                    "force.control_mode unusable (expected CLOSED_LOOP_PID)");
    if (error != nullptr) {
        result.error_message = error;
        return result;
    }

    error = RequireEnum(view, "force.target_unit", "rpm",
                        "force.target_unit unusable (expected rpm)");
    if (error != nullptr) {
        result.error_message = error;
        return result;
    }

    error = RequireEnum(view, "force.completion_criterion", "TIMED",
                        "force.completion_criterion unusable (expected TIMED)");
    if (error != nullptr) {
        result.error_message = error;
        return result;
    }

    error = RequireEnum(view, "force.center_action", "ZERO_TORQUE",
                        "force.center_action unusable (expected ZERO_TORQUE)");
    if (error != nullptr) {
        result.error_message = error;
        return result;
    }

    error = RequireEnum(view, "force.target_default_source", "SBUS_CH12",
                        "force.target_default_source unusable (expected SBUS_CH12)");
    if (error != nullptr) {
        result.error_message = error;
        return result;
    }

    bool dedicated_fault_handling = true;
    if (view.getBool("force.has_dedicated_fault_handling", dedicated_fault_handling) !=
            marshland::config::ConfigStatus::OK ||
        dedicated_fault_handling) {
        result.error_message = "force.has_dedicated_fault_handling unusable (expected false)";
        return result;
    }

    std::int64_t target_range = 0;
    error = RequireInt(view, "force.target_range", 1, 15000, target_range,
                       "force.target_range unusable (expected 1..15000)");
    if (error != nullptr) {
        result.error_message = error;
        return result;
    }

    std::int64_t deadband = 0;
    error = RequireInt(view, "force.center_deadband_rpm", 0, 2000, deadband,
                       "force.center_deadband_rpm unusable (expected 0..2000)");
    if (error != nullptr) {
        result.error_message = error;
        return result;
    }

    std::int64_t force_time_s = 0;
    error = RequireInt(view, "timing.force_time_s", 0, 3600, force_time_s,
                       "timing.force_time_s unusable (expected 0..3600)");
    if (error != nullptr) {
        result.error_message = error;
        return result;
    }

    std::int64_t max_cycle_ms = 0;
    error = RequireInt(view, "timing.max_control_cycle_ms", 1, 100, max_cycle_ms,
                       "timing.max_control_cycle_ms unusable (expected 1..100)");
    if (error != nullptr) {
        result.error_message = error;
        return result;
    }

    int motor_id = 0;
    if (!FindForceMotorId(view, motor_id)) {
        result.error_message =
            "motor[] must contain exactly one entry with role 'force' and a valid id";
        return result;
    }

    ForceConfig loaded{};
    loaded.motor_id = motor_id;
    loaded.target_range_rpm = static_cast<std::int32_t>(target_range);
    loaded.center_deadband_rpm = static_cast<std::int32_t>(deadband);
    loaded.timed_action_ms = static_cast<std::uint32_t>(force_time_s) * 1000U;
    loaded.max_control_cycle_ms = static_cast<std::uint32_t>(max_cycle_ms);

    output = loaded;
    result.success = true;
    result.error_message = "";
    return result;
}

} // namespace marshland::atom09
