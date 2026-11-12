#include "atom15_config/config_store.hpp"

#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string_view>
#include <utility>

#include "nlohmann/json.hpp"
#include "nlohmann/json-schema.hpp"
#include "config_schema_embed.hpp"

namespace marshland::atom15 {
namespace {

using nlohmann::json;

// ---------------------------------------------------------------------------
// 基础访问辅助
// ---------------------------------------------------------------------------

// nlohmann 会把超过 INT64_MAX 的非负整数解析为 number_unsigned；超出 int64 的
// 无符号数不视为整数。
bool integer(const json& v)
{
    if (v.is_number_unsigned()) {
        return v.get<std::uint64_t>() <=
               static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    }
    return v.is_number_integer();
}

std::int64_t intValue(const json& v)
{
    if (v.is_number_unsigned()) {
        return static_cast<std::int64_t>(v.get<std::uint64_t>());
    }
    return v.get<std::int64_t>();
}

bool number(const json& v)
{
    return v.is_number();
}

const json* member(const json* v, std::string_view key)
{
    if (!v || !v->is_object()) {
        return nullptr;
    }
    const auto it = v->find(key);
    return it == v->end() ? nullptr : &*it;
}

json* member(json* v, std::string_view key)
{
    if (!v || !v->is_object()) {
        return nullptr;
    }
    const auto it = v->find(key);
    return it == v->end() ? nullptr : &*it;
}

// 段语法：`name` 或 `name[index]`（每段最多一个下标）。
const json* findPath(const json& root, std::string_view key)
{
    const json* v = &root;
    std::size_t begin = 0;
    while (begin < key.size() && v) {
        const auto end = key.find('.', begin);
        const auto segment = key.substr(
            begin, end == std::string_view::npos ? key.size() - begin : end - begin);

        const auto bracket = segment.find('[');
        const auto name = segment.substr(0, bracket);
        if (name.empty() && bracket == std::string_view::npos) {
            return nullptr;
        }
        if (!name.empty()) {
            v = member(v, name);
            if (!v) {
                return nullptr;
            }
        }
        if (bracket != std::string_view::npos) {
            const auto close = segment.find(']', bracket);
            if (close == std::string_view::npos || !v->is_array()) {
                return nullptr;
            }
            const auto digits = segment.substr(bracket + 1, close - bracket - 1);
            if (digits.empty()) {
                return nullptr;
            }
            std::size_t index = 0;
            for (const char c : digits) {
                if (c < '0' || c > '9') {
                    return nullptr;
                }
                index = index * 10 + static_cast<std::size_t>(c - '0');
            }
            if (index >= v->size()) {
                return nullptr;
            }
            v = &(*v)[index];
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return v;
}

void diagnostic(std::vector<ConfigDiagnostic>& d, std::string key, std::string message)
{
    d.push_back({std::move(key), std::move(message), false});
}

// ---------------------------------------------------------------------------
// Schema 校验（编译期嵌入的 Rule/ConfigSchema.schema.json）
// ---------------------------------------------------------------------------

// validator 首次使用时编译一次并缓存；validate() 是 const 的，可在锁内复用。
nlohmann::json_schema::json_validator& schemaValidator()
{
    static const auto validator = [] {
        return std::make_unique<nlohmann::json_schema::json_validator>(
            json::parse(kConfigSchemaJson));
    }();
    return *validator;
}

class SchemaErrorCollector final : public nlohmann::json_schema::error_handler
{
public:
    void error(const json::json_pointer& ptr, const json&,
               const std::string& message) override
    {
        diagnostics.push_back(
            {ptr.empty() ? std::string() : ptr.to_string(), message, false});
    }

    std::vector<ConfigDiagnostic> diagnostics;
};

bool validate(const json& root, std::vector<ConfigDiagnostic>& d)
{
    try {
        SchemaErrorCollector collector;
        schemaValidator().validate(root, collector);
        for (auto& entry : collector.diagnostics) {
            d.push_back(std::move(entry));
        }
    } catch (const std::exception& e) {
        diagnostic(d, "", std::string("schema validation failed internally: ") + e.what());
    }
    return d.empty();
}

const char* errorName(ConfigError e) noexcept
{
    switch (e) {
    case ConfigError::OK:                 return "OK";
    case ConfigError::FILE_NOT_FOUND:     return "FILE_NOT_FOUND";
    case ConfigError::FILE_IO:            return "FILE_IO";
    case ConfigError::INVALID_JSON:       return "INVALID_JSON";
    case ConfigError::INVALID_SCHEMA:     return "INVALID_SCHEMA";
    case ConfigError::NOT_LOADED:         return "NOT_LOADED";
    case ConfigError::MISSING:            return "MISSING";
    case ConfigError::INVALID_TYPE:       return "INVALID_TYPE";
    case ConfigError::OUT_OF_RANGE:       return "OUT_OF_RANGE";
    case ConfigError::NOT_AUTHORIZED:     return "NOT_AUTHORIZED";
    case ConfigError::PERSISTENCE_FAILED: return "PERSISTENCE_FAILED";
    case ConfigError::LIMIT_REACHED:      return "LIMIT_REACHED";
    case ConfigError::NOT_FOUND:          return "NOT_FOUND";
    }
    return "UNKNOWN";
}

std::string reservedKey(std::uint32_t index)
{
    return std::string("reserved_") + (index < 10 ? "00" : index < 100 ? "0" : "") +
           std::to_string(index);
}

} // namespace

// ---------------------------------------------------------------------------
// ConfigStore
// ---------------------------------------------------------------------------

struct ConfigStore::Impl final
{
    mutable std::shared_mutex mutex;
    std::shared_ptr<json> snapshot;
    std::uint64_t version{0};
};

ConfigStore::ConfigStore()
    : impl_(std::make_unique<Impl>())
{
}

ConfigStore::~ConfigStore() = default;

ConfigLoadResult ConfigStore::load(const std::filesystem::path& file)
{
    ConfigLoadResult result;

    std::ifstream input(file, std::ios::binary);
    if (!input) {
        result.error = std::filesystem::exists(file) ? ConfigError::FILE_IO
                                                     : ConfigError::FILE_NOT_FOUND;
        result.diagnostics.push_back({file.string(), errorName(result.error), false});
        return result;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();

    json parsed;
    try {
        parsed = json::parse(buffer.str());
    } catch (const json::exception& e) {
        result.error = ConfigError::INVALID_JSON;
        result.diagnostics.push_back({"", e.what(), false});
        return result;
    }

    if (!validate(parsed, result.diagnostics)) {
        result.error = ConfigError::INVALID_SCHEMA;
        return result;
    }

    {
        std::unique_lock lock(impl_->mutex);
        impl_->snapshot = std::make_shared<json>(std::move(parsed));
        ++impl_->version;
    }
    result.success = true;
    result.error = ConfigError::OK;
    return result;
}

ConfigStatus ConfigStore::get(const char* path, ConfigValue& out) const noexcept
{
    try {
        std::shared_lock lock(impl_->mutex);
        if (!impl_->snapshot) {
            return ConfigStatus::NOT_LOADED;
        }
        const json* v = findPath(*impl_->snapshot, path ? path : "");
        if (!v) {
            return ConfigStatus::MISSING;
        }
        if (v->is_null()) {
            out = std::monostate{}; // 显式"待定"，与键缺失区分开
        } else if (v->is_string()) {
            out = v->get_ref<const std::string&>();
        } else if (v->is_boolean()) {
            out = v->get<bool>();
        } else if (integer(*v)) {
            out = intValue(*v);
        } else if (number(*v)) {
            out = v->get<double>();
        } else {
            return ConfigStatus::INVALID_TYPE; // 数组/对象本身：改用 getArraySize + 索引
        }
        return ConfigStatus::OK;
    } catch (...) {
        return ConfigStatus::INVALID_TYPE;
    }
}

ConfigStatus ConfigStore::getInt(const char* path, std::int64_t& out) const noexcept
{
    ConfigValue v;
    if (const auto status = get(path, v); status != ConfigStatus::OK) {
        return status;
    }
    const auto* p = std::get_if<std::int64_t>(&v);
    if (!p) {
        return ConfigStatus::INVALID_TYPE;
    }
    out = *p;
    return ConfigStatus::OK;
}

ConfigStatus ConfigStore::getDouble(const char* path, double& out) const noexcept
{
    ConfigValue v;
    if (const auto status = get(path, v); status != ConfigStatus::OK) {
        return status;
    }
    // JSON Schema 的 number 接受整数写法，因此整数按 double 取出（只放宽这一向；反向不放宽）
    if (const auto* p = std::get_if<double>(&v)) {
        out = *p;
        return ConfigStatus::OK;
    }
    if (const auto* i = std::get_if<std::int64_t>(&v)) {
        out = static_cast<double>(*i);
        return ConfigStatus::OK;
    }
    return ConfigStatus::INVALID_TYPE;
}

ConfigStatus ConfigStore::getBool(const char* path, bool& out) const noexcept
{
    ConfigValue v;
    if (const auto status = get(path, v); status != ConfigStatus::OK) {
        return status;
    }
    const auto* p = std::get_if<bool>(&v);
    if (!p) {
        return ConfigStatus::INVALID_TYPE;
    }
    out = *p;
    return ConfigStatus::OK;
}

ConfigStatus ConfigStore::getString(const char* path, std::string& out) const noexcept
{
    ConfigValue v;
    if (const auto status = get(path, v); status != ConfigStatus::OK) {
        return status;
    }
    const auto* p = std::get_if<std::string>(&v);
    if (!p) {
        return ConfigStatus::INVALID_TYPE;
    }
    out = *p;
    return ConfigStatus::OK;
}

ConfigStatus ConfigStore::getArraySize(const char* path,
                                       std::uint64_t& out) const noexcept
{
    try {
        std::shared_lock lock(impl_->mutex);
        if (!impl_->snapshot) {
            return ConfigStatus::NOT_LOADED;
        }
        const json* v = findPath(*impl_->snapshot, path ? path : "");
        if (!v) {
            return ConfigStatus::MISSING;
        }
        if (!v->is_array()) {
            return ConfigStatus::INVALID_TYPE;
        }
        out = v->size();
        return ConfigStatus::OK;
    } catch (...) {
        return ConfigStatus::INVALID_TYPE;
    }
}

std::uint64_t ConfigStore::configVersion() const noexcept
{
    try {
        std::shared_lock lock(impl_->mutex);
        return impl_->version;
    } catch (...) {
        return 0;
    }
}

PidReadResult ConfigStore::readPid(std::int64_t motor_id) const
{
    PidReadResult result;

    std::shared_lock lock(impl_->mutex);
    if (!impl_->snapshot) {
        result.message = "configuration is not loaded";
        return result;
    }
    const auto motors = member(impl_->snapshot.get(), "motor");
    if (!motors || !motors->is_array()) {
        result.error = ConfigError::INVALID_SCHEMA;
        result.message = "motor is not an array";
        return result;
    }

    for (const auto& motor : *motors) {
        const auto id = member(&motor, "id");
        if (!id || !integer(*id) || intValue(*id) != motor_id) {
            continue;
        }
        const auto pid = member(&motor, "pid");
        if (!pid || !pid->is_object()) {
            result.error = ConfigError::MISSING;
            result.message = "motor PID is missing";
            return result;
        }

        const std::pair<const char*, std::int64_t*> integers[] = {
            {"kp", &result.value.kp},
            {"ki", &result.value.ki},
            {"kd", &result.value.kd},
            {"integral_limit", &result.value.integral_limit},
            {"output_limit", &result.value.output_limit},
            {"control_period_ms", &result.value.control_period_ms},
        };
        for (const auto& [key, out] : integers) {
            const auto v = member(pid, key);
            if (!v || !integer(*v)) {
                result.error = ConfigError::INVALID_TYPE;
                result.message = "invalid PID integer";
                return result;
            }
            *out = intValue(*v);
        }
        const auto enabled = member(pid, "enabled");
        if (!enabled || !enabled->is_boolean()) {
            result.error = ConfigError::INVALID_TYPE;
            result.message = "invalid PID enabled flag";
            return result;
        }
        result.value.enabled = enabled->get<bool>();
        result.success = true;
        result.error = ConfigError::OK;
        return result;
    }

    result.error = ConfigError::NOT_FOUND;
    result.message = "motor id not found";
    return result;
}

PersistResult ConfigStore::readReserved(std::uint32_t index, std::int32_t& value) const
{
    if (index == 0 || index > 100) {
        return {false, false, false, ConfigError::OUT_OF_RANGE,
                "reserved index must be 1..100"};
    }
    std::shared_lock lock(impl_->mutex);
    if (!impl_->snapshot) {
        return {false, false, false, ConfigError::NOT_LOADED,
                "configuration is not loaded"};
    }
    const auto v = member(impl_->snapshot.get(), reservedKey(index));
    if (!v) {
        return {false, false, false, ConfigError::NOT_FOUND,
                "reserved variable not found"};
    }
    if (!integer(*v) || intValue(*v) < INT32_MIN || intValue(*v) > INT32_MAX) {
        return {false, false, false, ConfigError::INVALID_TYPE,
                "reserved variable is not int32"};
    }
    value = static_cast<std::int32_t>(intValue(*v));
    return {true, true, true, ConfigError::OK, "reserved variable read"};
}

} // namespace marshland::atom15
