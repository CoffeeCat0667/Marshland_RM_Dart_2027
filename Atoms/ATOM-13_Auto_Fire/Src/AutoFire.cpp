// ============================================================================
// AutoFire.cpp — ATOM-13 自动流程线程实现
//
// 当前为占位实现，只做一件事：按 20 ms 控制周期把 AutoData 内所有数据写 0
// （停止发射标志按设计基线特例写 1）。不读取任何输入、不驱动任何机构、
// 不推进任何自动流程步骤。
//
// 四发流程（蓄力、变力、换弹抬起/放下、供弹、YAW 到位等待、FIRE_READY
// 授权判断、发射调用）定义在 Design/Atomic 的 ATOM-13 文档内，本工程不得实现。
// ============================================================================
#include "AutoFire.hpp"

#include <mutex>
#include <thread>

namespace marshland::auto_fire {

bool RunAutoFire(AutoData& data,
                 const std::atomic<bool>& stop_requested) noexcept
{
    using Clock = std::chrono::steady_clock;

    auto next_wakeup = Clock::now();
    while (!stop_requested.load(std::memory_order_relaxed)) {
        {
            const std::lock_guard<std::mutex> guard(data.mutex);
            data.state = AutoFireState::IDLE; // 底层值 0
            data.shot_index = 0;
            data.active = false;
            data.stop_fire = true;            // 特例：停止发射标志写 1
            data.online = false;
        }

        // 按绝对时刻推进，避免 sleep_for 的累计漂移；落后时不追帧，
        // 直接以当前时刻重新对齐，保证周期不小于 20 ms。
        next_wakeup += kControlPeriod;
        const auto now = Clock::now();
        if (next_wakeup > now) {
            std::this_thread::sleep_until(next_wakeup);
        } else {
            next_wakeup = now;
        }
    }
    return true;
}

} // namespace marshland::auto_fire
