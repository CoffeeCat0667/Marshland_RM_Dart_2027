#include "atom15_config/config_store.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string_view>
#include <type_traits>
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

const json* findPath(const json& root, std::string_view key)
{
    const json* v = &root;
    std::size_t begin = 0;
    while (begin < key.size()) {
        const auto end = key.find('.', begin);
        const auto segment = key.substr(
            begin, end == std::string_view::npos ? key.size() - begin : end - begin);
        v = member(v, segment);
        if (!v) {
            return nullptr;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return v;
}

json* mutablePath(json& root, std::string_view key)
{
    json* v = &root;
    std::size_t begin = 0;
    while (begin < key.size()) {
        const auto end = key.find('.', begin);
        const auto segment = key.substr(
            begin, end == std::string_view::npos ? key.size() - begin : end - begin);
        v = member(v, segment);
        if (!v) {
            return nullptr;
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

// ---------------------------------------------------------------------------
// 运行时更新辅助
// ---------------------------------------------------------------------------

json toJson(const ConfigValue& value)
{
    return std::visit(
        [](const auto& x) -> json {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return nullptr; // 显式"待定"
            } else {
                return json(x);
            }
        },
        value);
}

// 类型预检；最终裁决仍是候选快照的完整 Schema 校验。
// 当前值为 null 表示"待定占位"，允许写入具体值；已定值不允许再改回 null。
bool compatible(const json& current, const ConfigValue& value)
{
    if (current.is_null()) {
        return true;
    }
    if (std::holds_alternative<std::monostate>(value)) {
        return false;
    }
    return std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                return integer(current);
            } else if constexpr (std::is_same_v<T, double>) {
                return number(current);
            } else if constexpr (std::is_same_v<T, bool>) {
                return current.is_boolean();
            } else {
                return current.is_string();
            }
        },
        value);
}

bool allowed(UpdateSource source, UpdateMode mode) noexcept
{
    return source == UpdateSource::WEB &&
           (mode == UpdateMode::MANUAL || mode == UpdateMode::DEBUG);
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

// 候选快照的统一收尾：校验 → 替换活动快照 → 普通文件写入。
// 前置条件：调用方已独占持有 mutex，且 snapshot 非空。
PersistResult commitCandidate(std::shared_ptr<json>& snapshot,
                              const std::filesystem::path& file_path,
                              std::uint64_t& version,
                              json candidate,
                              const char* violation_message,
                              const char* ok_message,
                              const char* persist_failed_message)
{
    std::vector<ConfigDiagnostic> validation;
    if (!validate(candidate, validation)) {
        return {false, false, false, ConfigError::OUT_OF_RANGE,
                validation.empty()
                    ? violation_message
                    : validation.front().key + ": " + validation.front().message};
    }

    const std::string text = candidate.dump();
    snapshot = std::make_shared<json>(std::move(candidate));
    ++version;

    PersistResult result{true, true, false, ConfigError::PERSISTENCE_FAILED,
                         persist_failed_message};
    std::ofstream output(file_path, std::ios::binary | std::ios::trunc);
    if (output) {
        output << text;
        result.persisted = static_cast<bool>(output);
        if (result.persisted) {
            result.error = ConfigError::OK;
            result.message = ok_message;
        }
    }
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// ConfigStore
// ---------------------------------------------------------------------------

struct ConfigStore::Impl final
{
    mutable std::shared_mutex mutex;
    std::shared_ptr<json> snapshot;
    std::filesystem::path file_path;
    std::vector<ConfigDiagnostic> diagnostics;
    ChangeCallback callback;
    std::uint64_t version{0};
};

ConfigStore::ConfigStore(ChangeCallback callback)
    : impl_(std::make_unique<Impl>())
{
    impl_->callback = std::move(callback);
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
        impl_->file_path = file;
        impl_->diagnostics = result.diagnostics;
        ++impl_->version;
    }
    result.success = true;
    result.error = ConfigError::OK;
    return result;
}

ConfigLoadResult ConfigStore::reload()
{
    std::filesystem::path path;
    {
        std::shared_lock lock(impl_->mutex);
        path = impl_->file_path;
    }
    if (path.empty()) {
        return {false, ConfigError::NOT_LOADED, {{"", "no configuration path", false}}};
    }
    return load(path);
}

std::filesystem::path ConfigStore::path() const
{
    std::shared_lock lock(impl_->mutex);
    return impl_->file_path;
}

std::vector<ConfigDiagnostic> ConfigStore::diagnostics() const
{
    std::shared_lock lock(impl_->mutex);
    return impl_->diagnostics;
}

void ConfigStore::setChangeCallback(ChangeCallback callback)
{
    std::unique_lock lock(impl_->mutex);
    impl_->callback = std::move(callback);
}

PersistResult ConfigStore::updateRuntime(const char* key, const ConfigValue& value,
                                         UpdateSource source, UpdateMode mode)
{
    const Assignment single{key, value};
    return updateRuntime(std::span<const Assignment>(&single, 1), source, mode);
}

PersistResult ConfigStore::updateRuntime(std::span<const Assignment> assignments,
                                         UpdateSource source, UpdateMode mode)
{
    if (assignments.empty()) {
        return {false, false, false, ConfigError::MISSING, "no assignment"};
    }
    if (!allowed(source, mode)) {
        return {false, false, false, ConfigError::NOT_AUTHORIZED,
                "only WEB in MANUAL or DEBUG may update"};
    }

    ChangeCallback callback;
    std::uint64_t version = 0;
    PersistResult result;
    {
        std::unique_lock lock(impl_->mutex);
        if (!impl_->snapshot) {
            return {false, false, false, ConfigError::NOT_LOADED,
                    "configuration is not loaded"};
        }

        // 整批落在同一候选快照上：任一条不合法则整批丢弃，活动快照、版本、文件均不变。
        json candidate = *impl_->snapshot;
        for (const auto& assignment : assignments) {
            if (!assignment.key || !*assignment.key) {
                return {false, false, false, ConfigError::MISSING, "empty key"};
            }
            const json* original = findPath(*impl_->snapshot, assignment.key);
            if (!original) {
                return {false, false, false, ConfigError::MISSING,
                        "unknown configuration key"};
            }
            if (!compatible(*original, assignment.value)) {
                return {false, false, false, ConfigError::INVALID_TYPE,
                        "value type does not match"};
            }
            if (const auto numeric = std::get_if<double>(&assignment.value);
                numeric && !std::isfinite(*numeric)) {
                return {false, false, false, ConfigError::OUT_OF_RANGE,
                        "value must be finite"};
            }
            json* current = mutablePath(candidate, assignment.key);
            if (!current) {
                return {false, false, false, ConfigError::MISSING,
                        "unknown configuration key"};
            }
            *current = toJson(assignment.value);
        }

        result = commitCandidate(impl_->snapshot, impl_->file_path, impl_->version,
                                 std::move(candidate),
                                 "updated value violates configuration schema",
                                 "configuration updated",
                                 "effective value updated but persistence failed");
        if (result.effective) {
            callback = impl_->callback;
            version = impl_->version;
        }
    }
    if (callback) {
        for (const auto& assignment : assignments) {
            callback(assignment.key, version);
        }
    }
    return result;
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

PersistResult ConfigStore::writePid(std::int64_t motor_id, const PidConfig& value,
                                    UpdateSource source, UpdateMode mode)
{
    if (!allowed(source, mode)) {
        return {false, false, false, ConfigError::NOT_AUTHORIZED,
                "only WEB in MANUAL or DEBUG may update PID"};
    }
    if (value.integral_limit < 0 || value.output_limit < 0 ||
        value.control_period_ms <= 0) {
        return {false, false, false, ConfigError::OUT_OF_RANGE,
                "invalid PID limits or period"};
    }

    ChangeCallback callback;
    std::uint64_t version = 0;
    PersistResult result;
    {
        std::unique_lock lock(impl_->mutex);
        if (!impl_->snapshot) {
            return {false, false, false, ConfigError::NOT_LOADED,
                    "configuration is not loaded"};
        }

        json candidate = *impl_->snapshot;
        json* target = nullptr;
        if (auto motors = member(&candidate, "motor"); motors && motors->is_array()) {
            for (auto& motor : *motors) {
                auto id = member(&motor, "id");
                if (id && integer(*id) && intValue(*id) == motor_id) {
                    target = &motor;
                }
            }
        }
        if (!target) {
            return {false, false, false, ConfigError::NOT_FOUND, "motor id not found"};
        }
        auto pid = member(target, "pid");
        if (!pid || !pid->is_object()) {
            return {false, false, false, ConfigError::MISSING, "motor PID is missing"};
        }

        const std::pair<const char*, json> fields[] = {
            {"kp", json(value.kp)},
            {"ki", json(value.ki)},
            {"kd", json(value.kd)},
            {"integral_limit", json(value.integral_limit)},
            {"output_limit", json(value.output_limit)},
            {"control_period_ms", json(value.control_period_ms)},
            {"enabled", json(value.enabled)},
        };
        for (const auto& [key, v] : fields) {
            auto field = member(pid, key);
            if (!field) {
                return {false, false, false, ConfigError::MISSING,
                        std::string("missing PID field: ") + key};
            }
            *field = v;
        }

        result = commitCandidate(impl_->snapshot, impl_->file_path, impl_->version,
                                 std::move(candidate),
                                 "PID violates configuration schema",
                                 "PID updated",
                                 "effective PID updated but persistence failed");
        if (result.effective) {
            callback = impl_->callback;
            version = impl_->version;
        }
    }
    if (callback) {
        callback("motor.pid", version);
    }
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

PersistResult ConfigStore::writeReserved(std::uint32_t index, std::int32_t value,
                                         UpdateSource source, UpdateMode mode)
{
    if (index == 0 || index > 100) {
        return {false, false, false, ConfigError::OUT_OF_RANGE,
                "reserved index must be 1..100"};
    }
    if (!allowed(source, mode)) {
        return {false, false, false, ConfigError::NOT_AUTHORIZED,
                "only WEB in MANUAL or DEBUG may update"};
    }

    const auto key = reservedKey(index);
    ChangeCallback callback;
    std::uint64_t version = 0;
    PersistResult result;
    {
        std::unique_lock lock(impl_->mutex);
        if (!impl_->snapshot) {
            return {false, false, false, ConfigError::NOT_LOADED,
                    "configuration is not loaded"};
        }

        json candidate = *impl_->snapshot;
        candidate[key] = value;

        result = commitCandidate(impl_->snapshot, impl_->file_path, impl_->version,
                                 std::move(candidate),
                                 "reserved variable violates configuration schema",
                                 "reserved variable updated",
                                 "effective reserved variable updated but persistence failed");
        if (result.effective) {
            callback = impl_->callback;
            version = impl_->version;
        }
    }
    if (callback) {
        callback(key, version);
    }
    return result;
}

} // namespace marshland::atom15
