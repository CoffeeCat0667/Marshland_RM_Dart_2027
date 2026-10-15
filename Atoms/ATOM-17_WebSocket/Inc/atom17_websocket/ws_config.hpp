// ============================================================================
// ws_config.hpp — ATOM-17 WebSocket 配置模型与加载
//
// 数据来源（只读，禁止修改）：
//   Design/Config.md §12        WebSocket 连接与消息配置（数值唯一来源）
//   Rule/ConfigSchema.md §2.14  websocket.* 键名契约
//   Rule/ConfigSchema.schema.json 形式化契约（元素结构以此为准）
//
// 约束：
//   - 全部取值经 marshland::config::IConfigView 查询；任何必需键失败即整体
//     失败并报告首个失败键，禁止回填默认值或猜测值（ConfigSchema §1.7）。
//   - message_types 的 payload 字段定义必须来自配置，禁止在代码中硬编码
//     消息类型表或 payload 字段（ConfigSchema §2.14 禁令）。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "IConfigView.hpp"

namespace marshland::atom17 {

// 单个服务端消息类型的定义（对应 websocket.message_types[] 元素）。
struct MessageTypeDefinition {
    std::string type;
    // payload 顶层字段名，顺序即配置顺序；为空表示配置未提供该类型的字段
    // 定义（例如 IConfigView 实现尚不支持嵌套数组查询），消费方必须显式处理。
    std::vector<std::string> payload_fields;
    bool error_message_payload{false};
};

struct WsConfig {
    std::string listen_address;        // websocket.listen_address
    std::int64_t port{0};              // websocket.port
    std::string message_version;       // websocket.message_version
    std::int64_t heartbeat_period_ms{0};
    std::int64_t connection_timeout_ms{0};
    std::int64_t push_period_ms{0};
    std::int64_t io_timeout_ms{0};     // timing.websocket_io_timeout_ms
    bool control_over_ws_allowed{true}; // 加载完成后必须为 false（只读通道）
    std::string multi_client_mode;
    std::vector<std::string> envelope_fields;
    std::vector<MessageTypeDefinition> message_types;

    // 状态 payload 所需的跨域配置：
    std::int64_t auto_total_shots{0};              // baseline.auto_total_shots
    std::vector<std::string> fire_priority_order;  // baseline.fire_priority_order
};

struct WsConfigLoadResult {
    config::ConfigStatus status{config::ConfigStatus::NOT_LOADED};
    std::string failed_key;  // 首个失败键；status == OK 时为空
    WsConfig config{};
};

// 读取并校验全部 WS 配置。语义级校验（端口范围、信封字段合法性、
// control_over_ws_allowed 必须为 false 等）失败时以 INVALID_TYPE/OUT_OF_RANGE
// 表达并给出 failed_key。
WsConfigLoadResult LoadWsConfig(const config::IConfigView& config) noexcept;

} // namespace marshland::atom17
