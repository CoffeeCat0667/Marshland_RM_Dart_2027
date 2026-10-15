// ============================================================================
// fire_action.hpp — ATOM-11 纯逻辑内核（一次发射动作状态机）
//
// 职责（Design/Atomic/ATOM-11_Fire.md 的 6 个原子步骤）：
//   1. 校验急停 / 故障 / 授权结论与配置就绪；
//   2. 写入发射角目标；
//   3. 发射角保持计时（timing.fire_angle_hold_ms = 100 ms）；
//   4. 保持到期后写回待机角目标；
//   5. 仅待机目标写回成功才置 COMPLETED；
//   6. 恰好发布一次完成事件。
//
// 驱动模型（用户裁决）：外部 tick + 显式单调时间 now_us；内核不自建线程、
// 不睡眠、不假设回调准时。
//
// 安全约束：急停 / 故障事实传入时内核自身拒绝写入舵机目标，不把安全寄托于
// 调用方先查 RunGate()；动作进行中每个 tick 复检急停与故障，出现时不再写
// 任何新目标。
//
// 窄端口：
//   IServoAngleWriter  由适配层直接转发到
//                      marshland::atom07::ServoCore::writeTarget(
//                          ServoMechanism::Fire, ...)（用户裁决：直连 ATOM-07）；
//   IFireEventSink     完成事件与故障事实（W-CMD-001 / F-SERVO-001），
//                      由适配层转发给装配层绑定的事件出口。
// ============================================================================
#pragma once

#include <cstdint>

#include "atom11_fire/fire_config.hpp"
#include "atom11_fire/fire_facts.hpp"
#include "atom11_fire/fire_types.hpp"

namespace marshland::atom11 {

// 故障码（Design/Config.md §14.2）
inline constexpr const char* kFaultServoFailure = "F-SERVO-001";
inline constexpr const char* kFaultAngleRejected = "W-CMD-001";

class IServoAngleWriter {
public:
    virtual ~IServoAngleWriter() = default;
    // 返回是否被舵机底层接受（目标值语义，非物理反馈）。
    virtual bool writeFireTarget(double angle_deg) noexcept = 0;
};

class IFireEventSink {
public:
    virtual ~IFireEventSink() = default;
    virtual void onFireCompleted(std::uint64_t now_us) noexcept = 0;
    virtual void onFireFault(const char* fault_code, std::uint64_t now_us) noexcept = 0;
};

class FireActionCore final {
public:
    explicit FireActionCore(IServoAngleWriter& writer,
                            IFireEventSink* events = nullptr) noexcept
        : writer_(writer), events_(events) {}

    FireActionCore(const FireActionCore&) = delete;
    FireActionCore& operator=(const FireActionCore&) = delete;

    // 校验角度范围（0~180）与时序参数；失败则保持未配置，禁止动作。
    bool configure(const FireConfig& config) noexcept;
    bool configured() const noexcept { return configured_; }
    FireActionState state() const noexcept { return state_; }

    // 发起一次发射动作。仅 IDLE / COMPLETED / FAILED 可发起；
    // 动作进行中再次发起返回 REJECTED_NOT_READY（不自行重复发射）。
    FireStepOutcome begin(const FireFacts& facts, std::uint64_t now_us) noexcept;

    // 每个控制周期驱动保持计时与待机角写回。
    FireStepOutcome tick(const FireFacts& facts, std::uint64_t now_us) noexcept;

private:
    FireStepOutcome failWith(FireStepOutcome outcome, const char* fault_code,
                             std::uint64_t now_us) noexcept;
    void emitCompleted(std::uint64_t now_us) noexcept;

    IServoAngleWriter& writer_;
    IFireEventSink* events_;

    FireConfig config_{};
    bool configured_{false};
    FireActionState state_{FireActionState::IDLE};

    std::uint64_t start_us_{0};
    std::uint64_t hold_us_{0};
    std::uint64_t max_action_us_{0};
    bool completion_emitted_{false};
};

} // namespace marshland::atom11
