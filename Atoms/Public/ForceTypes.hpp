#ifndef MARSHLAND_PUBLIC_FORCE_TYPES_HPP
#define MARSHLAND_PUBLIC_FORCE_TYPES_HPP

#include "MarshlandTypes.hpp"
#include <cstdint>

namespace marshland::force {

/// 变力模式
enum class ForceMode {
    TIMED,   ///< 定时模式（自动流程，timing.force_time_s 到期完成）
    MANUAL   ///< 手动模式（SBUS/WEB，无时限，持续有效）
};

/// 变力动作状态
enum class ForceActionState {
    IDLE,       ///< 空闲（无活动目标）
    RUNNING,    ///< 运行中
    COMPLETED,  ///< 已完成
    REJECTED,   ///< 已拒绝
    FAULT       ///< 故障
};

/// 变力目标提交结果
enum class ForceSubmitResult {
    ACCEPTED,              ///< 已接受
    REJECTED_INVALID,      ///< 拒绝：无效参数
    REJECTED_RANGE,        ///< 拒绝：超出配置范围
    REJECTED_CONFIG,       ///< 拒绝：配置未就绪
    REJECTED_BUSY,         ///< 拒绝：TIMED 模式下已有动作在进行
    REJECTED_SAFETY,       ///< 拒绝：安全条件不满足（急停/故障/无授权）
    MOTOR_COMMIT_FAILED    ///< 电机提交失败
};

/// 变力目标
struct ForceTarget {
    std::int32_t target_rpm;              ///< 目标转速（rpm）
    marshland::ControlSource source;      ///< 控制来源
    std::uint32_t request_id;             ///< 请求 ID（用于追踪）
};

/// 变力电机反馈
struct ForceMotorFeedback {
    bool valid;                  ///< 反馈是否有效
    bool has_received;           ///< 是否已接收过反馈
    std::int32_t rpm;            ///< 当前转速（rpm）
    std::uint64_t timestamp_us;  ///< 时间戳（微秒）
};

/// 变力状态
struct ForceStatus {
    ForceActionState state;              ///< 动作状态
    ForceMode mode;                      ///< 当前模式
    std::int32_t target_rpm;             ///< 目标转速（rpm）
    marshland::ControlSource source;     ///< 控制来源
    ForceMotorFeedback feedback;         ///< 电机反馈
    std::uint64_t deadline_us;           ///< 截止时间（微秒，仅 TIMED 模式）
    std::uint32_t generation;            ///< 动作代数（用于拒绝迟到事件）
    const char* error_code;              ///< 错误码（nullptr 表示无错误）
};

} // namespace marshland::force

#endif // MARSHLAND_PUBLIC_FORCE_TYPES_HPP
