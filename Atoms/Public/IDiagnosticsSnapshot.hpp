// ============================================================================
// IDiagnosticsSnapshot.hpp — 诊断快照契约（跨 ATOM 公共契约）
//
// 归属：Atoms/Public/，由 ATOM-18_Diagnostics_Logging 实现。
//
// 权威来源：
//   Design/Atomic/ATOM-18_Diagnostics_Logging.md 原子操作 15
//
// 语义约定：
//   * 快照是调用时刻的计数与状态拷贝，消费方（Web/装配层）据此观测模块
//     健康度；活动故障明细经 IFaultRegistry::QueryActiveFaults 获取；
//   * 线程安全、noexcept；不得触发文件 I/O。
// ============================================================================
#pragma once

#include "DiagnosticsTypes.hpp"

namespace marshland::diagnostics {

class IDiagnosticsSnapshotProvider {
public:
    virtual ~IDiagnosticsSnapshotProvider() = default;

    virtual DiagnosticsSnapshot GetSnapshot() const noexcept = 0;
};

} // namespace marshland::diagnostics
