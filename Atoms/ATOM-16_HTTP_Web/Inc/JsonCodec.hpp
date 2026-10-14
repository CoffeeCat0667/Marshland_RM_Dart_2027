// ============================================================================
// JsonCodec.hpp — JSON 请求解析与响应构造（Config.md §11.1/§11.3）
//
// 设计约束：
//   * 解析内部使用 nlohmann/json 的非抛异常 API（parse(..., false)），
//     解析失败经 valid()==false 表达，由调用方映射为 INVALID_JSON——
//     异常不进入控制流。
//   * 本头文件不暴露任何第三方类型；第三方头只在 Src/ 内包含（PRIVATE）。
//   * JsonWriter 为手写输出器（无第三方依赖），负责字符串转义；
//     仅支持本模块响应所需的扁平/浅嵌套对象。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace marshland::http_web {

// 解析后的 JSON 请求体（只读视图）。拷贝禁止，移动可用。
class JsonBody {
public:
    JsonBody() noexcept;
    ~JsonBody();
    JsonBody(JsonBody&&) noexcept;
    JsonBody& operator=(JsonBody&&) noexcept;
    JsonBody(const JsonBody&) = delete;
    JsonBody& operator=(const JsonBody&) = delete;

    // 非抛异常解析；任何语法错误都得到 valid()==false 的对象。
    static JsonBody Parse(const char* data, std::size_t size) noexcept;
    static JsonBody Parse(std::string_view text) noexcept
    {
        return Parse(text.data(), text.size());
    }

    bool valid() const noexcept;
    bool isObject() const noexcept;
    bool has(const char* key) const noexcept;

    // 键不存在或类型不符时返回 nullopt（二者不可区分，调用方按需用 has() 区分）。
    std::optional<std::string>  getString(const char* key) const;
    std::optional<bool>         getBool(const char* key) const;
    std::optional<std::int64_t> getInt(const char* key) const;
    std::optional<double>       getDouble(const char* key) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 手写 JSON 对象输出器。add* 自动处理逗号与字符串转义；
// beginObject/endObject 支持一层以上嵌套（error.details 等）。
class JsonWriter {
public:
    // 根层「首字段」标记初始为 1：根对象第一个字段前不加逗号。
    JsonWriter() { out_.reserve(256); out_ = '{'; depth_first_[0] = 1; }

    JsonWriter& add(std::string_view key, std::string_view value);
    JsonWriter& add(std::string_view key, const char* value);
    JsonWriter& add(std::string_view key, bool value);
    JsonWriter& add(std::string_view key, std::int32_t value);
    JsonWriter& add(std::string_view key, std::uint32_t value);
    JsonWriter& add(std::string_view key, std::int64_t value);
    JsonWriter& add(std::string_view key, std::uint64_t value);
    JsonWriter& add(std::string_view key, double value);
    JsonWriter& addNull(std::string_view key);
    JsonWriter& addEmptyArray(std::string_view key);
    // 追加一段调用方保证合法的原始 JSON（数组/对象字面量）。
    JsonWriter& addRaw(std::string_view key, std::string_view raw_json);

    JsonWriter& beginObject(std::string_view key);
    JsonWriter& endObject();

    // 结束根对象并取出完整文本。调用一次后本对象不可再用。
    std::string finish();

private:
    void writeKey(std::string_view key);
    static void writeEscaped(std::string& out, std::string_view text);

    std::string out_;
    std::uint32_t depth_first_[8]{};  // 每层一个「是否首字段」标记，嵌套深度上限 8
    std::uint32_t depth_{0};
    bool finished_{false};
};

} // namespace marshland::http_web
