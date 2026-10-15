#include "atom18/diagnostics_service.hpp"

#include <chrono>
#include <cstdio>
#include <utility>

namespace marshland::atom18 {

namespace {

using marshland::config::ConfigStatus;

std::int64_t UtcNowMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

diagnostics::LogLevel LevelForSeverity(FaultSeverity severity) noexcept
{
    switch (severity) {
    case FaultSeverity::INFO:    return diagnostics::LogLevel::INFO;
    case FaultSeverity::WARNING: return diagnostics::LogLevel::WARN;
    case FaultSeverity::SEVERE:  return diagnostics::LogLevel::ERROR;
    case FaultSeverity::ESTOP:   return diagnostics::LogLevel::ERROR;
    }
    return diagnostics::LogLevel::INFO;
}

bool ParseFaultSeverity(std::string_view text, FaultSeverity& out) noexcept
{
    if (text == "INFO")    { out = FaultSeverity::INFO; return true; }
    if (text == "WARNING") { out = FaultSeverity::WARNING; return true; }
    if (text == "SEVERE")  { out = FaultSeverity::SEVERE; return true; }
    if (text == "ESTOP")   { out = FaultSeverity::ESTOP; return true; }
    return false;
}

} // namespace

DiagnosticsService::DiagnosticsService(
    const marshland::config::IConfigView& config,
    const marshland::app::IMonotonicClock& clock) noexcept
    : config_(config), clock_(clock), writer_(clock)
{
}

DiagnosticsService::~DiagnosticsService()
{
    Shutdown();
}

void DiagnosticsService::OnRuntimeInitialize() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureConfigLoadedLocked();
}

void DiagnosticsService::Shutdown() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 配置始终未就绪时缓冲事件随析构丢弃；计数保留在快照语义内
    writer_.Stop();
}

// ---------------------------------------------------------------------------
// ILogSink
// ---------------------------------------------------------------------------

void DiagnosticsService::WriteDiagnostic(
    const diagnostics::LogEvent& event) noexcept
{
    HandleEvent(event, diagnostics::LogStream::DIAGNOSTIC);
}

void DiagnosticsService::WriteRun(const diagnostics::LogEvent& event) noexcept
{
    HandleEvent(event, diagnostics::LogStream::RUN);
}

void DiagnosticsService::WriteSafety(const diagnostics::LogEvent& event) noexcept
{
    HandleEvent(event, diagnostics::LogStream::SAFETY);
}

void DiagnosticsService::HandleEvent(const diagnostics::LogEvent& event,
                                     diagnostics::LogStream stream) noexcept
{
    const StoredEvent stored =
        StoreEvent(event, stream, UtcNowMs(), clock_.nowMicros());
    std::lock_guard<std::mutex> lock(mutex_);
    HandleEventLocked(stored);
}

void DiagnosticsService::HandleEventLocked(const StoredEvent& stored) noexcept
{
    if (!EnsureConfigLoadedLocked()) {
        if (pre_config_buffer_.size() >= kPreConfigBufferCapacity) {
            pre_config_buffer_.pop_front();
            ++pre_config_dropped_;
        }
        try {
            pre_config_buffer_.push_back(stored);
        } catch (...) {
            ++pre_config_dropped_;
        }
        return;
    }
    // 等级过滤：低于 log.level 的事件直接丢弃（不计入 dropped）
    if (static_cast<std::uint8_t>(stored.level) <
        static_cast<std::uint8_t>(log_config_.level)) {
        return;
    }
    // ERROR 与安全事件走保底通道 + 立即刷新（原子操作 12）
    const bool priority = stored.stream == diagnostics::LogStream::SAFETY ||
                          stored.level == diagnostics::LogLevel::ERROR;
    writer_.Enqueue(stored, priority);
}

bool DiagnosticsService::EnsureConfigLoadedLocked() noexcept
{
    if (!config_.isLoaded()) {
        return false;
    }
    const std::uint64_t version = config_.configVersion();
    if (!config_loaded_ || version != loaded_config_version_) {
        LogConfig fresh{};
        const ConfigLoadResult result = LoadLogConfig(config_, fresh);
        if (!result.ready) {
            return false;
        }
        if (!result.success) {
            // 配置错误显式表达：拒绝新配置，保留旧配置（若有）；禁止静默兜底
            std::fprintf(stderr, "[atom18] log config rejected at key: %s\n",
                         result.error_message);
            return config_loaded_;
        }
        const bool first_load = !config_loaded_;
        log_config_ = std::move(fresh);
        loaded_config_version_ = version;
        config_loaded_ = true;
        if (first_load) {
            OnConfigReadyLocked();
        } else if (writer_.running()) {
            writer_.UpdateConfig(log_config_);  // 热加载
        }
    }
    if (config_loaded_ && !writer_.running()) {
        if (writer_.Start(log_config_)) {
            ReplayPreConfigBufferLocked();
        }
    }
    return writer_.running();
}

void DiagnosticsService::OnConfigReadyLocked() noexcept
{
    // 补登配置就绪前的故障报告（此时才能从 fault.table[] 取等级）
    for (const PendingFault& pending : pending_faults_) {
        RegisterFaultLocked(pending);
    }
    pending_faults_.clear();
    if (writer_.Start(log_config_)) {
        ReplayPreConfigBufferLocked();
    }
}

void DiagnosticsService::ReplayPreConfigBufferLocked() noexcept
{
    try {
        while (!pre_config_buffer_.empty()) {
            const StoredEvent& stored = pre_config_buffer_.front();
            if (static_cast<std::uint8_t>(stored.level) >=
                static_cast<std::uint8_t>(log_config_.level)) {
                const bool priority =
                    stored.stream == diagnostics::LogStream::SAFETY ||
                    stored.level == diagnostics::LogLevel::ERROR;
                writer_.Enqueue(stored, priority);
            }
            pre_config_buffer_.pop_front();
        }
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// IFaultRegistry
// ---------------------------------------------------------------------------

std::uint64_t DiagnosticsService::ReportFault(std::string_view code,
                                              std::string_view source) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!config_loaded_) {
            // 配置就绪前：进入待登记缓冲，补登时才分配 id；缓冲有界，满时丢最旧
            if (pending_faults_.size() >= kPendingFaultCapacity) {
                pending_faults_.pop_front();
            }
            pending_faults_.push_back(PendingFault{std::string(code),
                                                   std::string(source),
                                                   UtcNowMs(),
                                                   clock_.nowMicros()});
            return 0;
        }
        return RegisterFaultLocked(PendingFault{std::string(code),
                                                std::string(source), UtcNowMs(),
                                                clock_.nowMicros()});
    } catch (...) {
        return 0;
    }
}

std::uint64_t DiagnosticsService::RegisterFaultLocked(
    const PendingFault& pending) noexcept
{
    try {
        FaultSeverity severity;
        if (!LookupFaultSeverity(pending.code, severity)) {
            return 0;  // 故障码不在 Config §14.2 故障表内，拒绝登记
        }
        const std::uint64_t id = next_fault_id_++;
        diagnostics::FaultRecord record;
        record.id = id;
        record.code = pending.code;
        record.severity = severity;
        record.occurred_utc_ms = pending.occurred_utc_ms;
        record.occurred_monotonic_us = pending.occurred_monotonic_us;
        record.source = pending.source;
        record.active = true;
        record.recovered = false;
        active_faults_.emplace(id, record);

        // 故障事件同步生成结构化日志（验收：故障产生结构化日志）
        const diagnostics::PayloadField fields[] = {
            {"fault_id", static_cast<std::int64_t>(id)},
        };
        diagnostics::LogEvent event;
        event.level = LevelForSeverity(severity);
        event.module = "diagnostics";
        event.event = "fault";
        event.source = record.source;
        event.error_code = record.code;
        event.payload = fields;
        event.payload_count = 1;
        const diagnostics::LogStream stream =
            severity == FaultSeverity::ESTOP ? diagnostics::LogStream::SAFETY
                                             : diagnostics::LogStream::DIAGNOSTIC;
        HandleEventLocked(StoreEvent(event, stream, record.occurred_utc_ms,
                                     record.occurred_monotonic_us));
        return id;
    } catch (...) {
        return 0;
    }
}

std::vector<diagnostics::FaultRecord>
DiagnosticsService::QueryActiveFaults() const noexcept
{
    try {
        std::vector<diagnostics::FaultRecord> result;
        std::lock_guard<std::mutex> lock(mutex_);
        result.reserve(active_faults_.size());
        for (const auto& [id, record] : active_faults_) {
            (void)id;
            result.push_back(record);
        }
        return result;
    } catch (...) {
        return {};
    }
}

std::vector<diagnostics::FaultRecord>
DiagnosticsService::QueryHistoryFaults(
    const diagnostics::FaultQuery& query) const noexcept
{
    try {
        std::vector<diagnostics::FaultRecord> result;
        std::lock_guard<std::mutex> lock(mutex_);
        // 最新优先（RING_LATEST_FIRST）
        for (auto it = fault_history_.rbegin(); it != fault_history_.rend();
             ++it) {
            const diagnostics::FaultRecord& record = *it;
            if (query.code && *query.code != record.code) {
                continue;
            }
            if (query.source && *query.source != record.source) {
                continue;
            }
            if (query.occurred_since_utc_ms &&
                record.occurred_utc_ms < *query.occurred_since_utc_ms) {
                continue;
            }
            if (query.occurred_until_utc_ms &&
                record.occurred_utc_ms > *query.occurred_until_utc_ms) {
                continue;
            }
            result.push_back(record);
        }
        return result;
    } catch (...) {
        return {};
    }
}

bool DiagnosticsService::MarkFaultRecovered(std::uint64_t id) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_faults_.find(id);
        if (it == active_faults_.end()) {
            return false;
        }
        if (it->second.recovered) {
            return true;
        }
        it->second.recovered = true;

        const diagnostics::PayloadField fields[] = {
            {"fault_id", static_cast<std::int64_t>(id)},
        };
        diagnostics::LogEvent event;
        event.level = diagnostics::LogLevel::INFO;
        event.module = "diagnostics";
        event.event = "fault_recovered";
        event.source = it->second.source;
        event.error_code = it->second.code;
        event.payload = fields;
        event.payload_count = 1;
        HandleEventLocked(StoreEvent(event, diagnostics::LogStream::DIAGNOSTIC,
                                     UtcNowMs(), clock_.nowMicros()));
        return true;
    } catch (...) {
        return false;
    }
}

bool DiagnosticsService::ClearFault(std::uint64_t id) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_faults_.find(id);
        if (it == active_faults_.end()) {
            return false;
        }
        if (!it->second.recovered) {
            return false;  // 清除策略：须已更新恢复状态
        }
        diagnostics::FaultRecord record = it->second;
        record.active = false;
        active_faults_.erase(it);

        fault_history_.push_back(record);
        const std::size_t limit =
            log_config_.fault_history_limit > 0
                ? static_cast<std::size_t>(log_config_.fault_history_limit)
                : 1000;
        while (fault_history_.size() > limit) {
            fault_history_.pop_front();  // 淘汰最旧，最新优先保留
        }

        const diagnostics::PayloadField fields[] = {
            {"fault_id", static_cast<std::int64_t>(id)},
        };
        diagnostics::LogEvent event;
        event.level = diagnostics::LogLevel::INFO;
        event.module = "diagnostics";
        event.event = "fault_cleared";
        event.source = record.source;
        event.error_code = record.code;
        event.payload = fields;
        event.payload_count = 1;
        HandleEventLocked(StoreEvent(event, diagnostics::LogStream::DIAGNOSTIC,
                                     UtcNowMs(), clock_.nowMicros()));
        return true;
    } catch (...) {
        return false;
    }
}

bool DiagnosticsService::LookupFaultSeverity(std::string_view code,
                                             FaultSeverity& severity) const noexcept
{
    try {
        std::uint64_t count = 0;
        if (config_.getArraySize("fault.table", count) != ConfigStatus::OK) {
            return false;
        }
        for (std::uint64_t i = 0; i < count; ++i) {
            const char* data = nullptr;
            std::uint64_t size = 0;
            if (config_.getStringAt("fault.table", i, "code", data, size) !=
                ConfigStatus::OK) {
                continue;
            }
            if (std::string_view(data, size) != code) {
                continue;
            }
            const char* severity_data = nullptr;
            std::uint64_t severity_size = 0;
            if (config_.getStringAt("fault.table", i, "severity",
                                    severity_data,
                                    severity_size) != ConfigStatus::OK) {
                return false;
            }
            return ParseFaultSeverity(std::string_view(severity_data,
                                                       severity_size),
                                      severity);
        }
        return false;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// IDiagnosticsSnapshotProvider
// ---------------------------------------------------------------------------

diagnostics::DiagnosticsSnapshot DiagnosticsService::GetSnapshot() const noexcept
{
    diagnostics::DiagnosticsSnapshot snapshot{};
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot.config_loaded = config_loaded_;
        snapshot.config_version = loaded_config_version_;
        snapshot.writer_active = writer_.running();
        snapshot.active_fault_count = active_faults_.size();
        snapshot.history_fault_count = fault_history_.size();
        snapshot.dropped_event_count =
            writer_.dropped_count() + pre_config_dropped_;
        snapshot.buffered_event_count = pre_config_buffer_.size();
        snapshot.written_event_count = writer_.written_count();
        snapshot.rotation_count = writer_.rotation_count();
    } catch (...) {
    }
    snapshot.generated_utc_ms = UtcNowMs();
    snapshot.generated_monotonic_us = clock_.nowMicros();
    return snapshot;
}

} // namespace marshland::atom18
