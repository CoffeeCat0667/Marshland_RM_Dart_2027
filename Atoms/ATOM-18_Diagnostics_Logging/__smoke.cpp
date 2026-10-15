// ATOM-18 冒烟测试（临时文件，验证后删除，不属于模块交付物）
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <chrono>
#include <utility>
#include <vector>

#include "atom18/diagnostics_service.hpp"

using namespace marshland;
using marshland::config::ConfigStatus;

class FakeClock final : public app::IMonotonicClock {
public:
    std::int64_t nowMicros() const noexcept override
    {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    }
};

class FakeConfig final : public config::IConfigView {
public:
    std::map<std::string, std::int64_t> ints;
    std::map<std::string, bool> bools;
    std::map<std::string, std::string> strings;
    std::vector<std::string> fields;
    std::vector<std::string> required_events;
    std::vector<std::pair<std::string, std::string>> fault_table;

    ConfigStatus getInt(const char* key, std::int64_t& out) const noexcept override
    {
        auto it = ints.find(key);
        if (it == ints.end()) return ConfigStatus::MISSING;
        out = it->second;
        return ConfigStatus::OK;
    }
    ConfigStatus getDouble(const char*, double&) const noexcept override
    {
        return ConfigStatus::MISSING;
    }
    ConfigStatus getBool(const char* key, bool& out) const noexcept override
    {
        auto it = bools.find(key);
        if (it == bools.end()) return ConfigStatus::MISSING;
        out = it->second;
        return ConfigStatus::OK;
    }
    ConfigStatus getString(const char* key, const char*& data,
                           std::uint64_t& size) const noexcept override
    {
        auto it = strings.find(key);
        if (it == strings.end()) return ConfigStatus::MISSING;
        data = it->second.data();
        size = it->second.size();
        return ConfigStatus::OK;
    }
    bool has(const char* key) const noexcept override
    {
        return ints.count(key) || bools.count(key) || strings.count(key);
    }
    ConfigStatus getArraySize(const char* key,
                              std::uint64_t& out) const noexcept override
    {
        const std::string k(key);
        if (k == "fault.table") { out = fault_table.size(); return ConfigStatus::OK; }
        if (k == "log.fields") { out = fields.size(); return ConfigStatus::OK; }
        if (k == "log.required_events") { out = required_events.size(); return ConfigStatus::OK; }
        return ConfigStatus::MISSING;
    }
    ConfigStatus getIntAt(const char*, std::uint64_t, const char*,
                          std::int64_t&) const noexcept override
    {
        return ConfigStatus::MISSING;
    }
    ConfigStatus getDoubleAt(const char*, std::uint64_t, const char*,
                             double&) const noexcept override
    {
        return ConfigStatus::MISSING;
    }
    ConfigStatus getBoolAt(const char*, std::uint64_t, const char*,
                           bool&) const noexcept override
    {
        return ConfigStatus::MISSING;
    }
    ConfigStatus getStringAt(const char* key, std::uint64_t index,
                             const char* element_key, const char*& data,
                             std::uint64_t& size) const noexcept override
    {
        const std::string k(key);
        const std::string elem = element_key != nullptr ? element_key : "";
        if (k == "fault.table" && index < fault_table.size()) {
            const std::string* value = nullptr;
            if (elem == "code") value = &fault_table[index].first;
            if (elem == "severity") value = &fault_table[index].second;
            if (value != nullptr) {
                data = value->data();
                size = value->size();
                return ConfigStatus::OK;
            }
            return ConfigStatus::MISSING;
        }
        if (element_key == nullptr) {
            const std::vector<std::string>* arr = nullptr;
            if (k == "log.fields") arr = &fields;
            if (k == "log.required_events") arr = &required_events;
            if (arr != nullptr && index < arr->size()) {
                data = (*arr)[index].data();
                size = (*arr)[index].size();
                return ConfigStatus::OK;
            }
        }
        return ConfigStatus::MISSING;
    }
    std::uint64_t configVersion() const noexcept override { return 1; }
    bool isLoaded() const noexcept override { return true; }
};

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            return 1;                                                      \
        }                                                                  \
    } while (0)

int main()
{
    FakeConfig cfg;
    cfg.strings["log.level"] = "INFO";
    cfg.strings["log.output_directory"] = "/tmp/atom18_logs";
    cfg.strings["log.stdout_echo"] = "NONE";
    cfg.strings["log.format"] = "JSON_LINES";
    cfg.strings["log.timestamp_format"] = "UTC_ISO8601_MS";
    cfg.bools["log.rotation_enabled"] = true;
    cfg.ints["log.max_files"] = 3;
    cfg.ints["log.max_file_size_bytes"] = 4096;  // 小阈值强制轮转
    cfg.ints["log.max_total_size_bytes"] = 1 << 20;
    cfg.ints["log.fault_history_limit"] = 1000;
    cfg.strings["log.fault_history_policy"] = "RING_LATEST_FIRST";
    cfg.strings["log.flush_policy"] = "ERROR_SAFETY_IMMEDIATE_OTHERS_BUFFER_1S";
    cfg.fields = {"timestamp", "level", "module", "event", "source",
                  "state", "request_id", "error_code", "payload"};
    cfg.fault_table = {{"F-CAN-001", "SEVERE"},
                       {"E-ESTOP-001", "ESTOP"},
                       {"W-CMD-001", "WARNING"}};

    FakeClock clock;
    atom18::DiagnosticsService service(cfg, clock);
    service.OnRuntimeInitialize();

    // 三类写入接口
    const diagnostics::PayloadField p1[] = {{"shot", std::int64_t{1}},
                                            {"note", std::string_view{"ok"}}};
    diagnostics::LogEvent ev;
    ev.level = diagnostics::LogLevel::INFO;
    ev.module = "state";
    ev.event = "mode_transition";
    ev.source = "WEB";
    ev.state = "MANUAL";
    ev.request_id = "req-1";
    ev.payload = p1;
    ev.payload_count = 2;
    service.WriteDiagnostic(ev);

    ev.module = "reload";
    ev.event = "reload_action";
    ev.level = diagnostics::LogLevel::INFO;
    service.WriteRun(ev);

    ev.module = "safety";
    ev.event = "software_estop";
    ev.level = diagnostics::LogLevel::ERROR;
    ev.error_code = "E-ESTOP-001";
    service.WriteSafety(ev);

    // 故障管理
    const std::uint64_t id = service.ReportFault("F-CAN-001", "can0");
    CHECK(id > 0);
    CHECK(service.ReportFault("F-NOPE-999", "x") == 0);  // 表外故障码拒绝
    CHECK(service.QueryActiveFaults().size() == 1);
    CHECK(!service.ClearFault(id));                      // 未恢复不可清除
    CHECK(service.MarkFaultRecovered(id));
    CHECK(service.ClearFault(id));
    CHECK(service.QueryActiveFaults().empty());
    diagnostics::FaultQuery query;
    query.code = std::string("F-CAN-001");
    auto history = service.QueryHistoryFaults(query);
    CHECK(history.size() == 1);
    CHECK(history[0].severity == FaultSeverity::SEVERE);
    CHECK(history[0].source == "can0");
    CHECK(!history[0].active && history[0].recovered);

    // 轮转：写入足够多事件越过 4 KiB 阈值
    const diagnostics::PayloadField p2[] = {
        {"text", std::string_view{"0123456789abcdef0123456789abcdef"}}};
    ev.module = "run";
    ev.event = "fire_action";
    ev.level = diagnostics::LogLevel::INFO;
    ev.error_code = "";
    ev.payload = p2;
    ev.payload_count = 1;
    for (int i = 0; i < 200; ++i) {
        service.WriteRun(ev);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const diagnostics::DiagnosticsSnapshot snapshot = service.GetSnapshot();
    std::printf("snapshot: config_loaded=%d writer_active=%d written=%llu "
                "dropped=%llu rotations=%llu history=%llu\n",
                snapshot.config_loaded ? 1 : 0, snapshot.writer_active ? 1 : 0,
                (unsigned long long)snapshot.written_event_count,
                (unsigned long long)snapshot.dropped_event_count,
                (unsigned long long)snapshot.rotation_count,
                (unsigned long long)snapshot.history_fault_count);
    CHECK(snapshot.config_loaded);
    CHECK(snapshot.writer_active);
    CHECK(snapshot.written_event_count >= 200);
    CHECK(snapshot.rotation_count >= 1);
    CHECK(snapshot.history_fault_count == 1);

    service.Shutdown();
    std::printf("SMOKE_OK\n");
    return 0;
}
