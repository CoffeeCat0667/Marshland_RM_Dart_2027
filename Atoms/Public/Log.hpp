// ============================================================================
// Log.hpp — 日志系统唯一对外接口（跨 ATOM 公共契约）
//
// 归属：Atoms/Public/，由 ATOM-18_Diagnostics_Logging 实现。
//
// 全系统日志功能只有本文件的以下两个函数构成对外接口，别无其他入口：
//   1. Init(Level level)              —— 初始化：设定终端打印阈值
//   2. Log(Level level, string_view)  —— 写入一条日志：等级 + 内容
//
// 语义（设计细节见 Atoms/ATOM-18_Diagnostics_Logging/PLAN.md §1、§2）：
//   * 终端打印设计：仅当 level >= Init 传入的等级时才打印到 stdout；
//     Init 未被调用时阈值为 Level::INFO；Init 可在运行期重复调用以调整阈值；
//   * 磁盘写入设计：所有等级的日志都会追加写入日志文件（不受阈值影响），
//     单文件超限轮转，并按文件数/总量上限清理最旧文件；
//   * 两个函数均线程安全、noexcept，不向调用方报告错误（失败静默降级）。
//
// Level 是本接口的参数类型，不是第三个接口。
// ============================================================================
#pragma once

#include <cstdint>
#include <string_view>

namespace marshland::log {

enum class Level : std::uint8_t {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERROR
};

// 初始化日志系统：设定"打印到终端"的最低等级（level 及以上打印到终端），
// 并确保日志目录存在。可重复调用。
void Init(Level level) noexcept;

// 写入一条日志：level 为日志等级，content 为日志内容。
// 该条日志一定会尝试写入磁盘文件；是否打印到终端由 Init 设定的阈值决定。
void Log(Level level, std::string_view content) noexcept;

} // namespace marshland::log
