// ============================================================================
// JsonCodec.cpp — JsonBody / JsonWriter 实现
//
// 第三方头 nlohmann/json.hpp 只在本翻译单元包含（CMake PRIVATE include），
// 不泄漏到 Inc/ 对外接口。解析一律使用非抛异常 API。
// ============================================================================
#include "JsonCodec.hpp"

#include "nlohmann/json.hpp"

#include <charconv>

namespace marshland::http_web {

// ---------------------------------------------------------------------------
// JsonBody
// ---------------------------------------------------------------------------

struct JsonBody::Impl {
    nlohmann::json value;  // 解析失败时为 discarded
};

JsonBody::JsonBody() noexcept : impl_(std::make_unique<Impl>()) {}

JsonBody::~JsonBody() = default;
JsonBody::JsonBody(JsonBody&&) noexcept = default;
JsonBody& JsonBody::operator=(JsonBody&&) noexcept = default;

JsonBody JsonBody::Parse(const char* data, std::size_t size) noexcept
{
    JsonBody body;
    if (data == nullptr) {
        return body;
    }
    try {
        // allow_exceptions=false：语法错误得到 discarded 值而非异常。
        body.impl_->value = nlohmann::json::parse(data, data + size, nullptr, false);
    } catch (...) {
        // 分配失败等极端情况：一律按解析失败处理。
        body.impl_->value = nlohmann::json(nlohmann::json::value_t::discarded);
    }
    return body;
}

bool JsonBody::valid() const noexcept
{
    return !impl_->value.is_discarded();
}

bool JsonBody::isObject() const noexcept
{
    return impl_->value.is_object();
}

bool JsonBody::has(const char* key) const noexcept
{
    return key != nullptr && impl_->value.is_object() && impl_->value.contains(key);
}

std::optional<std::string> JsonBody::getString(const char* key) const
{
    if (!has(key)) {
        return std::nullopt;
    }
    const auto& v = impl_->value.at(key);
    if (!v.is_string()) {
        return std::nullopt;
    }
    try {
        return v.get<std::string>();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> JsonBody::getBool(const char* key) const
{
    if (!has(key)) {
        return std::nullopt;
    }
    const auto& v = impl_->value.at(key);
    if (!v.is_boolean()) {
        return std::nullopt;
    }
    return v.get<bool>();
}

std::optional<std::int64_t> JsonBody::getInt(const char* key) const
{
    if (!has(key)) {
        return std::nullopt;
    }
    const auto& v = impl_->value.at(key);
    if (!v.is_number_integer() && !v.is_number_unsigned()) {
        return std::nullopt;
    }
    try {
        return v.get<std::int64_t>();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<double> JsonBody::getDouble(const char* key) const
{
    if (!has(key)) {
        return std::nullopt;
    }
    const auto& v = impl_->value.at(key);
    if (!v.is_number()) {
        return std::nullopt;
    }
    return v.get<double>();
}

// ---------------------------------------------------------------------------
// JsonWriter
// ---------------------------------------------------------------------------

void JsonWriter::writeKey(std::string_view key)
{
    if (depth_first_[depth_] == 0) {
        out_ += ',';
    }
    depth_first_[depth_] = 0;
    writeEscaped(out_, key);
    out_ += ':';
}

void JsonWriter::writeEscaped(std::string& out, std::string_view text)
{
    out += '"';
    for (const char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (uc < 0x20) {
                // 其余控制字符用 \u00XX；UTF-8 多字节序列（>= 0x80）原样通过。
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(uc >> 4) & 0xF];
                out += hex[uc & 0xF];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

JsonWriter& JsonWriter::add(std::string_view key, std::string_view value)
{
    writeKey(key);
    writeEscaped(out_, value);
    return *this;
}

JsonWriter& JsonWriter::add(std::string_view key, const char* value)
{
    return add(key, value != nullptr ? std::string_view(value) : std::string_view());
}

JsonWriter& JsonWriter::add(std::string_view key, bool value)
{
    writeKey(key);
    out_ += (value ? "true" : "false");
    return *this;
}

JsonWriter& JsonWriter::add(std::string_view key, std::int32_t value)
{
    return add(key, static_cast<std::int64_t>(value));
}

JsonWriter& JsonWriter::add(std::string_view key, std::uint32_t value)
{
    return add(key, static_cast<std::uint64_t>(value));
}

JsonWriter& JsonWriter::add(std::string_view key, std::int64_t value)
{
    writeKey(key);
    char buf[24];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    out_.append(buf, ptr);
    return *this;
}

JsonWriter& JsonWriter::add(std::string_view key, std::uint64_t value)
{
    writeKey(key);
    char buf[24];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    out_.append(buf, ptr);
    return *this;
}

JsonWriter& JsonWriter::add(std::string_view key, double value)
{
    writeKey(key);
    char buf[40];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    out_.append(buf, ptr);
    return *this;
}

JsonWriter& JsonWriter::addNull(std::string_view key)
{
    writeKey(key);
    out_ += "null";
    return *this;
}

JsonWriter& JsonWriter::addEmptyArray(std::string_view key)
{
    writeKey(key);
    out_ += "[]";
    return *this;
}

JsonWriter& JsonWriter::addRaw(std::string_view key, std::string_view raw_json)
{
    writeKey(key);
    out_ += raw_json;
    return *this;
}

JsonWriter& JsonWriter::beginObject(std::string_view key)
{
    writeKey(key);
    out_ += '{';
    if (depth_ + 1 < 8) {
        depth_ += 1;
        depth_first_[depth_] = 1;
    }
    return *this;
}

JsonWriter& JsonWriter::endObject()
{
    out_ += '}';
    if (depth_ > 0) {
        depth_ -= 1;
    }
    return *this;
}

std::string JsonWriter::finish()
{
    if (!finished_) {
        while (depth_ > 0) {
            endObject();
        }
        out_ += '}';
        finished_ = true;
    }
    return std::move(out_);
}

} // namespace marshland::http_web
