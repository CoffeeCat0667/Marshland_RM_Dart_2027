#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "CanTypes.hpp"
#include "MarshlandTypes.hpp"

namespace marshland::atom05 {

inline constexpr std::size_t kMotorCount = can::kMaxMotorNodes;
inline constexpr std::int32_t kGainLimit = 1000;
inline constexpr std::int32_t kPidLimit = 16000;
inline constexpr std::int32_t kActuatorLimitMa = 15000;
inline constexpr std::int32_t kEncoderLimitRpm = 7500;
inline constexpr std::uint32_t kMaximumControlPeriodMs = 100;
inline constexpr std::uint8_t kSaturationFaultCycles = 5;

enum class MotorRole : std::uint8_t {
    CHARGE_RIGHT, CHARGE_LEFT, RELOAD_RIGHT, RELOAD_LEFT, YAW, FORCE
};
enum class MotorMode : std::uint8_t { DISABLED, VELOCITY, CURRENT, TORQUE, POSITION };
enum class CommandPurpose : std::uint8_t { CONTROL, SAFETY_RECOVERY };
enum class Direction : std::uint8_t { STOPPED, POSITIVE, NEGATIVE };
enum class MotorStatus : std::uint8_t {
    OK, NOT_READY, UNKNOWN_MOTOR, INVALID_CONFIG, INVALID_COMMAND,
    UNSUPPORTED_MODE, PERMISSION_DENIED, FEEDBACK_INVALID, FEEDBACK_TIMEOUT,
    FAULT_LATCHED, DISABLED, NOT_DUE, OUTPUT_FAILED, BUSY, CLOCK_INVALID
};
enum class MotorFault : std::uint8_t {
    NONE, FEEDBACK_INVALID, FEEDBACK_TIMEOUT, OVER_TEMPERATURE,
    PID_NONFINITE, PID_SATURATION, OUTPUT_FAILED, RELOAD_PEER,
    CLOCK_OR_DEADLINE
};

struct PidConfig final {
    std::int32_t kp{0};
    std::int32_t ki{0};
    std::int32_t kd{0};
    std::int32_t integral_limit{0};
    std::int32_t output_limit{0};
    std::uint32_t control_period_ms{0};
    bool enabled{false};
};
constexpr bool ValidatePidConfig(const PidConfig& config) noexcept
{
    return config.kp >= -kGainLimit && config.kp <= kGainLimit &&
           config.ki >= -kGainLimit && config.ki <= kGainLimit &&
           config.kd >= -kGainLimit && config.kd <= kGainLimit &&
           config.integral_limit >= 0 && config.integral_limit <= kPidLimit &&
           config.output_limit > 0 && config.output_limit <= kPidLimit &&
           config.control_period_ms > 0 &&
           config.control_period_ms <= kMaximumControlPeriodMs;
}
struct MotorMapping final {
    std::uint8_t motor_id{0};
    std::uint32_t node_id{0};
    MotorRole role{MotorRole::CHARGE_RIGHT};
    PidConfig pid{};
};
struct MotorCommand final {
    std::uint8_t motor_id{0};
    MotorMode mode{MotorMode::DISABLED};
    double target{0.0}; // rpm for VELOCITY; mA for CURRENT/TORQUE.
    std::int32_t output_limit_ma{0};
    ControlSource source{ControlSource::NONE};
    CommandPurpose purpose{CommandPurpose::CONTROL};
    std::uint32_t request_id{0};
};
struct MotorFeedback final {
    bool has_received{false};
    bool valid{false};
    std::uint16_t rotor_angle{0}; // C620 count, not accumulated position.
    std::int16_t speed_rpm{0};
    std::int16_t torque_current_raw{0}; // No guessed feedback scaling.
    std::uint8_t temperature_c{0};
    Direction direction{Direction::STOPPED};
    std::uint64_t last_valid_time_us{0};
    std::uint64_t sequence{0};
};
struct MotorSnapshot final {
    bool found{false};
    std::uint8_t motor_id{0};
    std::uint32_t node_id{0};
    MotorFeedback feedback{};
    MotorCommand command{};
    PidConfig pid{};
    bool enabled{false};
    bool target_active{false};
    MotorFault fault{MotorFault::NONE};
    double output_ma{0.0};
    std::uint8_t saturation_cycles{0};
    std::uint64_t pid_revision{0};
};
struct PersistenceLocation final {
    std::array<char, 256> directory{};
    std::array<char, 128> file_name{};
};
enum class PersistenceStatus : std::uint8_t {
    NOT_ATTEMPTED, SAVED, FAILED, UNAVAILABLE
};
struct PidUpdateRequest final {
    std::uint8_t motor_id{0};
    ControlSource source{ControlSource::NONE};
    std::uint32_t request_id{0};
    PidConfig parameters{};
};
struct PidUpdateResult final {
    MotorStatus status{MotorStatus::NOT_READY};
    bool applied{false};
    PersistenceStatus persistence{PersistenceStatus::NOT_ATTEMPTED};
    std::uint64_t revision{0};
};
struct FaultEvent final {
    std::uint8_t motor_id{0};
    std::uint8_t reload_peer_id{0};
    MotorFault fault{MotorFault::NONE};
    std::uint64_t timestamp_us{0};
    bool zero_latched{false};
};
struct MotorOutputBatch final {
    std::array<std::array<std::int16_t, can::kMotorSlotsPerFrame>,
               can::kMotorControlFrameCount> currents{}; // C620 command counts.
    std::uint8_t active_mask{0}; // Bit n describes node 0x201+n.
    std::uint64_t generation{0};
};
constexpr std::int16_t MilliampsToC620(std::int32_t milliamps) noexcept
{
    // C620 command +/-16384 corresponds to +/-20 A, not +/-16384 mA.
    const auto bounded = milliamps > kActuatorLimitMa ? kActuatorLimitMa :
                         milliamps < -kActuatorLimitMa ? -kActuatorLimitMa : milliamps;
    return static_cast<std::int16_t>(static_cast<std::int64_t>(bounded) * 16384 / 20000);
}
static_assert(ValidatePidConfig({3, 0, 0, 16000, 16000, 20, true}));
static_assert(ValidatePidConfig({-3, -1, -2, 16000, 16000, 20, true}));
static_assert(!ValidatePidConfig({1001, 0, 0, 16000, 16000, 20, true}));
static_assert(ValidatePidConfig({-1000, 1000, -1000, 0, 1, 1, false}));
static_assert(!ValidatePidConfig({3, 0, 0, -1, 16000, 20, true}));
static_assert(!ValidatePidConfig({3, 0, 0, 16000, 0, 20, true}));
static_assert(!ValidatePidConfig({3, 0, 0, 16000, 16000, 101, true}));
static_assert(MilliampsToC620(15000) == 12288);
static_assert(MilliampsToC620(-15000) == -12288);
static_assert(MilliampsToC620(2147483647) == 12288);
static_assert(MilliampsToC620(-2147483647 - 1) == -12288);

} // namespace marshland::atom05
