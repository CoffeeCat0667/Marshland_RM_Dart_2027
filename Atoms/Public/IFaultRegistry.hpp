// ============================================================================
// IFaultRegistry.hpp — 故障登记与查询契约（跨 ATOM 公共契约）
//
// 归属：Atoms/Public/，由 ATOM-18_Diagnostics_Logging 实现。
//
// 权威来源：
//   Design/Atomic/ATOM-18_Diagnostics_Logging.md 原子操作 2~7
//   Design/Config.md §14.2 故障表（故障码封闭集）、§16.1（历史故障保留
//   1000 条，RING_LATEST_FIRST）
//
// 语义约定：
//   * ReportFault 的 code 必须存在于 Config fault.table[]，否则拒绝并返回 0；
//   * 故障等级从 fault.table[] 读取，调用方不得自定；
//   * ClearFault 仅在恢复状态已置位（MarkFaultRecovered）后生效；
//     若故障表要求人工确认（requires_confirm），ClearFault 调用本身即代表
//     该确认已由上游完成；清除只影响指定 id 这一条记录；
//   * 全部方法线程安全、noexcept；查询失败（如分配失败）返回空集。
// ============================================================================
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "DiagnosticsTypes.hpp"

namespace marshland::diagnostics {

class IFaultRegistry {
public:
    virtual ~IFaultRegistry() = default;

    // 登记一条故障，返回分配的故障 id（>0）。
    // 返回 0 表示拒绝：故障码不在 Config fault.table[] 中，
    // 或配置尚未就绪（此时故障报告进入待登记缓冲，配置就绪后补登）。
    virtual std::uint64_t ReportFault(std::string_view code,
                                      std::string_view source) noexcept = 0;

    // 当前活动故障集合（未被清除的）
    virtual std::vector<FaultRecord> QueryActiveFaults() const noexcept = 0;

    // 历史故障查询；空条件返回全部历史（最新优先）
    virtual std::vector<FaultRecord> QueryHistoryFaults(
        const FaultQuery& query) const noexcept = 0;

    // 更新恢复状态（根因已清除）；id 不存在返回 false
    virtual bool MarkFaultRecovered(std::uint64_t id) noexcept = 0;

    // 清除单条满足策略的故障（须已 MarkFaultRecovered）；
    // 成功后该记录从活动集移入历史集，不影响其他任何故障
    virtual bool ClearFault(std::uint64_t id) noexcept = 0;
};

} // namespace marshland::diagnostics
