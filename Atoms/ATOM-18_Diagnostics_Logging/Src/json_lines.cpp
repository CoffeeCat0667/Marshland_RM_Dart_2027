#include "atom18/json_lines.hpp"

#include <cstdio>
#include <ctime>

#include "DiagnosticsTypes.hpp"
#include "nlohmann/json.hpp"

namespace marshland::atom18 {

std::string FormatUtcIso8601Ms(std::int64_t utc_ms)
{
    const std::time_t secs = static_cast<std::time_t>(utc_ms / 1000);
    std::tm tm_utc{};
    ::gmtime_r(&secs, &tm_utc);

    // 钳制到合法日历范围，保证输出定长（"YYYY-MM-DDTHH:MM:SS.mmmZ"，24 字符）
    const auto clamp = [](int value, int low, int high) {
        return value < low ? low : (value > high ? high : value);
    };
    const int year = clamp(tm_utc.tm_year + 1900, 0, 9999);
    const int mon = clamp(tm_utc.tm_mon + 1, 1, 12);
    const int mday = clamp(tm_utc.tm_mday, 1, 31);
    const int hour = clamp(tm_utc.tm_hour, 0, 23);
    const int min = clamp(tm_utc.tm_min, 0, 59);
    const int sec = clamp(tm_utc.tm_sec, 0, 60);  // 闰秒
    const int ms = clamp(static_cast<int>(utc_ms % 1000), 0, 999);

    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  year, mon, mday, hour, min, sec, ms);
    return buffer;
}

namespace {

const std::vector<std::string>& DefaultFieldOrder()
{
    static const std::vector<std::string> order{
        "timestamp", "level", "module", "event", "source",
        "state", "request_id", "error_code", "payload"};
    return order;
}

nlohmann::ordered_json PayloadToJson(const StoredEvent& event)
{
    nlohmann::ordered_json payload = nlohmann::ordered_json::object();
    payload[std::string(diagnostics::kPayloadKeyStream)] =
        diagnostics::ToString(event.stream);
    for (std::size_t i = 0; i < event.payload_count; ++i) {
        const StoredPayloadField& field = event.payload[i];
        const std::string key(field.key.view());
        if (key.empty()) {
            continue;
        }
        switch (field.type) {
        case StoredPayloadField::Type::INT:
            payload[key] = field.int_value;
            break;
        case StoredPayloadField::Type::DOUBLE:
            payload[key] = field.double_value;
            break;
        case StoredPayloadField::Type::BOOL:
            payload[key] = field.bool_value;
            break;
        case StoredPayloadField::Type::TEXT:
            payload[key] = std::string(field.text.view());
            break;
        case StoredPayloadField::Type::NONE:
            payload[key] = nullptr;
            break;
        }
    }
    if (event.truncated) {
        payload[std::string(diagnostics::kPayloadKeyTruncated)] = true;
    }
    return payload;
}

} // namespace

std::string SerializeEventJsonLine(const StoredEvent& event,
                                   const std::vector<std::string>& field_order)
{
    const std::vector<std::string>& order =
        field_order.empty() ? DefaultFieldOrder() : field_order;

    nlohmann::ordered_json json = nlohmann::ordered_json::object();
    for (const std::string& field : order) {
        if (field == "timestamp") {
            json["timestamp"] = FormatUtcIso8601Ms(event.utc_ms);
        } else if (field == "level") {
            json["level"] = diagnostics::ToString(event.level);
        } else if (field == "module") {
            json["module"] = std::string(event.module.view());
        } else if (field == "event") {
            json["event"] = std::string(event.event_name.view());
        } else if (field == "source") {
            json["source"] = std::string(event.source.view());
        } else if (field == "state") {
            json["state"] = std::string(event.state.view());
        } else if (field == "request_id") {
            json["request_id"] = std::string(event.request_id.view());
        } else if (field == "error_code") {
            json["error_code"] = std::string(event.error_code.view());
        } else if (field == "payload") {
            json["payload"] = PayloadToJson(event);
        }
        // 未识别的字段名跳过（配置容错）
    }
    return json.dump();
}

} // namespace marshland::atom18
