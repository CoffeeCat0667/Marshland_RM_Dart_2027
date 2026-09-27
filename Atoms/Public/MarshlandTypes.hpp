// ============================================================================
// MarshlandTypes.hpp — 跨 ATOM 共享的基础类型
//
// 权威来源（逐字对齐，禁止改名/改顺序/增删成员）：
//   SystemMode    <- Design/Interface.md §6.2 系统模式
//   ControlSource <- Design/Interface.md §6.1 控制源
//   AutoFireState <- Design/Interface.md §6.3 自动流程状态
//   FaultSeverity <- Design/Config.md §14.2 故障等级四档（信息/警告/严重/急停）
//
// 本文件由 ATOM-02 首次落地于 Atoms/Public/；其他 ATOM 直接包含复用，
// 禁止各自重复定义同名枚举（同一编译单元会重定义报错）。
// ============================================================================
#pragma once

#include <cstdint>

namespace marshland {

// Interface §6.2 系统模式（11 项，顺序一致）
enum class SystemMode : std::uint8_t {
    UNINITIALIZED,
    INITIALIZING,
    INIT_FAILED,
    STANDBY,
    MANUAL,
    AUTO_FIRE,
    DEBUG,
    CALIBRATION,
    RECOVERY,
    SOFTWARE_ESTOP,
    FAULT
};

// Interface §6.1 控制源（4 项）
enum class ControlSource : std::uint8_t {
    NONE,
    SBUS,
    WEB,
    AUTO
};

// Interface §6.3 自动流程状态（15 项，顺序一致）
enum class AutoFireState : std::uint8_t {
    IDLE,
    AUTO_PREPARE,
    CHARGE,
    FORCE,
    SHOT_1_YAW,
    RELOAD_LIFT,
    FEED,
    FEED_WAIT,
    RELOAD_LOWER,
    SHOT_N_YAW,
    FIRE_READY,
    FIRING,
    FIRE_COMPLETED,
    AUTO_COMPLETED,
    AUTO_TERMINATED
};

// Config §14.2 故障等级：信息 / 警告 / 严重 / 急停
enum class FaultSeverity : std::uint8_t {
    INFO,
    WARNING,
    SEVERE,
    ESTOP
};

// 控制源优先级：SBUS > WEB > AUTO（Interface §3.2、§6.1）。
// NONE 不参与普通控制源排序，秩为 0。
inline int ControlSourceRank(ControlSource source) noexcept
{
    switch (source) {
    case ControlSource::SBUS:
        return 3;
    case ControlSource::WEB:
        return 2;
    case ControlSource::AUTO:
        return 1;
    case ControlSource::NONE:
        return 0;
    }
    return 0;
}

inline bool IsHigherPriority(ControlSource lhs, ControlSource rhs) noexcept
{
    return ControlSourceRank(lhs) > ControlSourceRank(rhs);
}

inline const char* ToString(SystemMode mode) noexcept
{
    switch (mode) {
    case SystemMode::UNINITIALIZED: return "UNINITIALIZED";
    case SystemMode::INITIALIZING:  return "INITIALIZING";
    case SystemMode::INIT_FAILED:   return "INIT_FAILED";
    case SystemMode::STANDBY:       return "STANDBY";
    case SystemMode::MANUAL:        return "MANUAL";
    case SystemMode::AUTO_FIRE:     return "AUTO_FIRE";
    case SystemMode::DEBUG:         return "DEBUG";
    case SystemMode::CALIBRATION:   return "CALIBRATION";
    case SystemMode::RECOVERY:      return "RECOVERY";
    case SystemMode::SOFTWARE_ESTOP:return "SOFTWARE_ESTOP";
    case SystemMode::FAULT:         return "FAULT";
    }
    return "UNKNOWN";
}

inline const char* ToString(ControlSource source) noexcept
{
    switch (source) {
    case ControlSource::NONE: return "NONE";
    case ControlSource::SBUS: return "SBUS";
    case ControlSource::WEB:  return "WEB";
    case ControlSource::AUTO: return "AUTO";
    }
    return "UNKNOWN";
}

inline const char* ToString(AutoFireState state) noexcept
{
    switch (state) {
    case AutoFireState::IDLE:           return "IDLE";
    case AutoFireState::AUTO_PREPARE:   return "AUTO_PREPARE";
    case AutoFireState::CHARGE:         return "CHARGE";
    case AutoFireState::FORCE:          return "FORCE";
    case AutoFireState::SHOT_1_YAW:     return "SHOT_1_YAW";
    case AutoFireState::RELOAD_LIFT:    return "RELOAD_LIFT";
    case AutoFireState::FEED:           return "FEED";
    case AutoFireState::FEED_WAIT:      return "FEED_WAIT";
    case AutoFireState::RELOAD_LOWER:   return "RELOAD_LOWER";
    case AutoFireState::SHOT_N_YAW:     return "SHOT_N_YAW";
    case AutoFireState::FIRE_READY:     return "FIRE_READY";
    case AutoFireState::FIRING:         return "FIRING";
    case AutoFireState::FIRE_COMPLETED: return "FIRE_COMPLETED";
    case AutoFireState::AUTO_COMPLETED: return "AUTO_COMPLETED";
    case AutoFireState::AUTO_TERMINATED:return "AUTO_TERMINATED";
    }
    return "UNKNOWN";
}

inline const char* ToString(FaultSeverity severity) noexcept
{
    switch (severity) {
    case FaultSeverity::INFO:    return "INFO";
    case FaultSeverity::WARNING: return "WARNING";
    case FaultSeverity::SEVERE:  return "SEVERE";
    case FaultSeverity::ESTOP:   return "ESTOP";
    }
    return "UNKNOWN";
}

} // namespace marshland
