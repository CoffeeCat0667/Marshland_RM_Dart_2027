// ============================================================================
// stored_event.hpp — 队列内自持有的日志事件存储（模块内部）
//
// Public 契约的 LogEvent 使用 string_view，跨异步队列传递必须深拷贝。
// 本头定义定长缓冲的 StoredEvent：入队时拷贝，杜绝悬垂引用；
// 超长按截断处理并置 truncated 标志（序列化时写入 payload._truncated）。
// ============================================================================
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "DiagnosticsTypes.hpp"

namespace marshland::atom18 {

template <std::size_t N>
struct FixedString {
    std::array<char, N> data{};
    std::uint16_t len{0};

    // 返回是否发生截断
    bool assign(std::string_view text) noexcept
    {
        const std::size_t n = std::min(text.size(), N);
        std::copy_n(text.data(), n, data.data());
        len = static_cast<std::uint16_t>(n);
        return text.size() > N;
    }

    [[nodiscard]] std::string_view view() const noexcept
    {
        return {data.data(), len};
    }

    [[nodiscard]] bool empty() const noexcept { return len == 0; }
};

struct StoredPayloadField {
    FixedString<24> key{};

    enum class Type : std::uint8_t { NONE, INT, DOUBLE, BOOL, TEXT };
    Type type{Type::NONE};

    std::int64_t int_value{0};
    double double_value{0.0};
    bool bool_value{false};
    FixedString<96> text{};
};

struct StoredEvent {
    std::int64_t utc_ms{0};              // system_clock 毫秒时间戳
    std::int64_t monotonic_us{0};        // 单调时钟微秒（时限测量用）
    diagnostics::LogLevel level{diagnostics::LogLevel::INFO};
    diagnostics::LogStream stream{diagnostics::LogStream::DIAGNOSTIC};

    FixedString<48> module{};
    FixedString<48> event_name{};
    FixedString<32> source{};
    FixedString<32> state{};
    FixedString<40> request_id{};
    FixedString<24> error_code{};

    std::array<StoredPayloadField, diagnostics::kMaxPayloadFields> payload{};
    std::size_t payload_count{0};
    bool truncated{false};
};

// 将 Public 契约事件深拷贝为自持有存储；utc_ms / monotonic_us 由调用方打好
[[nodiscard]] StoredEvent StoreEvent(const diagnostics::LogEvent& event,
                                     diagnostics::LogStream stream,
                                     std::int64_t utc_ms,
                                     std::int64_t monotonic_us) noexcept;

} // namespace marshland::atom18
