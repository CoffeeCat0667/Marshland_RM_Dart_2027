// ============================================================================
// AutoFire.hpp — ATOM-13 自动流程线程入口声明（模块内头文件）
//
// 对外暴露的数据结构 AutoData 在 Atoms/Public/Auto.hpp；本文件只声明线程入口
// 与周期常量，不含任何类型定义，避免与公共契约重复。
//
// 装配层以此拉起线程（仅限链接了 Marshland::atom_13_auto_fire 的调用方）：
//     std::thread t(marshland::auto_fire::RunAutoFire, std::ref(data),
//                   std::cref(stop_flag));
//
// 四发流程本身定义在 Design/Atomic 的 ATOM-13 文档内，本工程不得实现。
// ============================================================================
#pragma once

#include <atomic>
#include <chrono>

#include "Auto.hpp"

namespace marshland::auto_fire {

// 控制周期：20 ms（50 Hz）。自动流程线程唯一的节拍来源，调用方不得另设周期。
inline constexpr std::chrono::milliseconds kControlPeriod{20};

// 自动流程线程入口。阻塞运行直至 stop_requested 置位；返回 true = 收到停止
// 请求后正常退出。调用方拥有线程与两个引用参数的生命周期，本函数返回前不得
// 销毁 data。当前为占位实现，行为见 Src/AutoFire.cpp 的注释。
bool RunAutoFire(AutoData& data,
                 const std::atomic<bool>& stop_requested) noexcept;

} // namespace marshland::auto_fire
