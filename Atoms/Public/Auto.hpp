// ============================================================================
// Auto.hpp — ATOM-13 固定四发自动流程的跨 ATOM 公共契约
//
// 本文件只保留对外暴露的数据结构 AutoData，不声明任何函数、不定义任何常量：
//   线程入口 RunAutoFire() 与周期常量 kControlPeriod 声明于
//   Atoms/ATOM-13_Auto_Fire/Inc/AutoFire.hpp，实现见同模块 Src/AutoFire.cpp。
//
// 自动流程本身（蓄力、变力、换弹抬起/放下、供弹、YAW 到位等待、FIRE_READY
// 授权判断、发射调用）定义在 Design/Atomic 的 ATOM-13 文档内，本工程不得实现。
//
// 保持叶子契约：只依赖 C++ 标准库与同目录 MarshlandTypes.hpp。
// ============================================================================
#pragma once

#include <cstdint>
#include <mutex>

#include "MarshlandTypes.hpp"

namespace marshland::auto_fire {

// One writer (ATOM-13 自动流程线程)。All readers must hold mutex while
// copying the fields；本结构体不提供任何读写辅助函数。
//   state      : 业务自动流程状态（Interface §6.3）。IDLE 的底层值为 0。
//   shot_index : 当前发次，合法范围 1..4；0 表示尚未开始任何发次。
//   active     : 自动流程是否正在推进；false 表示未运行。
//   stop_fire  : 停止发射标志，设计基线默认 True；FIRE_READY 只读扫描，不得由
//                流程自身修改。真正的写入者为 WEB / 视觉系统。
//   online     : 自动流程线程是否存活；false 表示线程已退出。
struct AutoData final {
    mutable std::mutex mutex;
    AutoFireState state{AutoFireState::IDLE};
    std::uint8_t shot_index{0};
    bool active{false};
    bool stop_fire{true};
    bool online{false};
};

} // namespace marshland::auto_fire
