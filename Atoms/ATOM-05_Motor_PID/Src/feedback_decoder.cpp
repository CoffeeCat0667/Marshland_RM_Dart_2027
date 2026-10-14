#include "atom05_motor_pid/feedback_decoder.hpp"

namespace marshland::atom05 {
namespace {

std::uint16_t unsigned16(const std::uint8_t* data) noexcept
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) |
                                       static_cast<std::uint16_t>(data[1]));
}

std::int16_t signed16(const std::uint8_t* data) noexcept
{
    const auto raw = static_cast<std::int32_t>(unsigned16(data));
    return static_cast<std::int16_t>(raw >= 32768 ? raw - 65536 : raw);
}

} // namespace

MotorFault DecodeMotorFeedback(const can::CanFrame& frame,
                                MotorFeedback& output) noexcept
{
    if (!can::isMotorNode(frame.can_id) || frame.dlc != can::kMotorControlDlc) {
        return MotorFault::FEEDBACK_INVALID;
    }
    MotorFeedback decoded{};
    decoded.rotor_angle = unsigned16(frame.payload.data());
    decoded.speed_rpm = signed16(frame.payload.data() + 2);
    decoded.torque_current_raw = signed16(frame.payload.data() + 4);
    decoded.temperature_c = frame.payload[6];
    if (decoded.rotor_angle > 8191U ||
        decoded.speed_rpm > kEncoderLimitRpm || decoded.speed_rpm < -kEncoderLimitRpm) {
        return MotorFault::FEEDBACK_INVALID;
    }
    if (decoded.temperature_c >= 75U) {
        return MotorFault::OVER_TEMPERATURE;
    }
    decoded.direction = decoded.speed_rpm == 0 ? Direction::STOPPED :
                        decoded.speed_rpm > 0 ? Direction::POSITIVE : Direction::NEGATIVE;
    decoded.has_received = true;
    decoded.valid = true;
    decoded.last_valid_time_us = frame.timestamp_us;
    output = decoded;
    return MotorFault::NONE;
}

} // namespace marshland::atom05
