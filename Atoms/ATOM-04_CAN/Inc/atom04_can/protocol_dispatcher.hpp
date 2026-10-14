// ============================================================================
// protocol_dispatcher.hpp — 原始帧定点投递 + 节点在线状态（纯逻辑内核）
//
// 权威来源：Design/Atomic/ATOM-04_CAN.md 原子操作第 3、5、6 条与验收条件
//           Design/Config.md §5.2（200 ms 离线判定）、§6.2（心跳/错误状态行）
//
// 分工边界（用户已裁决）：ATOM-04 **不解析反馈帧**，只把 0x201~0x206 的原始帧
// 原样投递给 ICanFrameSink（ATOM-05 实现）。本文件不做任何字段/单位/错误码解释。
//
// 依赖：Atoms/Public/CanTypes.hpp、CanInterfaces.hpp 与配置结构。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include "CanInterfaces.hpp"
#include "CanTypes.hpp"
#include "atom04_can/can_config.hpp"

namespace marshland::atom04 {

// 单帧处理结果，供接收侧决定是否发布事件。
struct DispatchOutcome final {
    marshland::can::CanError error{marshland::can::CanError::OK};
    bool node_became_online{false};   // UNKNOWN/TIMEOUT/FAULT -> ONLINE
    std::uint32_t node_id{0};
    int motor_id{0};
};

class ProtocolDispatcher final {
public:
    ProtocolDispatcher(marshland::can::ICanFrameSink& sink,
                       marshland::can::CanNodeStatusStore& store) noexcept
        : sink_(sink), store_(store)
    {
    }

    ProtocolDispatcher(const ProtocolDispatcher&) = delete;
    ProtocolDispatcher& operator=(const ProtocolDispatcher&) = delete;
    ~ProtocolDispatcher() = default;

    void configure(const CanRuntimeConfig& config) noexcept { config_ = config; }

    // 初始化阶段登记全部受控节点为 UNKNOWN（尚未收到任何反馈）。
    bool registerNodes() noexcept;

    // 处理一个已收到的原始帧。
    //   * 帧结构非法（DLC != 8）：返回 FRAME_INVALID，**不刷新在线状态、不投递**；
    //   * 0x201~0x206 且 DLC==8：刷新节点在线状态并原样投递给 sink；
    //   * 其他 ID（含 0x200/0x1FF 回显）：返回 UNKNOWN_ID，不投递、不臆造字段。
    // 状态不变量：FAULT 为粘滞终态——故障节点仍接收并转发帧（否则 ATOM-05 看不到
    // 恢复尝试），但其 state 不会被反馈或超时轮询改回 ONLINE。解除故障必须显式调用
    // markOnline() 或重新 registerNodes()。
    DispatchOutcome onReceived(const marshland::can::CanFrame& frame) noexcept;

    // 按 timings.can_comm_timeout_ms 判定超时；返回本次由 ONLINE 转为 TIMEOUT 的节点数。
    std::size_t pollTimeouts(std::uint64_t now_us,
                             marshland::can::CanNodeStatus* expired_output,
                             std::size_t capacity) noexcept;

    // 把某节点标记为故障（例如设备上报错误状态；错误码语义归 ATOM-05）。
    // 故障为粘滞状态，需显式 markOnline() 才能解除。
    bool markFault(std::uint32_t node_id) noexcept;

    // 显式解除某节点的故障（返回其是否原处于 FAULT）。
    // 调用方必须是授权路径（故障清除流程），不得由普通反馈路径隐式调用。
    bool markOnline(std::uint32_t node_id, std::uint64_t now_us) noexcept;

    const CanRuntimeConfig& config() const noexcept { return config_; }

private:
    const MotorEndpoint* findMotor(std::uint32_t node_id) const noexcept;

    marshland::can::ICanFrameSink& sink_;
    marshland::can::CanNodeStatusStore& store_;
    CanRuntimeConfig config_{};
    std::uint64_t timeout_us_{0};
};

} // namespace marshland::atom04
