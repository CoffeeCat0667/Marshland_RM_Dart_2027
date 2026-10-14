// ============================================================================
// servo_types.hpp — ATOM-07 模块内部类型
//
// 归属：本模块私有（Inc/atom07_servo_driver/），不是跨 ATOM 公共契约。
//   跨模块共享类型按用户裁决不新增（由 ATOM-10/11 各自声明窄接口、装配层适配）。
//
// 约束：
//   * 只依赖 C++ 标准库与 Atoms/Public；
//   * "最近接受的目标值"与"物理反馈"在类型与命名上严格区分：本文件只定义
//     目标值语义（WriteResult / lastAcceptedTarget），不提供任何反馈类型。
// ============================================================================
#pragma once

#include <cstdint>

namespace marshland::atom07 {

// ---------------------------------------------------------------------------
// 机构编号（来自 servo.channels[].mechanism）
// ---------------------------------------------------------------------------
enum class ServoMechanism : std::uint8_t {
    Unknown,
    Reload, // 遗留：换弹已改为双 M3508，不产生 PWM 输出
    Feed,   // 供弹舵机（配置通道 4）
    Fire,   // 发射舵机（配置通道 5）
};

inline const char* ToString(ServoMechanism mechanism) noexcept
{
    switch (mechanism) {
    case ServoMechanism::Unknown: return "UNKNOWN";
    case ServoMechanism::Reload:  return "RELOAD";
    case ServoMechanism::Feed:    return "FEED";
    case ServoMechanism::Fire:    return "FIRE";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 写入结果状态（显式枚举；禁止用异常吞掉硬件错误）
// ---------------------------------------------------------------------------
enum class ServoStatus : std::uint8_t {
    Ok,                // 已成功写入 PCA9685A 通道
    NotReady,          // 未初始化 / 驱动未就绪 / 总线未打开
    ChannelNotWired,   // servo.channels[].channel 为 null（未接入），禁止推断
    ChannelInvalid,    // 通道号不在 servo.pca9685.channel_range 内
    AngleOutOfRange,   // 角度越界（out_of_range_action = REJECT_AND_WARN）
    EstopRejected,     // 软件急停锁存期间拒绝普通目标
    FaultRejected,     // IIC 故障状态期间拒绝普通目标（comm_fault_servos = HOLD）
    IicError,          // 总线写失败（短写 / errno）
    Timeout,           // 单次 I2C 事务超过 timing.iic_comm_timeout_ms
    InvalidArgument,   // 参数非法（与配置无关的调用错误）
};

inline const char* ToString(ServoStatus status) noexcept
{
    switch (status) {
    case ServoStatus::Ok:              return "OK";
    case ServoStatus::NotReady:        return "NOT_READY";
    case ServoStatus::ChannelNotWired: return "CHANNEL_NOT_WIRED";
    case ServoStatus::ChannelInvalid:  return "CHANNEL_INVALID";
    case ServoStatus::AngleOutOfRange: return "ANGLE_OUT_OF_RANGE";
    case ServoStatus::EstopRejected:   return "ESTOP_REJECTED";
    case ServoStatus::FaultRejected:   return "FAULT_REJECTED";
    case ServoStatus::IicError:        return "IIC_ERROR";
    case ServoStatus::Timeout:         return "TIMEOUT";
    case ServoStatus::InvalidArgument: return "INVALID_ARGUMENT";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 一次目标写入的结果（目标值语义，非物理反馈）
// ---------------------------------------------------------------------------
struct WriteResult final {
    ServoStatus status{ServoStatus::NotReady};
    int servo_id{0};
    int channel{-1};
    double angle_deg{0.0};
    std::uint16_t off_ticks{0};

    bool ok() const noexcept { return status == ServoStatus::Ok; }
};

// ---------------------------------------------------------------------------
// IIC 故障事实（供上层查询；历史记录由 ATOM-18 负责）
// ---------------------------------------------------------------------------
enum class ServoFaultKind : std::uint8_t {
    None,
    InitFailed,        // 打开设备 / 初始化 / 回读校验失败
    IicError,          // 事务失败
    Timeout,           // 事务超时
};

inline const char* ToString(ServoFaultKind kind) noexcept
{
    switch (kind) {
    case ServoFaultKind::None:       return "NONE";
    case ServoFaultKind::InitFailed: return "INIT_FAILED";
    case ServoFaultKind::IicError:   return "IIC_ERROR";
    case ServoFaultKind::Timeout:    return "TIMEOUT";
    }
    return "UNKNOWN";
}

struct ServoFault final {
    ServoFaultKind kind{ServoFaultKind::None};
    int servo_id{0};            // 0 = 与具体舵机无关（初始化 / 广播）
    int channel{-1};            // -1 = 无关联通道
    int system_error{0};        // errno 或 0
    std::int64_t timestamp_us{0}; // 单调时钟（来自注入的 IMonotonicClock）

    bool active() const noexcept { return kind != ServoFaultKind::None; }
};

} // namespace marshland::atom07
