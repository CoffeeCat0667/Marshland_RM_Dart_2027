#include "atom05_motor_pid/pid_controller.hpp"

#include <algorithm>
#include <cmath>

namespace marshland::atom05 {

void PidController::reset() noexcept
{
    integral_term_ = 0.0;
    previous_error_ = 0.0;
    has_previous_ = false;
    saturation_cycles_ = 0;
}

PidStepResult PidController::step(const PidConfig& config, double target_rpm,
    double speed_rpm, double dt_seconds, double effective_limit_ma) noexcept
{
    PidStepResult result{};
    if (!ValidatePidConfig(config) || !config.enabled ||
        !std::isfinite(target_rpm) || !std::isfinite(speed_rpm) ||
        !std::isfinite(dt_seconds) || dt_seconds <= 0.0 || dt_seconds > 0.1 ||
        !std::isfinite(effective_limit_ma) || effective_limit_ma <= 0.0) {
        result.fault = MotorFault::PID_NONFINITE;
        return result;
    }
    const double limit = std::min({effective_limit_ma,
        static_cast<double>(config.output_limit), static_cast<double>(kActuatorLimitMa)});
    const double error = target_rpm - speed_rpm;
    const double derivative = has_previous_ ? (error - previous_error_) / dt_seconds : 0.0;
    const double delta_integral = static_cast<double>(config.ki) * error * dt_seconds;
    double integral = config.ki == 0 ? 0.0 : integral_term_ + delta_integral;
    const double integral_limit = static_cast<double>(config.integral_limit);
    if (!std::isfinite(error) || !std::isfinite(derivative) || !std::isfinite(integral)) {
        result.fault = MotorFault::PID_NONFINITE;
        return result;
    }
    integral = std::clamp(integral, -integral_limit, integral_limit);
    const double proportional = static_cast<double>(config.kp) * error;
    const double differential = static_cast<double>(config.kd) * derivative;
    double raw = proportional + integral + differential;
    if (!std::isfinite(raw)) {
        result.fault = MotorFault::PID_NONFINITE;
        return result;
    }
    // Freeze only integral changes that push farther into saturation. This
    // uses the integral contribution's sign, so negative Ki is also supported.
    if ((raw > limit && delta_integral > 0.0) || (raw < -limit && delta_integral < 0.0)) {
        integral = config.ki == 0 ? 0.0 : integral_term_;
        raw = proportional + integral + differential;
    }
    if (!std::isfinite(raw)) {
        result.fault = MotorFault::PID_NONFINITE;
        return result;
    }
    const double output = std::clamp(raw, -limit, limit);
    const bool saturated = std::abs(raw) >= limit;
    if (!saturated) {
        saturation_cycles_ = 0;
    } else if (!has_previous_ || std::abs(error) < std::abs(previous_error_)) {
        saturation_cycles_ = 1;
    } else if (saturation_cycles_ < kSaturationFaultCycles) {
        ++saturation_cycles_;
    }
    integral_term_ = integral;
    previous_error_ = error;
    has_previous_ = true;
    result.saturation_cycles = saturation_cycles_;
    if (saturation_cycles_ >= kSaturationFaultCycles) {
        result.fault = MotorFault::PID_SATURATION;
        return result;
    }
    result.output_ma = output;
    return result;
}

} // namespace marshland::atom05
