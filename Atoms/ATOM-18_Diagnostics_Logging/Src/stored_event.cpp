#include "atom18/stored_event.hpp"

#include <type_traits>
#include <variant>

namespace marshland::atom18 {

StoredEvent StoreEvent(const diagnostics::LogEvent& event,
                       diagnostics::LogStream stream,
                       std::int64_t utc_ms,
                       std::int64_t monotonic_us) noexcept
{
    StoredEvent out{};
    out.utc_ms = utc_ms;
    out.monotonic_us = monotonic_us;
    out.level = event.level;
    out.stream = stream;

    bool truncated = false;
    truncated |= out.module.assign(event.module);
    truncated |= out.event_name.assign(event.event);
    truncated |= out.source.assign(event.source);
    truncated |= out.state.assign(event.state);
    truncated |= out.request_id.assign(event.request_id);
    truncated |= out.error_code.assign(event.error_code);

    if (event.payload != nullptr && event.payload_count > 0) {
        const std::size_t count =
            std::min(event.payload_count, diagnostics::kMaxPayloadFields);
        if (event.payload_count > count) {
            truncated = true;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const diagnostics::PayloadField& src = event.payload[i];
            StoredPayloadField& dst = out.payload[i];
            if (src.key.empty()) {
                continue;  // 空键字段跳过，不计入 payload_count
            }
            truncated |= dst.key.assign(src.key);
            std::visit(
                [&dst, &truncated](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, std::monostate>) {
                        dst.type = StoredPayloadField::Type::NONE;
                    } else if constexpr (std::is_same_v<T, std::int64_t>) {
                        dst.type = StoredPayloadField::Type::INT;
                        dst.int_value = value;
                    } else if constexpr (std::is_same_v<T, double>) {
                        dst.type = StoredPayloadField::Type::DOUBLE;
                        dst.double_value = value;
                    } else if constexpr (std::is_same_v<T, bool>) {
                        dst.type = StoredPayloadField::Type::BOOL;
                        dst.bool_value = value;
                    } else if constexpr (std::is_same_v<T, std::string_view>) {
                        dst.type = StoredPayloadField::Type::TEXT;
                        truncated |= dst.text.assign(value);
                    }
                },
                src.value);
            ++out.payload_count;
        }
    }

    out.truncated = truncated;
    return out;
}

} // namespace marshland::atom18
