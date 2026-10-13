#include "atom03_SBUS/sbus_config.hpp"
#include "SbusTypes.hpp"

namespace marshland::atom03 {
namespace {

bool requiredInt(const marshland::config::IConfigView& view, const char* key,
                 std::int64_t& value, const char*& error) noexcept
{
    if (view.getInt(key, value) != marshland::config::ConfigStatus::OK) {
        error = key;
        return false;
    }
    return true;
}

bool requiredString(const marshland::config::IConfigView& view, const char* key,
                    std::string& value, const char*& error) noexcept
{
    const char* data = nullptr;
    std::uint64_t size = 0;
    if (view.getString(key, data, size) != marshland::config::ConfigStatus::OK ||
        data == nullptr || size == 0U) {
        error = key;
        return false;
    }
    try {
        value.assign(data, static_cast<std::size_t>(size));
    } catch (...) {
        error = key;
        return false;
    }
    return true;
}

} // namespace

ConfigLoadResult LoadSbusConfig(const marshland::config::IConfigView& view,
                                SbusConfig& output) noexcept
{
    std::int64_t value = 0;
    const char* error = "sbus configuration";
    if (!requiredString(view, "sbus.serial_device", output.serial.serial_device, error)) {
        return {false, error};
    }
    if (!requiredInt(view, "sbus.channel_scale_x", value, error) || value < 1 || value > 10) {
        return {false, error};
    }
    output.channel_scale_x = static_cast<std::uint8_t>(value);
    if (!requiredInt(view, "timing.sbus_receive_period_ms", value, error) || value <= 0) {
        return {false, error};
    }
    output.receive_period_ms = static_cast<std::uint32_t>(value);
    if (!requiredInt(view, "timing.sbus_loss_timeout_ms", value, error) || value <= 0) {
        return {false, error};
    }
    output.loss_timeout_ms = static_cast<std::uint32_t>(value);
    if (!requiredInt(view, "sbus.frame_bytes", value, error)) {
        return {false, error};
    }
    output.frame_bytes = static_cast<std::uint16_t>(value);
    if (!requiredInt(view, "sbus.joystick_center", value, error)) {
        return {false, error};
    }
    output.channel_center = static_cast<std::uint16_t>(value);
    if (!requiredInt(view, "sbus.valid_min", value, error)) {
        return {false, error};
    }
    output.channel_minimum = static_cast<std::uint16_t>(value);
    if (!requiredInt(view, "sbus.valid_max", value, error)) {
        return {false, error};
    }
    output.channel_maximum = static_cast<std::uint16_t>(value);
    output.fire_confirmation_frames = marshland::sbus::kFireConfirmationFrames;
    return {true, nullptr};
}

} // namespace marshland::atom03
