// ============================================================================
// diagnostics_service.hpp — ATOM-18 主类（模块内部）
//
// 实现 Public 三接口：ILogSink / IFaultRegistry / IDiagnosticsSnapshotProvider。
//
// 初始化时序（PLAN §3.3）：RuntimeStep::CreateDiagnostics 早于
// LoadConfiguration，构造时只存引用；每次受理事件前检查
// IConfigView::configVersion()，首次 LoadLogConfig 成功后启动写线程并
// 回放配置就绪前的内存缓冲。配置未就绪期间事件入环形缓冲（丢最旧+计数），
// 故障报告入待登记缓冲，配置就绪后补登（此时才分配故障 id）。
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "DiagnosticsTypes.hpp"
#include "IConfigView.hpp"
#include "IDiagnosticsSnapshot.hpp"
#include "IFaultRegistry.hpp"
#include "ILogSink.hpp"
#include "IMonotonicClock.hpp"
#include "atom18/log_config.hpp"
#include "atom18/log_writer.hpp"

namespace marshland::atom18 {

class DiagnosticsService final : public diagnostics::ILogSink,
                                 public diagnostics::IFaultRegistry,
                                 public diagnostics::IDiagnosticsSnapshotProvider {
public:
    static constexpr std::size_t kPreConfigBufferCapacity = 256;
    static constexpr std::size_t kPendingFaultCapacity = 64;

    DiagnosticsService(const marshland::config::IConfigView& config,
                       const marshland::app::IMonotonicClock& clock) noexcept;
    ~DiagnosticsService() override;

    DiagnosticsService(const DiagnosticsService&) = delete;
    DiagnosticsService& operator=(const DiagnosticsService&) = delete;

    // 装配层入口（对应 RuntimeStep::CreateDiagnostics / 关闭流程）
    void OnRuntimeInitialize() noexcept;
    void Shutdown() noexcept;

    // ILogSink
    void WriteDiagnostic(const diagnostics::LogEvent& event) noexcept override;
    void WriteRun(const diagnostics::LogEvent& event) noexcept override;
    void WriteSafety(const diagnostics::LogEvent& event) noexcept override;

    // IFaultRegistry
    std::uint64_t ReportFault(std::string_view code,
                              std::string_view source) noexcept override;
    std::vector<diagnostics::FaultRecord> QueryActiveFaults() const noexcept override;
    std::vector<diagnostics::FaultRecord> QueryHistoryFaults(
        const diagnostics::FaultQuery& query) const noexcept override;
    bool MarkFaultRecovered(std::uint64_t id) noexcept override;
    bool ClearFault(std::uint64_t id) noexcept override;

    // IDiagnosticsSnapshotProvider
    diagnostics::DiagnosticsSnapshot GetSnapshot() const noexcept override;

private:
    struct PendingFault {
        std::string code;
        std::string source;
        std::int64_t occurred_utc_ms{0};
        std::int64_t occurred_monotonic_us{0};
    };

    void HandleEvent(const diagnostics::LogEvent& event,
                     diagnostics::LogStream stream) noexcept;
    // 以下 *Locked 方法要求调用方已持有 mutex_
    void HandleEventLocked(const StoredEvent& stored) noexcept;
    // 返回 false 表示配置未就绪或写线程不可用（缓冲模式）
    bool EnsureConfigLoadedLocked() noexcept;
    void OnConfigReadyLocked() noexcept;
    void ReplayPreConfigBufferLocked() noexcept;
    // 返回分配的故障 id；故障码不在 fault.table[] 时拒绝并返回 0
    std::uint64_t RegisterFaultLocked(const PendingFault& pending) noexcept;
    // 在 Config fault.table[] 中查找故障码；找到时输出等级
    bool LookupFaultSeverity(std::string_view code,
                             FaultSeverity& severity) const noexcept;

    const marshland::config::IConfigView& config_;
    const marshland::app::IMonotonicClock& clock_;

    mutable std::mutex mutex_;
    LogConfig log_config_{};
    bool config_loaded_{false};
    std::uint64_t loaded_config_version_{0};
    bool writer_start_attempted_{false};

    LogWriter writer_;
    std::deque<StoredEvent> pre_config_buffer_;
    std::uint64_t pre_config_dropped_{0};
    std::deque<PendingFault> pending_faults_;

    std::unordered_map<std::uint64_t, diagnostics::FaultRecord> active_faults_;
    std::deque<diagnostics::FaultRecord> fault_history_;  // 最新在尾部
    std::uint64_t next_fault_id_{1};
};

} // namespace marshland::atom18
