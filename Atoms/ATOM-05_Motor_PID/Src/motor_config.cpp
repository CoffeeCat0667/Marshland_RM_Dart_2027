#include "atom05_motor_pid/motor_config.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <system_error>
#include <utility>

namespace marshland::atom05 {
namespace {

constexpr std::array<const char*, kMotorCount> kRoles{
    "charge_right", "charge_left", "reload_right", "reload_left", "yaw", "force"};

std::string_view text(const char* data, std::uint64_t size) noexcept
{
    return data == nullptr ? std::string_view{} :
           std::string_view(data, static_cast<std::size_t>(size));
}

config::ConfigStatus period(const config::IConfigView& view, const char* key,
    std::uint32_t maximum, std::uint32_t& output) noexcept
{
    std::int64_t value = 0;
    const auto status = view.getInt(key, value);
    if (status != config::ConfigStatus::OK) {
        return status;
    }
    if (value < 1 || value > static_cast<std::int64_t>(maximum)) {
        return config::ConfigStatus::OUT_OF_RANGE;
    }
    output = static_cast<std::uint32_t>(value);
    return config::ConfigStatus::OK;
}

config::ConfigStatus integer(const config::IConfigView& view, std::uint64_t index,
    const char* key, std::int32_t minimum, std::int32_t maximum,
    std::int32_t& output) noexcept
{
    std::int64_t value = 0;
    const auto status = view.getIntAt("motor", index, key, value);
    if (status != config::ConfigStatus::OK) {
        return status;
    }
    if (value < minimum || value > maximum) {
        return config::ConfigStatus::OUT_OF_RANGE;
    }
    output = static_cast<std::int32_t>(value);
    return config::ConfigStatus::OK;
}

config::ConfigStatus exact(const config::IConfigView& view, std::uint64_t index,
    const char* key, std::string_view expected) noexcept
{
    const char* data = nullptr;
    std::uint64_t size = 0;
    const auto status = view.getStringAt("motor", index, key, data, size);
    if (status != config::ConfigStatus::OK) {
        return status;
    }
    return text(data, size) == expected ? config::ConfigStatus::OK :
                                        config::ConfigStatus::OUT_OF_RANGE;
}

template <std::size_t N>
config::ConfigStatus path(const config::IConfigView& view, const char* key,
    std::array<char, N>& output) noexcept
{
    const char* data = nullptr;
    std::uint64_t size = 0;
    const auto status = view.getString(key, data, size);
    if (status != config::ConfigStatus::OK) {
        return status;
    }
    if (data == nullptr || size == 0 || size >= N) {
        return config::ConfigStatus::OUT_OF_RANGE;
    }
    const auto value = text(data, size);
    if (value.find('\0') != std::string_view::npos) {
        return config::ConfigStatus::OUT_OF_RANGE;
    }
    std::copy(value.begin(), value.end(), output.begin());
    output[static_cast<std::size_t>(size)] = '\0';
    return config::ConfigStatus::OK;
}

} // namespace

bool ValidateMotorRuntimeConfig(const MotorRuntimeConfig& config) noexcept
{
    if (config.maximum_cycle_ms == 0 || config.maximum_cycle_ms > kMaximumControlPeriodMs ||
        config.motor_loop_period_ms == 0 || config.motor_loop_period_ms > config.maximum_cycle_ms ||
        config.pid_period_ms == 0 || config.pid_period_ms > config.maximum_cycle_ms ||
        config.feedback_timeout_ms == 0 || config.feedback_timeout_ms > 60000 ||
        config.persistence.directory.front() == '\0' ||
        config.persistence.directory.back() != '\0' ||
        config.persistence.file_name.front() == '\0' ||
        config.persistence.file_name.back() != '\0') {
        return false;
    }
    std::array<bool, kMotorCount> seen{};
    for (const auto& motor : config.motors) {
        if (motor.motor_id < 1 || motor.motor_id > kMotorCount) {
            return false;
        }
        const auto index = static_cast<std::size_t>(motor.motor_id - 1U);
        if (seen[index] || motor.node_id != can::kMotorNodeMin + motor.motor_id - 1U ||
            static_cast<std::size_t>(motor.role) != index || !ValidatePidConfig(motor.pid) ||
            motor.pid.control_period_ms > config.maximum_cycle_ms) {
            return false;
        }
        seen[index] = true;
    }
    return true;
}

ConfigLoadResult LoadMotorRuntimeConfig(const config::IConfigView& view,
                                        MotorRuntimeConfig& output) noexcept
{
    if (!view.isLoaded()) {
        return {};
    }
    MotorRuntimeConfig loaded{};
    loaded.config_version = view.configVersion();
    const char* key = "timing.max_control_cycle_ms";
    auto status = period(view, key, kMaximumControlPeriodMs, loaded.maximum_cycle_ms);
    if (status != config::ConfigStatus::OK) { return {false, status, key, 0}; }
    key = "timing.motor_loop_period_ms";
    status = period(view, key, loaded.maximum_cycle_ms, loaded.motor_loop_period_ms);
    if (status != config::ConfigStatus::OK) { return {false, status, key, 0}; }
    key = "timing.pid_period_ms";
    status = period(view, key, loaded.maximum_cycle_ms, loaded.pid_period_ms);
    if (status != config::ConfigStatus::OK) { return {false, status, key, 0}; }
    key = "timing.can_comm_timeout_ms";
    status = period(view, key, 60000, loaded.feedback_timeout_ms);
    if (status != config::ConfigStatus::OK) { return {false, status, key, 0}; }
    key = "persistence.directory";
    status = path(view, key, loaded.persistence.directory);
    if (status != config::ConfigStatus::OK) { return {false, status, key, 0}; }
    key = "persistence.file_name";
    status = path(view, key, loaded.persistence.file_name);
    if (status != config::ConfigStatus::OK) { return {false, status, key, 0}; }
    std::uint64_t count = 0;
    status = view.getArraySize("motor", count);
    if (status != config::ConfigStatus::OK) { return {false, status, "motor", 0}; }
    if (count != kMotorCount) { return {false, config::ConfigStatus::OUT_OF_RANGE, "motor", 0}; }
    for (std::uint64_t i = 0; i < count; ++i) {
        auto& motor = loaded.motors[static_cast<std::size_t>(i)];
        std::int32_t value = 0;
        key = "id";
        status = integer(view, i, key, 1, static_cast<std::int32_t>(kMotorCount), value);
        if (status != config::ConfigStatus::OK) { return {false, status, key, i}; }
        motor.motor_id = static_cast<std::uint8_t>(value);
        motor.role = static_cast<MotorRole>(motor.motor_id - 1U);
        key = "role";
        status = exact(view, i, key, kRoles[static_cast<std::size_t>(motor.role)]);
        if (status != config::ConfigStatus::OK) { return {false, status, key, i}; }
        for (const auto& entry : std::array<std::pair<const char*, const char*>, 4>{
            {{"bus", "CAN0"}, {"control_mode", "CLOSED_LOOP_PID"},
             {"encoder_source", "CAN"}, {"target_unit", "rpm"}}}) {
            status = exact(view, i, entry.first, entry.second);
            if (status != config::ConfigStatus::OK) { return {false, status, entry.first, i}; }
        }
        const char* data = nullptr;
        std::uint64_t size = 0;
        key = "node";
        status = view.getStringAt("motor", i, key, data, size);
        if (status != config::ConfigStatus::OK) { return {false, status, key, i}; }
        auto node = text(data, size);
        if (node.size() != 5 || node.substr(0, 2) != "0x") {
            return {false, config::ConfigStatus::INVALID_TYPE, key, i};
        }
        const auto parsed = std::from_chars(node.data() + 2, node.data() + node.size(), motor.node_id, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != node.data() + node.size()) {
            return {false, config::ConfigStatus::INVALID_TYPE, key, i};
        }
        for (const auto& entry : std::array<std::pair<const char*, std::int32_t*>, 3>{
            {{"pid.kp", &motor.pid.kp}, {"pid.ki", &motor.pid.ki}, {"pid.kd", &motor.pid.kd}}}) {
            status = integer(view, i, entry.first, -kGainLimit, kGainLimit, *entry.second);
            if (status != config::ConfigStatus::OK) { return {false, status, entry.first, i}; }
        }
        key = "pid.integral_limit";
        status = integer(view, i, key, 0, kPidLimit, motor.pid.integral_limit);
        if (status != config::ConfigStatus::OK) { return {false, status, key, i}; }
        key = "pid.output_limit";
        status = integer(view, i, key, 1, kPidLimit, motor.pid.output_limit);
        if (status != config::ConfigStatus::OK) { return {false, status, key, i}; }
        key = "pid.control_period_ms";
        status = integer(view, i, key, 1, static_cast<std::int32_t>(loaded.maximum_cycle_ms), value);
        if (status != config::ConfigStatus::OK) { return {false, status, key, i}; }
        motor.pid.control_period_ms = static_cast<std::uint32_t>(value);
        key = "pid.enabled";
        status = view.getBoolAt("motor", i, key, motor.pid.enabled);
        if (status != config::ConfigStatus::OK) { return {false, status, key, i}; }
    }
    if (!view.isLoaded() || view.configVersion() != loaded.config_version) {
        return {false, config::ConfigStatus::NOT_LOADED, "configVersion", 0};
    }
    if (!ValidateMotorRuntimeConfig(loaded)) {
        return {false, config::ConfigStatus::OUT_OF_RANGE, "motor mapping", 0};
    }
    output = loaded;
    return {true, config::ConfigStatus::OK, nullptr, 0};
}

} // namespace marshland::atom05
