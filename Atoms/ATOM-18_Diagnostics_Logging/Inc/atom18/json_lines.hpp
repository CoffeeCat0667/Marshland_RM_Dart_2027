// ============================================================================
// json_lines.hpp — JSON Lines 序列化与 UTC 时间戳（模块内部）
//
// 格式权威：Design/Config.md §16.1（UTF-8 JSON Lines、UTC ISO8601 毫秒）、
// §16.3（字段与顺序）。序列化使用 nlohmann/json（PRIVATE 链接，
// 不泄漏到 Public 接口）。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "atom18/stored_event.hpp"

namespace marshland::atom18 {

// 毫秒时间戳 → "YYYY-MM-DDTHH:MM:SS.mmmZ"（UTC）
[[nodiscard]] std::string FormatUtcIso8601Ms(std::int64_t utc_ms);

// 序列化为一行 JSON（不含末尾换行）；field_order 为空时使用 Config §16.3 默认顺序。
// payload 渲染为嵌套对象，并注入保留键 "stream"；截断时附加 "_truncated": true。
[[nodiscard]] std::string SerializeEventJsonLine(
    const StoredEvent& event, const std::vector<std::string>& field_order);

} // namespace marshland::atom18
