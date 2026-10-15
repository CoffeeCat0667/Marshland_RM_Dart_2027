#include "message_builder.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace marshland::atom17::detail {
namespace {

using nlohmann::json;
using nlohmann::ordered_json;

const char* TransitionReasonToString(state_arbitration::TransitionReason reason) noexcept
{
    using state_arbitration::TransitionReason;
    switch (reason) {
    case TransitionReason::NONE:                   return "NONE";
    case TransitionReason::START:                  return "START";
    case TransitionReason::INIT_SUCCESS:           return "INIT_SUCCESS";
    case TransitionReason::INIT_FAILURE:           return "INIT_FAILURE";
    case TransitionReason::ENTER_MANUAL:           return "ENTER_MANUAL";
    case TransitionReason::AUTO_START:             return "AUTO_START";
    case TransitionReason::SBUS_TAKEOVER:          return "SBUS_TAKEOVER";
    case TransitionReason::WEB_MANUAL_TAKEOVER:    return "WEB_MANUAL_TAKEOVER";
    case TransitionReason::SOFTWARE_ESTOP_TRIGGER: return "SOFTWARE_ESTOP_TRIGGER";
    case TransitionReason::ESTOP_CONFIRM:          return "ESTOP_CONFIRM";
    case TransitionReason::ESTOP_RELEASED:         return "ESTOP_RELEASED";
    case TransitionReason::FAULT_DETECTED:         return "FAULT_DETECTED";
    case TransitionReason::RECOVERY_START:         return "RECOVERY_START";
    case TransitionReason::RECOVERY_COMPLETE:      return "RECOVERY_COMPLETE";
    case TransitionReason::DEBUG_ENTER:            return "DEBUG_ENTER";
    case TransitionReason::DEBUG_EXIT:             return "DEBUG_EXIT";
    case TransitionReason::CALIBRATION_ENTER:      return "CALIBRATION_ENTER";
    case TransitionReason::CALIBRATION_EXIT:       return "CALIBRATION_EXIT";
    case TransitionReason::AUTO_FLOW_RUN:          return "AUTO_FLOW_RUN";
    case TransitionReason::AUTO_FLOW_STEP:         return "AUTO_FLOW_STEP";
    case TransitionReason::AUTO_FLOW_DONE:         return "AUTO_FLOW_DONE";
    case TransitionReason::AUTO_FLOW_ABORT:        return "AUTO_FLOW_ABORT";
    case TransitionReason::CONTROL_SOURCE_REFRESH: return "CONTROL_SOURCE_REFRESH";
    }
    return "UNKNOWN";
}

// 判断某消息类型当前是否存在任一数据源（无数据源的类型整体跳过推送）。
bool HasAnyDataSource(const std::string& type)
{
    // mechanism_state / motor_state / sensor_state 的机构、电机与 GPIO 数据
    // 在 Public 契约中尚无来源；parameter_changed 由 ATOM-15 热加载事件驱动，
    // 当前同样无来源。这些类型跳过推送，不输出全 null 的占位消息。
    return type == "system_state" || type == "fire_progress" ||
           type == "control_arbiter" || type == "estop_state" ||
           type == "fault" || type == "yaw_state";
}

// 字段名 → 数据源映射。返回 false 表示该字段无数据源（输出 null）。
bool FieldValue(const std::string& type, const std::string& field,
                const StateSnapshot& snapshot, const WsConfig& config,
                json& out)
{
    const auto& context = snapshot.context;
    const auto& inputs = context.inputs;

    if (type == "system_state") {
        if (field == "system_mode")     { out = ToString(context.system_mode); return true; }
        if (field == "initialized")     { out = snapshot.hardware_ready; return true; }
        if (field == "control_source")  { out = ToString(context.control_source); return true; }
        if (field == "auto_fire_state") { out = ToString(context.auto_fire_state); return true; }
        if (field == "current_shot")    { out = snapshot.current_shot; return true; }
        if (field == "total_shots")     { out = config.auto_total_shots; return true; }
        if (field == "stop_fire")       { out = inputs.stop_fire; return true; }
        if (field == "estop_active")    { out = inputs.software_estop || snapshot.estop_latched; return true; }
        return false;
    }

    if (type == "fire_progress") {
        if (field == "current_shot") { out = snapshot.current_shot; return true; }
        if (field == "total_shots")  { out = config.auto_total_shots; return true; }
        if (field == "state")        { out = ToString(context.auto_fire_state); return true; }
        if (field == "stage")        { out = ToString(context.auto_fire_state); return true; }
        if (field == "stop_fire")    { out = inputs.stop_fire; return true; }
        return false;
    }

    if (type == "control_arbiter") {
        if (field == "active_source") { out = ToString(context.control_source); return true; }
        if (field == "priority_order") { out = config.fire_priority_order; return true; }
        if (field == "preempt_reason") {
            if (snapshot.has_preempt_reason) {
                out = TransitionReasonToString(snapshot.preempt_reason);
                return true;
            }
            return false;
        }
        return false;
    }

    if (type == "estop_state") {
        if (field == "active") { out = inputs.software_estop || snapshot.estop_latched; return true; }
        // source / confirm_required / recovery_state 在 Public 契约中尚无来源。
        return false;
    }

    if (type == "fault") {
        if (field == "active_faults") {
            out = json::array();
            if (inputs.fault_active) {
                // 仅有故障等级来源；故障码、时间、恢复状态均无来源，置 null。
                ordered_json entry;
                entry["code"] = nullptr;
                entry["severity"] = ToString(inputs.fault_severity);
                entry["message"] = nullptr;
                entry["occurred_at"] = nullptr;
                entry["recovery_state"] = nullptr;
                out.push_back(json(std::move(entry)));
            }
            return true;
        }
        return false;
    }

    if (type == "yaw_state") {
        if (field == "arrived") { out = inputs.yaw_arrived; return true; }
        // target_angle / cleared_by_main 在 Public 契约中尚无来源。
        return false;
    }

    return false;
}

} // namespace

std::string Iso8601UtcNow()
{
    using namespace std::chrono;
    const system_clock::time_point now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm utc{};
    ::gmtime_r(&seconds, &utc);

    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setfill('0') << std::setw(3) << static_cast<int>(millis) << 'Z';
    return out.str();
}

ordered_json BuildEnvelope(const WsConfig& config, const char* type,
                           ordered_json payload, const json* request_id)
{
    ordered_json envelope;
    for (const auto& field : config.envelope_fields) {
        if (field == "version") {
            envelope["version"] = config.message_version;
        } else if (field == "type") {
            envelope["type"] = type;
        } else if (field == "timestamp") {
            envelope["timestamp"] = Iso8601UtcNow();
        } else if (field == "request_id") {
            envelope["request_id"] = request_id != nullptr ? *request_id : json(nullptr);
        } else if (field == "source") {
            // Config.md §12.2：服务端推送统一标识为 WEB，不随事件原始来源变化。
            envelope["source"] = "WEB";
        } else if (field == "payload") {
            envelope["payload"] = std::move(payload);
        }
    }
    return envelope;
}

std::optional<ordered_json> BuildPayload(const MessageTypeDefinition& def,
                                         const StateSnapshot& snapshot,
                                         const WsConfig& config)
{
    if (!HasAnyDataSource(def.type)) {
        return std::nullopt;
    }

    ordered_json payload;
    for (const auto& field : def.payload_fields) {
        json value;
        if (FieldValue(def.type, field, snapshot, config, value)) {
            payload[field] = std::move(value);
        } else {
            payload[field] = nullptr;
        }
    }
    return payload;
}

ordered_json BuildError(const WsConfig& config, const char* code,
                        const char* message, ordered_json details,
                        const json* request_id)
{
    ordered_json payload;
    payload["code"] = code;
    payload["message"] = message;
    payload["details"] = std::move(details);
    payload["request_id"] = request_id != nullptr ? *request_id : json(nullptr);
    return BuildEnvelope(config, "error", std::move(payload), request_id);
}

ordered_json BuildPong(const WsConfig& config, const json* request_id)
{
    return BuildEnvelope(config, "pong", ordered_json::object(), request_id);
}

} // namespace marshland::atom17::detail
