#include "atom07_servo_driver/angle_to_pulse.hpp"

#include <cmath>

namespace marshland::atom07 {

PulseConversion angleToOffTicks(double angle_deg,
                                int angle_min_deg,
                                int angle_max_deg,
                                int pulse_min_us,
                                int pulse_max_us,
                                int resolution_bits,
                                double pwm_frequency_hz) noexcept
{
    PulseConversion result{};

    // ---- 参数合法性：任何非法输入都返回失败，不做隐式钳位 -------------------
    if (!(angle_max_deg > angle_min_deg) || !(pulse_max_us > pulse_min_us)) {
        return result;
    }
    if (resolution_bits < 1 || resolution_bits > 16) {
        return result;
    }
    if (!(pwm_frequency_hz > 0.0)) {
        return result;
    }
    if (!(angle_deg >= static_cast<double>(angle_min_deg)) ||
        !(angle_deg <= static_cast<double>(angle_max_deg))) {
        return result; // 越界：REJECT_AND_WARN，调用方负责记录 W-CMD-001
    }

    const double span_deg = static_cast<double>(angle_max_deg - angle_min_deg);
    const double ratio = (angle_deg - static_cast<double>(angle_min_deg)) / span_deg;
    const double pulse_us = static_cast<double>(pulse_min_us) +
                            ratio * static_cast<double>(pulse_max_us - pulse_min_us);

    const double period_us = 1000000.0 / pwm_frequency_hz;
    if (!(pulse_us > 0.0) || pulse_us > period_us) {
        return result; // 脉宽与频率自相矛盾（例如 50 Hz 下超过 20000 us）
    }

    const double steps = static_cast<double>(1U << static_cast<unsigned>(resolution_bits));
    const double raw_ticks = std::round(pulse_us / period_us * steps);

    // 计数值上限为 steps - 1（12 位 -> 4095）；此处仅做数值保护，不改变角度语义。
    if (raw_ticks < 0.0 || raw_ticks >= steps) {
        return result;
    }

    result.ok = true;
    result.pulse_us = pulse_us;
    result.off_ticks = static_cast<std::uint16_t>(raw_ticks);
    return result;
}

} // namespace marshland::atom07
