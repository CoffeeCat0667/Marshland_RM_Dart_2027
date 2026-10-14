// ============================================================================
// servo_core.hpp — ATOM-07 内核（纯逻辑 + 独立 IIC 线程）
//
// 内部原子操作 4~9 的落地：
//   4. 校验目标是否落在配置范围内
//   5. 角度 -> PWM/脉宽输出目标（angle_to_pulse）
//   6. 单通道写入（经 Pca9685Device -> II2cBus）
//   7. 查询最近接受的目标值（目标值语义，不冒充物理反馈）
//   8. 记录并发布一次 IIC 故障结果
//   9. 急停/通信异常期间拒绝新的普通角度目标（本模块自身拒绝，不依赖调用方查闸门）
//
// 寻址方式（本轮裁决）：既支持按 servo.channels[].id，也支持按 mechanism。
//   发射舵机的 id 在配置中为 null，必须能按 mechanism = FIRE 寻址。
//
// 线程模型（用户裁决 #5：独立 IIC 线程）：
//   * 唯一的工作线程拥有全部总线事务 -> 天然串行化（access_mode = SERIALIZED）；
//   * 调用方线程只做"闸门判定 + 入队 + 等待结果"，不直接访问 I2C；
//   * 持 StateReadView 租约期间不做任何 I/O（在入队之前就已释放）。
//
// 依赖：只包含 Atoms/Public 契约（AppInterfaces.hpp / IMonotonicClock.hpp /
//       IConfigView.hpp）与本模块私有头；禁止包含任何 ATOM 私有头。
// ============================================================================
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "AppInterfaces.hpp"
#include "IMonotonicClock.hpp"
#include "atom07_servo_driver/pca9685_device.hpp"
#include "atom07_servo_driver/servo_config.hpp"
#include "atom07_servo_driver/servo_types.hpp"

namespace marshland::atom07 {

class ServoCore final {
public:
    static constexpr std::size_t kMaxTrackedSlots = 16U;

    // input_reporter / state_reader 均为可空公共窄端口（装配层注入；同 ATOM-03 模式）。
    ServoCore(II2cBus& bus,
              const marshland::app::IMonotonicClock& clock,
              marshland::app::IInputReporter* input_reporter = nullptr,
              const marshland::app::IStateReader* state_reader = nullptr) noexcept;
    ~ServoCore() noexcept;

    ServoCore(const ServoCore&) = delete;
    ServoCore& operator=(const ServoCore&) = delete;
    ServoCore(ServoCore&&) = delete;
    ServoCore& operator=(ServoCore&&) = delete;

    // 初始化设备并启动独立 IIC 线程；失败返回 false（不抛异常）。
    bool start(const ServoConfig& config) noexcept;
    // 幂等、noexcept；停止线程并释放设备状态。
    void stop() noexcept;

    bool isReady() const noexcept { return ready_.load(); }

    // 写单舵机目标角度（按 servo.channels[].id 寻址）；返回显式状态（目标值语义）。
    WriteResult writeTarget(int servo_id, double angle_deg) noexcept;

    // 写单舵机目标角度（按 mechanism 寻址；发射舵机 id 为 null 时必须用本重载）。
    WriteResult writeTarget(ServoMechanism mechanism, double angle_deg) noexcept;

    // 最近一次成功接受的目标值；从未成功则返回 nullopt（目标值，非物理反馈）。
    std::optional<double> lastAcceptedTarget(int servo_id) const noexcept;
    std::optional<double> lastAcceptedTarget(ServoMechanism mechanism) const noexcept;

    // 最近一次 IIC 故障事实（历史记录由 ATOM-18 负责）。
    ServoFault lastIicFault() const noexcept;

    // 由上层故障恢复流程在根因清除后调用；清除后重新接受普通目标。
    void resetFault() noexcept;

    const ServoConfig& config() const noexcept { return config_; }

private:
    struct Request final {
        int servo_id{0};
        int channel{-1};
        std::uint16_t off_ticks{0};
        ServoStatus status{ServoStatus::NotReady};
        bool done{false};
    };

    void run() noexcept;
    ServoStatus gateStatus() const noexcept;
    void recordFault(ServoFaultKind kind, int servo_id, int channel, int system_error) noexcept;
    void forgetFault() noexcept;

    bool locateSlot(int servo_id, const ServoSlotConfig*& slot, std::size_t& index) const noexcept;
    bool locateSlot(ServoMechanism mechanism, const ServoSlotConfig*& slot,
                    std::size_t& index) const noexcept;
    WriteResult writeLocated(const ServoSlotConfig& slot, std::size_t index, double angle_deg) noexcept;

    II2cBus& bus_;
    const marshland::app::IMonotonicClock& clock_;
    marshland::app::IInputReporter* input_reporter_{nullptr};
    const marshland::app::IStateReader* state_reader_{nullptr};

    ServoConfig config_{};
    std::optional<Pca9685Device> device_{};

    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::thread worker_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable done_cv_;
    std::deque<std::shared_ptr<Request>> queue_;

    mutable std::mutex target_mutex_;
    std::array<std::optional<double>, kMaxTrackedSlots> last_accepted_{};

    mutable std::mutex fault_mutex_;
    ServoFault fault_{};
};

} // namespace marshland::atom07
