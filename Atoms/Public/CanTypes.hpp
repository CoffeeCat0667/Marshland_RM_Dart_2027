// ============================================================================
// CanTypes.hpp — CAN/SocketCAN 跨 ATOM 公共契约类型
//
// 归属：Atoms/Public/，与 ILifecycle.hpp / SbusTypes.hpp 同级。
// 生产方：ATOM-04_CAN。消费方：ATOM-05（电机反馈）、ATOM-14（安全）、装配层。
//
// 权威来源：
//   Design/Config.md §5.1/§6.1/§6.2/§6.3/§14.2
//   Design/Atomic/ATOM-04_CAN.md
//   Config/dart_control.json（motor[].id / motor[].node、can.zero_torque）
//
// 依赖方向（不可协商）：仅依赖 C++ 标准库。
// 禁止包含 AppCoordinator.hpp、任何 ATOM 私有头、任何 SocketCAN 平台头。
//
// 不作为：不定义反馈帧字段布局/单位/错误码映射（归 ATOM-05）。
// ============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace marshland::can {

// ---------------------------------------------------------------------------
// 帧常量与槽位契约（Design/Config.md §6.2）
//
//   0x200 -> node 0x201、0x202、0x203、0x204（电机 1..4）
//   0x1FF -> node 0x205、0x206（电机 5、6）；其余两个槽位未使用
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kMotorControlFrameIdA = 0x200U;
inline constexpr std::uint32_t kMotorControlFrameIdB = 0x1FFU;
inline constexpr std::size_t kCanPayloadBytes = 8U;
inline constexpr std::size_t kMotorSlotsPerFrame = 4U;

inline constexpr std::uint32_t kMotorNodeMin = 0x201U;
inline constexpr std::size_t kMaxMotorNodes = 6U;

// 一次提交的槽位载荷：4 个有符号 int16 电流值（大端序上线）。
using MotorSlotValues = std::array<std::int16_t, kMotorSlotsPerFrame>;

// ---------------------------------------------------------------------------
// 单帧表示（can_frame 的与平台无关镜像）
// can_id 已去掉 CAN_EFF_FLAG / CAN_RTR_FLAG / CAN_ERR_FLAG。
// ---------------------------------------------------------------------------
struct CanFrame final {
    std::uint32_t can_id{0};
    std::uint8_t dlc{0};
    std::array<std::uint8_t, kCanPayloadBytes> payload{};
    std::uint64_t timestamp_us{0};
};

// ---------------------------------------------------------------------------
// 错误模型（ATOM-04 验收条件：区分成功/超时/BUS_OFF/总线错误；失败不得虚报成功）
// ---------------------------------------------------------------------------
enum class CanError : std::uint8_t {
    OK = 0,
    NOT_OPEN,
    PARAM_INVALID,
    SEND_FAILED,
    SEND_INCOMPLETE,   // 内核未确认完整写入
    RECEIVE_TIMEOUT,
    BUS_OFF,
    BUS_ERROR,
    FRAME_INVALID,
    UNKNOWN_ID,
    UNSUPPORTED,
};

inline const char* toString(CanError error) noexcept
{
    switch (error) {
    case CanError::OK:              return "OK";
    case CanError::NOT_OPEN:        return "NOT_OPEN";
    case CanError::PARAM_INVALID:   return "PARAM_INVALID";
    case CanError::SEND_FAILED:     return "SEND_FAILED";
    case CanError::SEND_INCOMPLETE: return "SEND_INCOMPLETE";
    case CanError::RECEIVE_TIMEOUT: return "RECEIVE_TIMEOUT";
    case CanError::BUS_OFF:         return "BUS_OFF";
    case CanError::BUS_ERROR:       return "BUS_ERROR";
    case CanError::FRAME_INVALID:   return "FRAME_INVALID";
    case CanError::UNKNOWN_ID:      return "UNKNOWN_ID";
    case CanError::UNSUPPORTED:     return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

// 显式标注 failed 语义，便于调用方读作 if (result.failed)。
struct CanResult final {
    bool failed{false};
    CanError error{CanError::OK};
    int system_errno{0};   // 发送/接收失败时的 errno，用于定位到具体调用
};

// ---------------------------------------------------------------------------
// 力矩输出模式：由安全状态授权切换。
//
// 零力矩帧与控制帧使用**同一对 CAN ID**，因此每个周期只执行当前模式对应的
// 那一种，绝不并存。默认 ZERO_TORQUE（fail-safe）。
// ---------------------------------------------------------------------------
enum class TorqueOutputMode : std::uint8_t {
    ZERO_TORQUE = 0,   // 发送全零帧（急停锁定/通信异常/待机/未授权出力）
    CONTROL,           // 发送已提交的控制量（安全恢复授权出力/正常运行）
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
// 节点状态
//
// FAULT 为**粘滞终态**：设备上报/上层判定的故障不会被普通反馈或超时轮询清除，
// 必须经 CanCore::markOnline() 显式解除。
// ---------------------------------------------------------------------------
enum class CanNodeState : std::uint8_t {
    UNKNOWN = 0,
    ONLINE,
    TIMEOUT,
    FAULT,
};

inline const char* toString(CanNodeState state) noexcept
{
    switch (state) {
    case CanNodeState::UNKNOWN: return "UNKNOWN";
    case CanNodeState::ONLINE:  return "ONLINE";
    case CanNodeState::TIMEOUT: return "TIMEOUT";
    case CanNodeState::FAULT:   return "FAULT";
    }
    return "UNKNOWN";
}

struct CanNodeStatus final {
    int motor_id{0};
    std::uint32_t node_id{0};
    CanNodeState state{CanNodeState::UNKNOWN};
    bool has_received{false};        // 超时判定依据（不得用时间戳 0 代替）
    std::uint64_t last_frame_time_us{0};
    std::uint64_t frame_count{0};
};

// ---------------------------------------------------------------------------
// 对外发布的状态快照（一次性整体读取，互斥保护）
// ---------------------------------------------------------------------------
struct CanCoreSnapshot final {
    bool running{false};
    bool open{false};
    CanError last_error{CanError::OK};
    int last_errno{0};
    TorqueOutputMode mode{};
    std::uint64_t rx_count{0};
    std::uint64_t tx_count{0};
    std::uint64_t tx_fail_count{0};
    std::uint64_t unknown_id_count{0};
    std::array<CanNodeStatus, kMaxMotorNodes> nodes{};
    std::size_t node_count{0};
};


// ---------------------------------------------------------------------------
// 槽位契约查询（编译期可判定；ATOM-05 用它把电机映射到控制帧槽位）
// ---------------------------------------------------------------------------
struct MotorSlotQuery final {
    bool valid{false};
    std::uint32_t frame_id{0};
    std::size_t slot{0};
};

constexpr MotorSlotQuery queryMotorSlot(std::uint32_t node_id) noexcept
{
    if (node_id >= kMotorNodeMin && node_id < kMotorNodeMin + kMotorSlotsPerFrame) {
        return MotorSlotQuery{true, kMotorControlFrameIdA,
                              static_cast<std::size_t>(node_id - kMotorNodeMin)};
    }
    if (node_id >= 0x205U && node_id <= 0x206U) {
        return MotorSlotQuery{true, kMotorControlFrameIdB,
                              static_cast<std::size_t>(node_id - 0x205U)};
    }
    return MotorSlotQuery{false, 0U, 0U};
}

// 编译期固化 Design/Config.md §6.2 的槽位映射（唯一真相）
static_assert(queryMotorSlot(0x201U).frame_id == kMotorControlFrameIdA &&
              queryMotorSlot(0x201U).slot == 0U);  // 电机1 charge_right
static_assert(queryMotorSlot(0x202U).frame_id == kMotorControlFrameIdA &&
              queryMotorSlot(0x202U).slot == 1U);  // 电机2 charge_left
static_assert(queryMotorSlot(0x203U).frame_id == kMotorControlFrameIdA &&
              queryMotorSlot(0x203U).slot == 2U);  // 电机3 reload_right
static_assert(queryMotorSlot(0x204U).frame_id == kMotorControlFrameIdA &&
              queryMotorSlot(0x204U).slot == 3U);  // 电机4 reload_left
static_assert(queryMotorSlot(0x205U).frame_id == kMotorControlFrameIdB &&
              queryMotorSlot(0x205U).slot == 0U);  // 电机5 YAW
static_assert(queryMotorSlot(0x206U).frame_id == kMotorControlFrameIdB &&
              queryMotorSlot(0x206U).slot == 1U);  // 电机6 变力
static_assert(!queryMotorSlot(0x207U).valid);
static_assert(!queryMotorSlot(0x200U).valid);
// 帧 ID 与节点 ID 是两个不同的空间，不得互相混用：
//   0x200 / 0x1FF 是帧 ID（不是节点）；0x201~0x206 是节点。
static_assert(!queryMotorSlot(0x200U).valid);
static_assert(!queryMotorSlot(kMotorControlFrameIdB).valid);
static_assert(queryMotorSlot(0x201U).frame_id == kMotorControlFrameIdA);
static_assert(queryMotorSlot(0x206U).frame_id == kMotorControlFrameIdB);

} // namespace marshland::can
