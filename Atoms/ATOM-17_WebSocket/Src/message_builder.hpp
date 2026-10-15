// ============================================================================
// message_builder.hpp — ATOM-17 内部消息构造（仅 Src 使用）
//
// 信封字段名与顺序来自配置 websocket.envelope_fields；各消息类型的 payload
// 顶层字段来自配置 websocket.message_types[].payload_fields（ConfigSchema
// §2.14 禁令：禁止在代码中硬编码消息类型表或 payload 字段）。
// 字段名 → 数据源的映射是领域逻辑，无法配置化；无数据源的字段输出 null
// （"未接入"语义，ConfigSchema §1.7），整个类型均无数据源时跳过推送。
// ============================================================================
#pragma once

#include <optional>
#include <string>

#include "nlohmann/json.hpp"

#include "atom17_websocket/ws_config.hpp"
#include "atom17_websocket/ws_snapshot.hpp"

namespace marshland::atom17::detail {

// UTC ISO 8601 毫秒时间戳（信封 timestamp；周期/超时计算用单调时钟，不在此）。
std::string Iso8601UtcNow();

// 按 envelope_fields 顺序组装统一信封。request_id 传入 nullptr 表示
// 服务端主动推送（Config.md §12.1：无关联请求的服务端事件 request_id 为 null）。
nlohmann::ordered_json BuildEnvelope(const WsConfig& config, const char* type,
                                     nlohmann::ordered_json payload,
                                     const nlohmann::json* request_id);

// 构造指定类型的 payload；返回 nullopt 表示该类型当前无任何数据源，
// 本周期跳过推送（不计错误）。
std::optional<nlohmann::ordered_json> BuildPayload(
    const MessageTypeDefinition& def, const StateSnapshot& snapshot,
    const WsConfig& config);

// 构造 error 消息（拒绝执行/协议错误）。payload 含 code/message/details，
// 并按 Config.md §12.3 在 payload 内保留 request_id；信封 request_id 同样保留。
nlohmann::ordered_json BuildError(const WsConfig& config, const char* code,
                                  const char* message,
                                  nlohmann::ordered_json details,
                                  const nlohmann::json* request_id);

// 构造 pong：与客户端 ping 关联相同 request_id（Config.md §12.1）。
nlohmann::ordered_json BuildPong(const WsConfig& config,
                                 const nlohmann::json* request_id);

} // namespace marshland::atom17::detail
