#include "atom07_servo_driver/servo_config.hpp"

#include <cstddef>

#include "atom07_servo_driver/pca9685_registers.hpp"

namespace marshland::atom07 {
namespace {

using marshland::config::ConfigStatus;
using marshland::config::IConfigView;

constexpr const char* kKeyI2cDevice      = "servo.pca9685.i2c_device";
constexpr const char* kKeyAddress        = "servo.pca9685.address";
constexpr const char* kKeyChannelCount   = "servo.pca9685.channel_count";
constexpr const char* kKeyResolution     = "servo.pca9685.pwm_resolution_bits";
constexpr const char* kKeyFrequency      = "servo.pca9685.pwm_frequency_hz";
constexpr const char* kKeyChannelRange   = "servo.pca9685.channel_range";
constexpr const char* kKeyOeLevel        = "servo.pca9685.oe_active_level";
constexpr const char* kKeyAccessMode     = "servo.pca9685.access_mode";
constexpr const char* kKeyErrorHandling  = "servo.pca9685.error_handling";
constexpr const char* kKeyPulseMin       = "servo.model.pulse_min_us";
constexpr const char* kKeyPulseMax       = "servo.model.pulse_max_us";
constexpr const char* kKeyAngleRange     = "servo.model.angle_range_deg";
constexpr const char* kKeyIicTimeout     = "timing.iic_comm_timeout_ms";
constexpr const char* kKeyChannels       = "servo.channels";

ServoConfigLoadResult fail(const char* key, ConfigStatus status) noexcept
{
    ServoConfigLoadResult result{};
    result.success = false;
    result.failed_key = key;
    result.status = status;
    return result;
}

// 标量数组（array<int>）读取：元素无对象键，按空 element_key 查询。
// 若 ATOM-15 对 array<int> 使用其它约定，本函数会如实失败而不是臆造默认值（见 PLAN §11）。
bool readIntArray(const IConfigView& view, const char* key, int& first, int& last,
                  const char*& failed_key, ConfigStatus& failed_status) noexcept
{
    std::uint64_t size = 0;
    const ConfigStatus size_status = view.getArraySize(key, size);
    if (size_status != ConfigStatus::OK || size != 2U) {
        failed_key = key;
        failed_status = (size_status == ConfigStatus::OK) ? ConfigStatus::OUT_OF_RANGE : size_status;
        return false;
    }
    std::int64_t value = 0;
    const ConfigStatus first_status = view.getIntAt(key, 0U, "", value);
    if (first_status != ConfigStatus::OK) {
        failed_key = key;
        failed_status = first_status;
        return false;
    }
    first = static_cast<int>(value);
    const ConfigStatus last_status = view.getIntAt(key, 1U, "", value);
    if (last_status != ConfigStatus::OK) {
        failed_key = key;
        failed_status = last_status;
        return false;
    }
    last = static_cast<int>(value);
    return true;
}

bool readString(const IConfigView& view, const char* key, std::string& out,
                const char*& failed_key, ConfigStatus& failed_status) noexcept
{
    const char* data = nullptr;
    std::uint64_t size = 0;
    const ConfigStatus status = view.getString(key, data, size);
    if (status != ConfigStatus::OK || data == nullptr || size == 0U) {
        failed_key = key;
        failed_status = (status == ConfigStatus::OK) ? ConfigStatus::INVALID_TYPE : status;
        return false;
    }
    try {
        out.assign(data, static_cast<std::size_t>(size));
    } catch (...) {
        failed_key = key;
        failed_status = ConfigStatus::INVALID_TYPE;
        return false;
    }
    return true;
}

bool readInt(const IConfigView& view, const char* key, std::int64_t& out,
             const char*& failed_key, ConfigStatus& failed_status) noexcept
{
    const ConfigStatus status = view.getInt(key, out);
    if (status != ConfigStatus::OK) {
        failed_key = key;
        failed_status = status;
        return false;
    }
    return true;
}

// "0x40" -> 0x40；非法返回 false（禁止猜测地址）。
bool parseAddress(const std::string& text, int& out) noexcept
{
    if (text.empty() || text.size() > 4U) {
        return false;
    }
    int value = 0;
    for (const char c : text) {
        int digit = -1;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = 10 + (c - 'a');
        } else if (c >= 'A' && c <= 'F') {
            digit = 10 + (c - 'A');
        } else {
            return false;
        }
        value = (value * 16) + digit;
    }
    if (value < 0x03 || value > 0x77) { // 7 位地址有效范围
        return false;
    }
    out = value;
    return true;
}

bool parseMechanism(const std::string& text, ServoMechanism& out) noexcept
{
    if (text == "RELOAD") { out = ServoMechanism::Reload; return true; }
    if (text == "FEED")   { out = ServoMechanism::Feed;   return true; }
    if (text == "FIRE")   { out = ServoMechanism::Fire;   return true; }
    return false;
}

} // namespace

ServoConfigLoadResult LoadServoConfig(const IConfigView& view, ServoConfig& output) noexcept
{
    ServoConfigLoadResult result{};
    const char* failed_key = nullptr;
    ConfigStatus failed_status = ConfigStatus::NOT_LOADED;

    try {
        // ---- servo.pca9685.* -------------------------------------------------
        if (!readString(view, kKeyI2cDevice, output.i2c_device, failed_key, failed_status)) {
            return fail(failed_key, failed_status);
        }
        std::string address_text;
        if (!readString(view, kKeyAddress, address_text, failed_key, failed_status)) {
            return fail(failed_key, failed_status);
        }
        if (!parseAddress(address_text, output.device_address)) {
            return fail(kKeyAddress, ConfigStatus::OUT_OF_RANGE);
        }

        std::int64_t value = 0;
        if (!readInt(view, kKeyChannelCount, value, failed_key, failed_status)) {
            return fail(kKeyChannelCount, failed_status);
        }
        if (value != static_cast<std::int64_t>(pca9685::kChannelCountMax)) {
            return fail(kKeyChannelCount, ConfigStatus::OUT_OF_RANGE);
        }
        output.channel_count = static_cast<int>(value);
        if (!readInt(view, kKeyResolution, value, failed_key, failed_status)) {
            return fail(kKeyResolution, failed_status);
        }
        if (value < 1 || value > 16) {
            return fail(kKeyResolution, ConfigStatus::OUT_OF_RANGE);
        }
        output.pwm_resolution_bits = static_cast<int>(value);
        if (!readInt(view, kKeyFrequency, value, failed_key, failed_status)) {
            return fail(kKeyFrequency, failed_status);
        }
        if (value <= 0) {
            return fail(kKeyFrequency, ConfigStatus::OUT_OF_RANGE);
        }
        output.pwm_frequency_hz = static_cast<double>(value);
        {
            std::uint8_t prescale = 0;
            if (!pca9685::computePrescale(output.pwm_frequency_hz, prescale)) {
                return fail(kKeyFrequency, ConfigStatus::OUT_OF_RANGE);
            }
        }
        if (!readIntArray(view, kKeyChannelRange, output.channel_min, output.channel_max,
                          failed_key, failed_status)) {
            return fail(failed_key, failed_status);
        }
        if (output.channel_min < 0 || output.channel_max >= output.channel_count ||
            output.channel_max < output.channel_min) {
            return fail(kKeyChannelRange, ConfigStatus::OUT_OF_RANGE);
        }

        // OE 只校验（用户裁决：OE 硬件默认使能，本模块不驱动 GPIO）。
        std::string oe_level;
        if (!readString(view, kKeyOeLevel, oe_level, failed_key, failed_status)) {
            return fail(kKeyOeLevel, failed_status);
        }
        if (oe_level != "LOW") {
            return fail(kKeyOeLevel, ConfigStatus::OUT_OF_RANGE);
        }
        std::string access_mode;
        if (!readString(view, kKeyAccessMode, access_mode, failed_key, failed_status)) {
            return fail(kKeyAccessMode, failed_status);
        }
        if (access_mode != "SERIALIZED") {
            return fail(kKeyAccessMode, ConfigStatus::OUT_OF_RANGE);
        }
        std::string error_handling;
        if (!readString(view, kKeyErrorHandling, error_handling, failed_key, failed_status)) {
            return fail(kKeyErrorHandling, failed_status);
        }
        if (error_handling != "ENTER_FAULT") {
            return fail(kKeyErrorHandling, ConfigStatus::OUT_OF_RANGE);
        }

        // ---- servo.model.* ---------------------------------------------------
        if (!readInt(view, kKeyPulseMin, value, failed_key, failed_status)) {
            return fail(kKeyPulseMin, failed_status);
        }
        if (value <= 0) {
            return fail(kKeyPulseMin, ConfigStatus::OUT_OF_RANGE);
        }
        output.pulse_min_us = static_cast<int>(value);
        if (!readInt(view, kKeyPulseMax, value, failed_key, failed_status)) {
            return fail(kKeyPulseMax, failed_status);
        }
        if (value <= output.pulse_min_us) {
            return fail(kKeyPulseMax, ConfigStatus::OUT_OF_RANGE);
        }
        output.pulse_max_us = static_cast<int>(value);
        if (!readIntArray(view, kKeyAngleRange, output.angle_min_deg, output.angle_max_deg,
                          failed_key, failed_status)) {
            return fail(failed_key, failed_status);
        }
        if (output.angle_max_deg <= output.angle_min_deg) {
            return fail(kKeyAngleRange, ConfigStatus::OUT_OF_RANGE);
        }

        // ---- timing.iic_comm_timeout_ms（单次事务超时） -----------------------
        if (!readInt(view, kKeyIicTimeout, value, failed_key, failed_status)) {
            return fail(kKeyIicTimeout, failed_status);
        }
        if (value <= 0) {
            return fail(kKeyIicTimeout, ConfigStatus::OUT_OF_RANGE);
        }
        output.iic_comm_timeout_ms = static_cast<std::uint32_t>(value);

        // ---- servo.channels[]（唯一归口） ------------------------------------
        std::uint64_t slot_count = 0;
        const ConfigStatus size_status = view.getArraySize(kKeyChannels, slot_count);
        if (size_status != ConfigStatus::OK || slot_count == 0U) {
            return fail(kKeyChannels,
                        size_status == ConfigStatus::OK ? ConfigStatus::OUT_OF_RANGE : size_status);
        }
        output.slots.clear();
        output.slots.reserve(static_cast<std::size_t>(slot_count));

        for (std::uint64_t index = 0; index < slot_count; ++index) {
            ServoSlotConfig slot{};

            // id 允许为 null（schema：servo_id 可为 integer 或 null）-> 视为未编号 0
            std::int64_t id_value = 0;
            if (view.getIntAt(kKeyChannels, index, "id", id_value) == ConfigStatus::OK) {
                slot.servo_id = static_cast<int>(id_value);
            }

            const char* mechanism_data = nullptr;
            std::uint64_t mechanism_size = 0;
            std::string mechanism_text;
            const ConfigStatus mechanism_status = view.getStringAt(
                kKeyChannels, index, "mechanism", mechanism_data, mechanism_size);
            if (mechanism_status != ConfigStatus::OK || mechanism_data == nullptr ||
                mechanism_size == 0U) {
                return fail(kKeyChannels, mechanism_status);
            }
            mechanism_text.assign(mechanism_data, static_cast<std::size_t>(mechanism_size));
            if (!parseMechanism(mechanism_text, slot.mechanism)) {
                return fail(kKeyChannels, ConfigStatus::INVALID_TYPE);
            }

            // channel 为 null -> 未接入（-1）；禁止推断
            std::int64_t channel_value = 0;
            if (view.getIntAt(kKeyChannels, index, "channel", channel_value) == ConfigStatus::OK) {
                slot.channel = static_cast<int>(channel_value);
                if (!output.channelInRange(slot.channel)) {
                    return fail(kKeyChannels, ConfigStatus::OUT_OF_RANGE);
                }
            }

            output.slots.push_back(slot);
        }
    } catch (...) {
        return fail(kKeyChannels, ConfigStatus::INVALID_TYPE);
    }

    result.success = true;
    result.failed_key = nullptr;
    return result;
}

const ServoSlotConfig* FindSlotById(const ServoConfig& config, int servo_id) noexcept
{
    if (servo_id <= 0) {
        return nullptr;
    }
    for (const ServoSlotConfig& slot : config.slots) {
        if (slot.servo_id == servo_id) {
            return &slot;
        }
    }
    return nullptr;
}

const ServoSlotConfig* FindSlotByMechanism(const ServoConfig& config,
                                           ServoMechanism mechanism) noexcept
{
    for (const ServoSlotConfig& slot : config.slots) {
        if (slot.mechanism == mechanism) {
            return &slot;
        }
    }
    return nullptr;
}

} // namespace marshland::atom07
