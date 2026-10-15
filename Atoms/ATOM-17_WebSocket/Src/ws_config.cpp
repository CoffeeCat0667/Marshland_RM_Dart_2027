#include "atom17_websocket/ws_config.hpp"

#include <algorithm>
#include <array>
#include <exception>

namespace marshland::atom17 {
namespace {

using config::ConfigStatus;

bool GetString(const config::IConfigView& cfg, const char* key, std::string& out,
               WsConfigLoadResult& result)
{
    const char* data = nullptr;
    std::uint64_t size = 0;
    const ConfigStatus status = cfg.getString(key, data, size);
    if (status != ConfigStatus::OK) {
        result.status = status;
        result.failed_key = key;
        return false;
    }
    out.assign(data, size);
    return true;
}

bool GetInt(const config::IConfigView& cfg, const char* key, std::int64_t& out,
            WsConfigLoadResult& result)
{
    const ConfigStatus status = cfg.getInt(key, out);
    if (status != ConfigStatus::OK) {
        result.status = status;
        result.failed_key = key;
        return false;
    }
    return true;
}

bool GetBool(const config::IConfigView& cfg, const char* key, bool& out,
             WsConfigLoadResult& result)
{
    const ConfigStatus status = cfg.getBool(key, out);
    if (status != ConfigStatus::OK) {
        result.status = status;
        result.failed_key = key;
        return false;
    }
    return true;
}

// 标量字符串数组：element_key 传 ""（ATOM-15 已确认的标量数组读取约定，
// 见 Atoms/ATOM-15_Config_Persistence/PLAN.md §11.1）。
bool GetStringArray(const config::IConfigView& cfg, const char* key,
                    std::vector<std::string>& out, WsConfigLoadResult& result)
{
    std::uint64_t count = 0;
    ConfigStatus status = cfg.getArraySize(key, count);
    if (status != ConfigStatus::OK) {
        result.status = status;
        result.failed_key = key;
        return false;
    }
    out.clear();
    out.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        const char* data = nullptr;
        std::uint64_t size = 0;
        status = cfg.getStringAt(key, i, "", data, size);
        if (status != ConfigStatus::OK) {
            result.status = status;
            result.failed_key = key;
            return false;
        }
        out.emplace_back(data, size);
    }
    return true;
}

bool Fail(WsConfigLoadResult& result, ConfigStatus status, const char* key)
{
    result.status = status;
    result.failed_key = key;
    return false;
}

bool IsKnownEnvelopeField(const std::string& field)
{
    static const std::array<const char*, 6> kFields{
        "version", "type", "timestamp", "request_id", "source", "payload"};
    return std::any_of(kFields.begin(), kFields.end(),
                       [&](const char* known) { return field == known; });
}

bool IsKnownMessageType(const std::string& type)
{
    // ConfigSchema.schema.json 中 websocket.message_types[].type 的枚举集合。
    static const std::array<const char*, 11> kTypes{
        "system_state", "mechanism_state", "motor_state", "sensor_state",
        "yaw_state", "fire_progress", "fault", "estop_state",
        "control_arbiter", "parameter_changed", "error"};
    return std::any_of(kTypes.begin(), kTypes.end(),
                       [&](const char* known) { return type == known; });
}

bool LoadMessageTypes(const config::IConfigView& cfg, WsConfig& out,
                      WsConfigLoadResult& result)
{
    constexpr const char* kKey = "websocket.message_types";
    std::uint64_t count = 0;
    ConfigStatus status = cfg.getArraySize(kKey, count);
    if (status != ConfigStatus::OK) {
        return Fail(result, status, kKey);
    }

    out.message_types.clear();
    out.message_types.reserve(count);
    for (std::uint64_t i = 0; i < count; ++i) {
        MessageTypeDefinition def;

        const char* data = nullptr;
        std::uint64_t size = 0;
        status = cfg.getStringAt(kKey, i, "type", data, size);
        if (status != ConfigStatus::OK) {
            return Fail(result, status, kKey);
        }
        def.type.assign(data, size);
        if (!IsKnownMessageType(def.type)) {
            return Fail(result, ConfigStatus::OUT_OF_RANGE, kKey);
        }

        // error_message_payload 为可选布尔；MISSING 按 false 处理。
        bool error_payload = false;
        status = cfg.getBoolAt(kKey, i, "error_message_payload", error_payload);
        if (status == ConfigStatus::OK) {
            def.error_message_payload = error_payload;
        } else if (status != ConfigStatus::MISSING) {
            return Fail(result, status, kKey);
        }

        // payload_fields 为对象数组元素内的嵌套数组，依赖 IConfigView 的
        // 嵌套数组查询（Public 契约的可选扩展）。MISSING 表示实现方尚不支持
        // 或配置未提供该类型的字段定义：按"配置未提供"处理，不报错、不回填。
        std::uint64_t field_count = 0;
        status = cfg.getNestedArraySize(kKey, i, "payload_fields", field_count);
        if (status == ConfigStatus::OK) {
            def.payload_fields.reserve(field_count);
            for (std::uint64_t j = 0; j < field_count; ++j) {
                const char* field_data = nullptr;
                std::uint64_t field_size = 0;
                status = cfg.getNestedStringAt(kKey, i, "payload_fields", j,
                                               field_data, field_size);
                if (status != ConfigStatus::OK) {
                    return Fail(result, status, kKey);
                }
                def.payload_fields.emplace_back(field_data, field_size);
            }
        } else if (status != ConfigStatus::MISSING) {
            return Fail(result, status, kKey);
        }

        out.message_types.push_back(std::move(def));
    }
    return true;
}

bool Validate(const WsConfig& cfg, WsConfigLoadResult& result)
{
    if (cfg.port < 1 || cfg.port > 65535) {
        return Fail(result, ConfigStatus::OUT_OF_RANGE, "websocket.port");
    }
    if (cfg.message_version.empty()) {
        return Fail(result, ConfigStatus::INVALID_TYPE, "websocket.message_version");
    }
    if (cfg.heartbeat_period_ms < 1) {
        return Fail(result, ConfigStatus::OUT_OF_RANGE, "websocket.heartbeat_period_ms");
    }
    if (cfg.connection_timeout_ms < 1) {
        return Fail(result, ConfigStatus::OUT_OF_RANGE, "websocket.connection_timeout_ms");
    }
    if (cfg.push_period_ms < 1) {
        return Fail(result, ConfigStatus::OUT_OF_RANGE, "websocket.push_period_ms");
    }
    if (cfg.io_timeout_ms < 1) {
        return Fail(result, ConfigStatus::OUT_OF_RANGE, "timing.websocket_io_timeout_ms");
    }
    // 只读通道是本模块的硬边界（Config.md §12.1 / schema const false）。
    if (cfg.control_over_ws_allowed) {
        return Fail(result, ConfigStatus::OUT_OF_RANGE, "websocket.control_over_ws_allowed");
    }
    if (cfg.multi_client_mode != "READ_ONLY_SUBSCRIBE") {
        return Fail(result, ConfigStatus::OUT_OF_RANGE, "websocket.multi_client_mode");
    }
    if (cfg.listen_address.empty()) {
        return Fail(result, ConfigStatus::INVALID_TYPE, "websocket.listen_address");
    }

    // 信封字段：必须恰好覆盖 Config.md §12.2 定义的六个字段，无重复、无未知。
    if (cfg.envelope_fields.size() != 6) {
        return Fail(result, ConfigStatus::OUT_OF_RANGE, "websocket.envelope_fields");
    }
    std::array<bool, 6> seen{};
    for (const auto& field : cfg.envelope_fields) {
        if (!IsKnownEnvelopeField(field)) {
            return Fail(result, ConfigStatus::OUT_OF_RANGE, "websocket.envelope_fields");
        }
        static const std::array<const char*, 6> kFields{
            "version", "type", "timestamp", "request_id", "source", "payload"};
        for (std::size_t k = 0; k < kFields.size(); ++k) {
            if (field == kFields[k]) {
                if (seen[k]) {
                    return Fail(result, ConfigStatus::OUT_OF_RANGE,
                                "websocket.envelope_fields");
                }
                seen[k] = true;
            }
        }
    }

    if (cfg.auto_total_shots < 1) {
        return Fail(result, ConfigStatus::OUT_OF_RANGE, "baseline.auto_total_shots");
    }
    if (cfg.fire_priority_order.empty()) {
        return Fail(result, ConfigStatus::MISSING, "baseline.fire_priority_order");
    }
    return true;
}

} // namespace

WsConfigLoadResult LoadWsConfig(const config::IConfigView& config) noexcept
{
    WsConfigLoadResult result;
    try {
        WsConfig& cfg = result.config;

        if (!GetString(config, "websocket.listen_address", cfg.listen_address, result) ||
            !GetInt(config, "websocket.port", cfg.port, result) ||
            !GetString(config, "websocket.message_version", cfg.message_version, result) ||
            !GetInt(config, "websocket.heartbeat_period_ms", cfg.heartbeat_period_ms, result) ||
            !GetInt(config, "websocket.connection_timeout_ms", cfg.connection_timeout_ms, result) ||
            !GetInt(config, "websocket.push_period_ms", cfg.push_period_ms, result) ||
            !GetInt(config, "timing.websocket_io_timeout_ms", cfg.io_timeout_ms, result) ||
            !GetBool(config, "websocket.control_over_ws_allowed", cfg.control_over_ws_allowed, result) ||
            !GetString(config, "websocket.multi_client_mode", cfg.multi_client_mode, result) ||
            !GetStringArray(config, "websocket.envelope_fields", cfg.envelope_fields, result) ||
            !GetInt(config, "baseline.auto_total_shots", cfg.auto_total_shots, result) ||
            !GetStringArray(config, "baseline.fire_priority_order", cfg.fire_priority_order, result) ||
            !LoadMessageTypes(config, cfg, result) ||
            !Validate(cfg, result)) {
            return result;
        }

        result.status = ConfigStatus::OK;
        result.failed_key.clear();
        return result;
    } catch (const std::exception&) {
        result.status = ConfigStatus::NOT_LOADED;
        result.failed_key = "<exception>";
        return result;
    } catch (...) {
        result.status = ConfigStatus::NOT_LOADED;
        result.failed_key = "<unknown-exception>";
        return result;
    }
}

} // namespace marshland::atom17
