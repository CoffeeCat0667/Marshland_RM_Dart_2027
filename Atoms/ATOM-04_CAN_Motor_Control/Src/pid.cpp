#include "atom04_can/pid.hpp"

namespace marshland::atom04 {

namespace {

constexpr double kDefaultPeriodSeconds = 0.02;  // 20 ms

double clamp(double value, double low, double high) noexcept
{
    if (value > high) {
        return high;
    }
    if (value < low) {
        return low;
    }
    return value;
}

} // namespace

void Pid::configure(const Gains& gains, double period_s) noexcept
{
    gains_ = gains;
    period_s_ = (period_s > 0.0) ? period_s : kDefaultPeriodSeconds;
    reset();
}

void Pid::reset() noexcept
{
    integral_ = 0.0;
    previous_measured_ = 0.0;
    has_previous_ = false;
}

double Pid::update(double setpoint, double measured) noexcept
{
    if (!(gains_.max > gains_.min)) {
        // 限幅非法：宁可不出力，也不发出没有依据的电流指令。
        // start() 已拒绝这种参数，这里是二次防御。
        return 0.0;
    }

    const double error = setpoint - measured;

    // 微分作用在测量值上：d(error)/dt ≈ -d(measured)/dt（目标值分段常值）。
    const double derivative =
        has_previous_ ? (-(measured - previous_measured_) / period_s_) : 0.0;
    previous_measured_ = measured;
    has_previous_ = true;

    integral_ += error * period_s_;

    // 抗积分饱和：把积分项单独限制在输出限幅内，防止饱和时积分继续累积。
    if (gains_.ki != 0.0) {
        const double integral_term = gains_.ki * integral_;
        if (integral_term > gains_.max) {
            integral_ = gains_.max / gains_.ki;
        } else if (integral_term < gains_.min) {
            integral_ = gains_.min / gains_.ki;
        }
    } else {
        integral_ = 0.0;  // ki == 0：不保留积分，避免无意义累积
    }

    const double output =
        (gains_.kp * error) + (gains_.ki * integral_) + (gains_.kd * derivative);
    return clamp(output, gains_.min, gains_.max);
}

} // namespace marshland::atom04
