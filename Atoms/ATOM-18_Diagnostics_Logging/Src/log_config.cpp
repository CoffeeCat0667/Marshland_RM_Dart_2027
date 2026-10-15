#include "atom18/log_config.hpp"

namespace marshland::atom18 {

namespace {

using marshland::config::ConfigStatus;
using marshland::config::IConfigView;

// 单个键的读取结果统一收口：MISSING → 内置默认 + 计数；
// INVALID_TYPE/OUT_OF_RANGE → 失败并记录键名；NOT_LOADED → 整体未就绪
struct Loader {
    const IConfigView& view;
    bool ready{true};
    bool ok{true};
    const char* error{""};
    std::uint32_t defaults{0};

    bool GetString(const char* key, std::string& out)
    {
        const char* data = nullptr;
        std::uint64_t size = 0;
        switch (view.getString(key, data, size)) {
        case ConfigStatus::OK:
            out.assign(data, size);
            return true;
        case ConfigStatus::MISSING:
            ++defaults;
            return true;
        case ConfigStatus::NOT_LOADED:
            ready = false;
            return false;
        default:
            ok = false;
            error = key;
            return false;
        }
    }

    bool GetInt(const char* key, std::int64_t& out)
    {
        switch (view.getInt(key, out)) {
        case ConfigStatus::OK:
            if (out <= 0) {  // schema 约束 minimum 1
                ok = false;
                error = key;
                return false;
            }
            return true;
        case ConfigStatus::MISSING:
            ++defaults;
            return true;
        case ConfigStatus::NOT_LOADED:
            ready = false;
            return false;
        default:
            ok = false;
            error = key;
            return false;
        }
    }

    bool GetBool(const char* key, bool& out)
    {
        switch (view.getBool(key, out)) {
        case ConfigStatus::OK:
            return true;
        case ConfigStatus::MISSING:
            ++defaults;
            return true;
        case ConfigStatus::NOT_LOADED:
            ready = false;
            return false;
        default:
            ok = false;
            error = key;
            return false;
        }
    }

    bool GetStringArray(const char* key, std::vector<std::string>& out)
    {
        std::uint64_t count = 0;
        switch (view.getArraySize(key, count)) {
        case ConfigStatus::OK:
            break;
        case ConfigStatus::MISSING:
            ++defaults;
            return true;
        case ConfigStatus::NOT_LOADED:
            ready = false;
            return false;
        default:
            ok = false;
            error = key;
            return false;
        }
        out.clear();
        out.reserve(count);
        for (std::uint64_t i = 0; i < count; ++i) {
            const char* data = nullptr;
            std::uint64_t size = 0;
            const ConfigStatus status =
                view.getStringAt(key, i, nullptr, data, size);
            if (status != ConfigStatus::OK) {
                ok = false;
                error = key;
                return false;
            }
            out.emplace_back(data, size);
        }
        return true;
    }
};

bool ParseStdoutEcho(const std::string& text, StdoutEcho& out)
{
    if (text == "NONE") { out = StdoutEcho::NONE; return true; }
    if (text == "CRITICAL_ONLY") { out = StdoutEcho::CRITICAL_ONLY; return true; }
    if (text == "ALL") { out = StdoutEcho::ALL; return true; }
    return false;
}

} // namespace

ConfigLoadResult LoadLogConfig(const IConfigView& view, LogConfig& out) noexcept
{
    try {
        if (!view.isLoaded()) {
            return ConfigLoadResult{false, false, "config not loaded", 0};
        }

        LogConfig fresh{};
        Loader loader{view};

        std::string text;

        if (!loader.GetString("log.level", text)) goto done;
        if (!text.empty()) {
            const auto level = diagnostics::LogLevelFromString(text);
            if (!level.has_value()) {
                loader.ok = false;
                loader.error = "log.level";
                goto done;
            }
            fresh.level = *level;
        }

        text.clear();
        if (!loader.GetString("log.output_directory", fresh.output_directory)) goto done;

        text.clear();
        if (!loader.GetString("log.stdout_echo", text)) goto done;
        if (!text.empty() && !ParseStdoutEcho(text, fresh.stdout_echo)) {
            loader.ok = false;
            loader.error = "log.stdout_echo";
            goto done;
        }

        text.clear();
        if (!loader.GetString("log.format", text)) goto done;
        if (!text.empty() && text != "JSON_LINES") {
            loader.ok = false;
            loader.error = "log.format";
            goto done;
        }

        text.clear();
        if (!loader.GetString("log.timestamp_format", text)) goto done;
        if (!text.empty() && text != "UTC_ISO8601_MS") {
            loader.ok = false;
            loader.error = "log.timestamp_format";
            goto done;
        }

        if (!loader.GetBool("log.rotation_enabled", fresh.rotation_enabled)) goto done;
        if (!loader.GetInt("log.max_files", fresh.max_files)) goto done;
        if (!loader.GetInt("log.max_file_size_bytes", fresh.max_file_size_bytes)) goto done;
        if (!loader.GetInt("log.max_total_size_bytes", fresh.max_total_size_bytes)) goto done;
        if (!loader.GetInt("log.fault_history_limit", fresh.fault_history_limit)) goto done;

        text.clear();
        if (!loader.GetString("log.fault_history_policy", text)) goto done;
        if (!text.empty() && text != "RING_LATEST_FIRST") {
            loader.ok = false;
            loader.error = "log.fault_history_policy";
            goto done;
        }

        if (!loader.GetString("log.flush_policy", fresh.flush_policy)) goto done;
        if (!loader.GetStringArray("log.required_events", fresh.required_events)) goto done;
        if (!loader.GetStringArray("log.fields", fresh.fields)) goto done;

    done:
        if (!loader.ready) {
            return ConfigLoadResult{false, false, "config not loaded", 0};
        }
        if (!loader.ok) {
            return ConfigLoadResult{true, false, loader.error, loader.defaults};
        }
        out = std::move(fresh);
        return ConfigLoadResult{true, true, "", loader.defaults};
    } catch (...) {
        return ConfigLoadResult{false, false, "allocation failure", 0};
    }
}

} // namespace marshland::atom18
