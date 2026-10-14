// ============================================================================
// can_core.hpp — ATOM-04 CAN 内核（组合传输 + 投递 + 节点状态 + 调度）
//
// 权威来源：Design/Atomic/ATOM-04_CAN.md 原子操作第 1~8 条与验收条件
//           Design/Config.md §4（周期）、§6.3（零力矩/全机静默）、§15（线程与调度）
//
// 结构：
//   ICanTransport（可替换后端）
//     -> ProtocolDispatcher（定点投递 + 节点在线状态）
//       -> CanCore（生命周期、RX/TX 线程、零力矩安全发送）
//
// 时钟：**严格不可注入的内部单调时钟**（用户已裁决）。
//   * 周期与超时一律使用 std::chrono::steady_clock；
//   * 依据 Config.md §15 使用**绝对期限**推进，不使用"睡固定时长"；
//   * 禁止墙上时钟。
//
// 依赖：Atoms/Public/CanTypes.hpp、CanInterfaces.hpp 与 C++ 标准库。
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "CanInterfaces.hpp"
#include "CanTypes.hpp"
#include "atom04_can/can_config.hpp"
#include "atom04_can/motor_frame_codec.hpp"
#include "atom04_can/protocol_dispatcher.hpp"

namespace marshland::atom04 {

class CanCore final {
public:
    struct Dependencies final {
        marshland::can::ICanTransport& transport;
        marshland::can::CanNodeStatusStore& node_store;
        marshland::can::ICanFrameSink& frame_sink;
        marshland::can::ICanEventSink* event_sink{nullptr};
        marshland::can::ICanSafetyNotifier* safety_notifier{nullptr};
    };

    explicit CanCore(const Dependencies& dependencies) noexcept;
    ~CanCore();

    CanCore(const CanCore&) = delete;
    CanCore& operator=(const CanCore&) = delete;

    // 打开 CAN 接口、登记节点并启动 RX/TX 线程。失败返回 false 并写入 error_detail。
    bool start(const CanRuntimeConfig& config, std::string& error_detail) noexcept;

    // 幂等、noexcept；对"从未成功 start"的调用安全。
    void stop() noexcept;

    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    // ---- 力矩输出阶段门控（缺陷 #1 修复核心）-------------------------------
    // 默认 ZERO_TORQUE（fail-safe）。发送线程每周期**只**执行当前唯一模式：
    //   ZERO_TORQUE -> 发送 0x200/0x1FF 全零帧
    //   CONTROL     -> 发送已锁存的控制量
    // 两种帧共用同一对 CAN ID，因此绝不允许在同一周期并存。
    // 安全模块必须在进入/退出的每个阶段显式切换；切换时锁存控制量先归零，
    // 避免残余控制量在授权瞬间被发出。
    void setTorqueOutputMode(marshland::can::TorqueOutputMode mode) noexcept;
    marshland::can::TorqueOutputMode torqueOutputMode() const noexcept;

    // 锁存一组电机电流（按槽位顺序），由发送线程按周期发出。
    // 语义：不立即发送、不返回发送结果；下一次发送周期生效。
    // 注意：只有在 TorqueOutputMode::CONTROL 下才会真正送到总线。
    bool latchMotorCurrents(std::uint32_t frame_id, const MotorSlotValues& currents) noexcept;

    // 查询当前锁存的槽位值（诊断用）。
    bool latchedCurrents(std::uint32_t frame_id, MotorSlotValues& out) const noexcept;

    // 立即把全部槽位置零（安全路径）。锁存值被清零后，发送线程在同一周期
    // 内发送的即为 8 字节全零帧。
    void forceZeroCurrents() noexcept;

    // 发送一组零力矩帧（0x200 / 0x1FF，8 字节全零）。
    // 任一帧失败都调用 safety_notifier（若已注入）并发布 ZERO_TORQUE_FAILED 事件。
    // 注意：无论锁存值如何，本方法发送的都是全零帧（安全动作优先）。
    marshland::can::CanError sendZeroTorque() noexcept;

    // 立即发送当前锁存的控制帧，并**返回真实发送结果**（缺陷 #2 修复）：
    // 任一帧发送失败都返回首个失败错误，绝不虚报成功。
    // 仅在 TorqueOutputMode::CONTROL 下发送；其他模式返回当前模式对应的
    // 安全帧发送结果（ZERO_TORQUE 模式下发零力矩帧）。
    marshland::can::CanError flushLatchedFrames() noexcept;

    marshland::can::CanBusStatus busStatus() const noexcept;
    const CanRuntimeConfig& config() const noexcept { return config_; }

private:
    // 内部单调时钟：只用于周期与超时，严格不可注入。
    using Clock = std::chrono::steady_clock;

    static std::uint64_t nowMicroseconds() noexcept;

    void receiveLoop() noexcept;
    void transmitLoop() noexcept;

    void publishEvent(const marshland::can::CanEvent& event) noexcept;
    bool shouldPublishBusError(marshland::can::CanError error) noexcept;
    void publishNodeEvent(const marshland::can::CanEvent& event) noexcept;
    void notifyZeroTorqueFailure(const marshland::can::CanIoResult& result) noexcept;
    marshland::can::CanError sendLatchedFrames() noexcept;
    marshland::can::CanBusStatus snapshotBusStatus() const noexcept;

    Dependencies dependencies_;
    CanRuntimeConfig config_{};
    ProtocolDispatcher dispatcher_;
    std::atomic<bool> running_{false};
    std::thread receive_thread_;
    std::thread transmit_thread_;
    std::atomic<std::uint64_t> unknown_id_count_{0};
    // 发布节流：持续总线错误不得引发事件风暴（缺陷 D7）。
    std::atomic<std::uint64_t> bus_error_count_{0};
    std::atomic<std::uint64_t> published_node_count_{0};
    std::atomic<int> last_published_error_{static_cast<int>(marshland::can::CanError::OK)};

    // 锁存的控制帧槽位值；以互斥锁保护（提交线程与发送线程并发）。
    mutable std::mutex latch_mutex_;
    std::array<MotorSlotValues, marshland::can::kMotorControlFrameCount> latched_{};

    // 力矩输出模式：默认 fail-safe 零力矩。
    std::atomic<marshland::can::TorqueOutputMode> torque_mode_{
        marshland::can::TorqueOutputMode::ZERO_TORQUE};
};

} // namespace marshland::atom04
