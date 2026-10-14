// ============================================================================
// CanInterfaces.hpp — CAN 跨 ATOM 公共端口契约
//
// 归属说明：
//   本文件是**跨模块契约**，与 SbusInterfaces.hpp / IConfigView.hpp /
//   ILifecycle.hpp 同级，置于 Atoms/Public/。
//   由 ATOM-04_CAN 声明，由 ASSEMBLY（装配层）把实现方适配到这些端口。
//
// 权威来源：
//   Design/Atomic/ATOM-04_CAN.md  "CAN 传输原子操作"1~8 条与验收条件
//   Design/Atomic/ATOM-05_Motor_PID.md  "从一帧 CAN 反馈更新单个电机状态"
//   Rule/AtomIntegration.md       §6.2 优先接口注入
//
// 依赖方向（不可协商）：
//   仅依赖 Atoms/Public/CanTypes.hpp 与 C++ 标准库。
//   禁止包含 AppCoordinator.hpp、任何 ATOM 私有头、任何 SocketCAN 平台头。
//
// 分工边界（用户已裁决）：
//   ATOM-04 只做"接收一个原始帧 -> 按 ID 投递"。反馈字段布局、单位与错误码
//   映射全部归 ATOM-05；ATOM-04 不解析、不缓存、不解释任何反馈字段。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include "CanTypes.hpp"

namespace marshland::can {

// ---------------------------------------------------------------------------
// ICanTransport —— 可替换的传输后端
//
// 实现方：atom04_can 的 LinuxSocketCanTransport（SocketCAN）。
// 语义要求：
//   * open() 必须在内核确认接口可用后才返回成功；
//   * receive() 无帧可读时必须返回 RECEIVE_TIMEOUT，禁止返回 OK；
//   * send() 只有在**内核确认完整写入**后才能返回 OK（禁止虚报成功）；
//   * close() 必须幂等，且必须能打断阻塞中的 receive()；
//   * 所有方法 noexcept；错误一律通过 CanIoResult 表达。
// ---------------------------------------------------------------------------
class ICanTransport {
public:
    virtual ~ICanTransport() = default;

    virtual CanIoResult open(const char* interface_name) noexcept = 0;
    virtual void close() noexcept = 0;

    // 阻塞至多 timeout_ms 毫秒；成功时填充 out。
    virtual CanIoResult receive(CanFrame& out, int timeout_ms) noexcept = 0;

    virtual CanIoResult send(const CanFrame& frame) noexcept = 0;

    virtual CanBusStatus busStatus() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// ICanFrameSink —— 原始 CAN 帧投递端口（ATOM-04 -> ATOM-05）
//
// 这是"反馈帧交给 ATOM-05"的唯一通道：ATOM-04 把 0x201~0x206 的**原始帧**
// 原样交付，不解析任何字段。实现方为 ATOM-05（或其适配器），由装配层绑定。
//
// 约束：
//   * 禁止在实现内回调 ATOM-04（避免形成环）；
//   * 实现必须 noexcept 且不得阻塞接收线程超过一个控制周期；
//   * ATOM-04 不校验本端口是否已绑定；未绑定即不投递。
// ---------------------------------------------------------------------------
class ICanFrameSink {
public:
    virtual ~ICanFrameSink() = default;
    virtual void onCanFrame(const CanFrame& frame) noexcept = 0;
};

// ---------------------------------------------------------------------------
// ICanEventSink —— 总线/节点错误变化事件发布端口
//
// 对应 ATOM-04 原子操作第 6 条"发布一次 CAN 总线或节点错误变化"。
// 装配层可把该事件桥接到 ATOM-18 诊断或 ATOM-14 安全流程（见待接入点）。
// ---------------------------------------------------------------------------
class ICanEventSink {
public:
    virtual ~ICanEventSink() = default;
    virtual void onCanEvent(const CanEvent& event) noexcept = 0;
};

// ---------------------------------------------------------------------------
// ICanNodeStatusReader —— 节点/总线状态读取端口
//
// 供上层查询 CAN 节点在线状态（Design/Config.md §5.2 的 200 ms 离线判定）。
// 反馈字段不在本端口内，见 ICanFrameSink。
// ---------------------------------------------------------------------------
class ICanNodeStatusReader {
public:
    virtual ~ICanNodeStatusReader() = default;
    virtual std::size_t snapshotNodes(CanNodeStatus* output, std::size_t capacity) const noexcept = 0;
    virtual CanBusStatus busStatus() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// TorqueOutputMode —— ATOM-04 的力矩输出阶段（由安全模块授权切换）
//
// 为什么必须有这个门控（缺陷 #1）：
//   零力矩帧与控制帧使用**同一对 CAN ID**。若发送线程每周期无条件发送零力矩帧，
//   它就会与控制帧在总线上逐帧交替，等效于每个周期给电机发一次零电流命令，
//   电机无法连续出力。因此"是否发零力矩"必须由安全状态决定，而不是由发送线程决定。
//
// 约定：
//   * 默认值为 ZERO_TORQUE（fail-safe）：任何未显式授权的系统都只能输出零力矩；
//   * 切换由装配层依据安全状态调用 ICanTorqueGate 的授权方；
//   * 两种模式**互斥**，同一周期只发一类帧，永不并存。
// ---------------------------------------------------------------------------
enum class TorqueOutputMode : std::uint8_t {
    ZERO_TORQUE = 0,   // 发送全零帧（急停锁定、通信异常、待机、安全恢复未授权出力）
    CONTROL,           // 发送已锁存的控制量（安全恢复授权出力 / 正常运行）
};

inline const char* toString(TorqueOutputMode mode) noexcept
{
    switch (mode) {
    case TorqueOutputMode::ZERO_TORQUE: return "ZERO_TORQUE";
    case TorqueOutputMode::CONTROL:     return "CONTROL";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// ICanSafetyNotifier —— 零力矩发送失败的安全上报端口
//
// 依据 Design/Config.md §6.3 与 §14.2（F-CAN-002）：零力矩帧发送失败时进入
// 错误状态并**全机静默**。ATOM-04 不做安全编排，只如实上报；具体静默动作
// 由装配层绑定到安全模块（ATOM-14）。
//
// 声明存在，接线未落地 —— 见 PLAN.md 待接入点 P-01。
// 未绑定时 ATOM-04 仍然发布 ZERO_TORQUE_FAILED 事件，不得静默吞掉失败。
// ---------------------------------------------------------------------------
class ICanSafetyNotifier {
public:
    virtual ~ICanSafetyNotifier() = default;
    virtual void onZeroTorqueSendFailed(const CanIoResult& result) noexcept = 0;
};

} // namespace marshland::can
