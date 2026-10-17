// ============================================================================
// CanInterfaces.hpp — CAN 跨 ATOM 公共端口契约
//
// 归属：Atoms/Public/，由 ATOM-04_CAN 声明，装配层把实现方适配到这些端口。
//
// 本文件只保留**装配层与其它 ATOM 必须实现**的端口：
//   ICanFrameSink       ATOM-05 接收原始反馈帧（唯一通道）
//   ICanEventSink       ATOM-18/14 接收总线与节点事件
//   ICanSafetyNotifier  ATOM-14 接收零力矩发送失败（F-CAN-002 全机静默）
// ICanTransport 归内核内部（atom04_can/can_transport.hpp），不在公共面。
//
// 依赖方向：仅 Atoms/Public/CanTypes.hpp 与 C++ 标准库。
// 分工边界：ATOM-04 只做"接收原始帧 -> 按 ID 投递"，不解析反馈字段。
// ============================================================================
#pragma once

#include "CanTypes.hpp"

namespace marshland::can {

// ---------------------------------------------------------------------------
// ICanFrameSink —— 原始 CAN 帧投递端口（ATOM-04 -> ATOM-05）
//
// 0x201~0x206 的**原始帧**原样交付，不做任何字段解析。
// 约束：实现必须 noexcept，且不得阻塞接收线程超过一个控制周期。
// ---------------------------------------------------------------------------
class ICanFrameSink {
public:
    virtual ~ICanFrameSink() = default;
    virtual void onCanFrame(const CanFrame& frame) noexcept = 0;
};

// ---------------------------------------------------------------------------
// ICanEventSink —— 总线/节点事件发布端口
//
// CAN 传输层没有日志依赖，因此事件以**原子值 + 显式类别**表达，由实现方
// 决定如何映射到诊断/故障通道（ATOM-18 / ATOM-14）。不在公共契约里携带
// 时间戳或复合结构，避免把诊断格式绑进 CAN 契约。
// ---------------------------------------------------------------------------
enum class CanEventKind : std::uint8_t {
    BUS_ERROR = 0,        // 总线错误（含 BUS_OFF、发送失败）
    NODE_STATE_CHANGE,    // 节点在线状态跃变
    ZERO_TORQUE_FAILED,   // 零力矩帧发送失败 -> F-CAN-002 全机静默
};

inline const char* toString(CanEventKind kind) noexcept
{
    switch (kind) {
    case CanEventKind::BUS_ERROR:          return "BUS_ERROR";
    case CanEventKind::NODE_STATE_CHANGE:  return "NODE_STATE_CHANGE";
    case CanEventKind::ZERO_TORQUE_FAILED: return "ZERO_TORQUE_FAILED";
    }
    return "UNKNOWN";
}

class ICanEventSink {
public:
    virtual ~ICanEventSink() = default;

    // error: 触发事件的错误（NODE_STATE_CHANGE 时为 OK）
    // detail: 该类别下的细分值 —— 节点事件为 node_id，其余为触发次数
    virtual void onCanEvent(CanEventKind kind, CanError error, std::uint32_t detail) noexcept = 0;
};

// ---------------------------------------------------------------------------
// ICanSafetyNotifier —— 零力矩发送失败的安全上报端口
//
// 依据 Design/Config.md §6.3 与 §14.2（F-CAN-002）：零力矩帧发送失败时进入
// 错误状态并**全机静默**。ATOM-04 不做安全编排，只如实上报；具体静默动作
// 由装配层绑定到 ATOM-14。
//
// 未绑定时 ATOM-04 仍然向 ICanEventSink 发布 ZERO_TORQUE_FAILED，不静默吞掉失败。
// ---------------------------------------------------------------------------
class ICanSafetyNotifier {
public:
    virtual ~ICanSafetyNotifier() = default;

    virtual void onZeroTorqueSendFailed(CanError error, int system_errno) noexcept = 0;
};

} // namespace marshland::can
