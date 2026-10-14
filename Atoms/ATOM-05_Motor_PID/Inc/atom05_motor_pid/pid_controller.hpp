#pragma once

#include "motor_types.hpp"

namespace marshland::atom05 {

struct PidStepResult final {
    double output_ma{0.0};
    MotorFault fault{MotorFault::NONE};
    std::uint8_t saturation_cycles{0};
};

class PidController final {
public:
    void reset() noexcept;
    PidStepResult step(const PidConfig& config, double target_rpm,
                      double speed_rpm, double dt_seconds,
                      double effective_limit_ma) noexcept;
    std::uint8_t saturationCycles() const noexcept { return saturation_cycles_; }
private:
    double integral_term_{0.0};
    double previous_error_{0.0};
    bool has_previous_{false};
    std::uint8_t saturation_cycles_{0};
};

} // namespace marshland::atom05
