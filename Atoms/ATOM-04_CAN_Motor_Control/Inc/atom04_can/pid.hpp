// ============================================================================
// pid.hpp — 单电机位置式 PID（转速误差 -> 电流指令）
//
// 用途：CAN 接收线程按 20ms 驱动周期调用 update()，把「目标转速 − 实测转速」
//       的误差换算为电调电流指令，由调用方组帧后经 0x200 / 0x1FF 发出。
//
// 设计要点：
//   * 位置式：输出 = kp*e + ki*∫e + kd*d(测量)/dt，再按 [min,max] 限幅；
//   * 微分作用在**测量值**上而非误差上，避免目标转速阶跃时产生微分冲击
//     （derivative kick）——调用方 setRpm() 的阶跃是常态；
//   * 抗积分饱和：把积分项单独限制在输出限幅内，防止饱和期间积分无限累积；
//   * 第一个周期微分项为 0（无历史测量可差分），避免启动尖峰；
//   * **不是线程安全的**：必须由单一线程持有并调用。在 ATOM-04 中它的唯一
//     调用者是 CAN 接收线程。
//
// 依赖：仅 C++ 标准库。
// ============================================================================
#pragma once

namespace marshland::atom04 {

class Pid final {
public:
    struct Gains final {
        double kp{0.0};
        double ki{0.0};
        double kd{0.0};
        double max{0.0};   // 输出上限（电流指令）
        double min{0.0};   // 输出下限（电流指令）
    };

    Pid() noexcept = default;

    // period_s 为采样/驱动周期（秒）；非正值回退到 0.02 s（20ms）。
    // 配置会清空积分与微分历史。
    void configure(const Gains& gains, double period_s) noexcept;

    // 清空积分与微分历史（重新使能或模式切换时使用）。
    void reset() noexcept;

    // 返回限幅到 [min,max] 的输出。限幅非法（max <= min）时恒返回 0。
    double update(double setpoint, double measured) noexcept;

    const Gains& gains() const noexcept { return gains_; }

private:
    Gains gains_{};
    double period_s_{0.02};
    double integral_{0.0};
    double previous_measured_{0.0};
    bool has_previous_{false};
};

} // namespace marshland::atom04
