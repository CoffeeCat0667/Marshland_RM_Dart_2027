// ============================================================================
// ILogSink.hpp — 日志写入契约（跨 ATOM 公共契约）
//
// 归属：Atoms/Public/，由 ATOM-18_Diagnostics_Logging 实现。
//
// 权威来源：
//   Design/Atomic/ATOM-18_Diagnostics_Logging.md 原子操作 1、8~12
//   Design/Config.md §16.1（刷新策略：ERROR 与安全事件立即刷新）
//
// 并发约定：
//   * 全部方法可在任意线程（含控制线程）调用，实现必须保证线程安全；
//   * 全部方法 noexcept、不得阻塞在文件 I/O 上（写入经异步队列）；
//   * WriteSafety 走独立保底通道，不与普通日志竞争队列容量，
//     受理后立即刷新落盘。
// ============================================================================
#pragma once

#include "DiagnosticsTypes.hpp"

namespace marshland::diagnostics {

class ILogSink {
public:
    virtual ~ILogSink() = default;

    // 诊断日志：状态迁移、控制权变更、迟到视觉状态等
    virtual void WriteDiagnostic(const LogEvent& event) noexcept = 0;

    // 运行日志：发射/动作结果、换弹/供弹/发射动作等
    virtual void WriteRun(const LogEvent& event) noexcept = 0;

    // 安全日志：急停、人工解除、安全事件；保底通道 + 立即刷新
    virtual void WriteSafety(const LogEvent& event) noexcept = 0;
};

} // namespace marshland::diagnostics
