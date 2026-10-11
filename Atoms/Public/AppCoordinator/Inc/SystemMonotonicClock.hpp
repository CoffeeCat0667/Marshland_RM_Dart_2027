// ============================================================================
// SystemMonotonicClock.hpp — IMonotonicClock 的唯一实现
//
// 依据：Design/Interface.md §3.3 —— 所有动作等待、超时和控制周期**必须**使用
// 单调时钟，不得使用可能被系统校时影响的墙上时钟。
//
// 位置说明：本文件放在 Atoms/Public/ 而非某个 ATOM 内，因为：
//   * IMonotonicClock 是 AppCoordinator（装配层）为状态机提供时间戳所需的契约；
//   * 时钟实现属于"平台服务"，不属于任何一个机构 ATOM 的单一职责。
//
// 实现选择：std::chrono::steady_clock，它被标准保证为单调（不会被 NTP /
// 手动改时影响），且不受闰秒影响。time_since_epoch() 的起点是实现定义的
// （Linux 上通常为系统启动时刻），因此只可用于**间隔与相对比较**，
// 不得当作日历时间使用。
// ============================================================================
#pragma once

#include <chrono>
#include <cstdint>

#include "IMonotonicClock.hpp"

namespace marshland::app {

class SystemMonotonicClock final : public IMonotonicClock {
public:
    std::int64_t nowMicros() const noexcept override
    {
        // 每实例不缓存起点，直接换算自 steady_clock 的纪元。
        // 标准库 epoch 起点通常远早于进程启动，因此 uint64 域内不会溢出。
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
};

} // namespace marshland::app
