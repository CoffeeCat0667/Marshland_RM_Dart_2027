#include "atom04_can/can_config.hpp"

#include <string_view>

namespace marshland::atom04 {
namespace {

using marshland::config::ConfigStatus;
using marshland::config::IConfigView;

constexpr std::int64_t kMinPeriodMs = 1;
constexpr std::int64_t kMaxPeriodMs = 60000;
constexpr std::int64_t kExpectedDataBits = 8;

std::string_view asView(const char* data, std::uint64_t size) noexcept
{
    if (data == nullptr) {
        return {};
    }
    return std::string_view(data, static_cast<std::size_t>(size));
}

// 读取"必须取既定数值"的整数键；缺失返回 MISSING，取值不符返回 OUT_OF_RANGE。
ConfigStatus readIntExact(const IConfigView& view, const char* key, std::int64_t expected) noexcept
{
    std::int64_t value = 0;
    const ConfigStatus status = view.getInt(key, value);
    if (status != ConfigStatus::OK) {
        return status;
    }
    if (value != expected) {
        return ConfigStatus::OUT_OF_RANGE;
    }
    return ConfigStatus::OK;
}

// 读取"必须取既定字面量"的字符串键；缺失、类型不符、取值不符分别如实返回。
ConfigStatus readExactString(const IConfigView& view, const char* key, const char* expected) noexcept
{
    const char* data = nullptr;
    std::uint64_t size = 0;
    const ConfigStatus status = view.getString(key, data, size);
    if (status != ConfigStatus::OK) {
        return status;
    }
    if (asView(data, size) != std::string_view(expected)) {
        return ConfigStatus::OUT_OF_RANGE;
    }
    return ConfigStatus::OK;
}

// 读取必须在 [1, 60000] ms 内的周期/超时键。
ConfigStatus readPeriod(const IConfigView& view, const char* key, int& out) noexcept
{
    std::int64_t value = 0;
    const ConfigStatus status = view.getInt(key, value);
    if (status != ConfigStatus::OK) {
        return status;
    }
    if (value < kMinPeriodMs || value > kMaxPeriodMs) {
        return ConfigStatus::OUT_OF_RANGE;
    }
    out = static_cast<int>(value);
    return ConfigStatus::OK;
}

// Design/Config.md §5.1 / §6.2：node 是十六进制字符串，形如 "0x201"。
bool parseHexNode(std::string_view text, std::uint32_t& out) noexcept
{
    if (text.empty()) {
        return false;
    }
    std::size_t index = 0;
    if (text.size() > 2U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        index = 2;
    }
    if (index >= text.size()) {
        return false;
    }

    std::uint32_t value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        std::uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint32_t>(c - 'a') + 10U;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint32_t>(c - 'A') + 10U;
        } else {
            return false;
        }
        value = (value << 4U) | digit;
        if (value > 0x1FFFFFFFU) {
            return false;  // 超出 29 位标准帧 ID 空间
        }
    }
    out = value;
    return true;
}

// 电机映射：只登记配置中真实存在的电机；node 越界或重复一律失败，不臆造槽位。
ConfigStatus readMotors(const IConfigView& view, CanRuntimeConfig& loaded) noexcept
{
    std::uint64_t count = 0;
    const ConfigStatus size_status = view.getArraySize("motor", count);
    if (size_status != ConfigStatus::OK) {
        return size_status;
    }
    if (count == 0U || count > marshland::can::kMaxMotorNodes) {
        return ConfigStatus::OUT_OF_RANGE;
    }

    loaded.motor_count = 0;

    for (std::uint64_t i = 0; i < count; ++i) {
        std::int64_t motor_id = 0;
        const ConfigStatus id_status = view.getIntAt("motor", i, "id", motor_id);
        if (id_status != ConfigStatus::OK) {
            return id_status;
        }
        if (motor_id <= 0 || motor_id > 255) {
            return ConfigStatus::OUT_OF_RANGE;
        }

        const char* node_data = nullptr;
        std::uint64_t node_size = 0;
        const ConfigStatus node_status = view.getStringAt("motor", i, "node", node_data, node_size);
        if (node_status != ConfigStatus::OK) {
            return node_status;
        }

        std::uint32_t node_id = 0;
        if (!parseHexNode(asView(node_data, node_size), node_id)) {
            return ConfigStatus::INVALID_TYPE;
        }
        if (!marshland::can::isMotorNode(node_id)) {
            // 不在 Design/Config.md §6.2 已定义的 0x201~0x206 内：拒绝，不猜。
            return ConfigStatus::OUT_OF_RANGE;
        }

        for (std::size_t k = 0; k < loaded.motor_count; ++k) {
            if (loaded.motors[k].motor_id == static_cast<int>(motor_id) ||
                loaded.motors[k].node_id == node_id) {
                return ConfigStatus::OUT_OF_RANGE;  // 重复映射
            }
        }

        // 槽位契约必须能覆盖该节点，否则整份配置不可用。
        std::uint32_t frame_id = 0;
        std::size_t slot = 0;
        if (!marshland::can::motorSlotOf(node_id, frame_id, slot)) {
            return ConfigStatus::OUT_OF_RANGE;
        }

        loaded.motors[loaded.motor_count].motor_id = static_cast<int>(motor_id);
        loaded.motors[loaded.motor_count].node_id = node_id;
        ++loaded.motor_count;
    }

    return ConfigStatus::OK;
}

} // namespace

ConfigStatus loadCanRuntimeConfig(const IConfigView& view, CanRuntimeConfig& config) noexcept
{
    if (!view.isLoaded()) {
        return ConfigStatus::NOT_LOADED;
    }

    // ---- 固定约束（Design/Config.md §6.1）----
    // 这些键必须显式存在并取既定值；缺失即失败，不使用内置默认值（§5 第 4 条）。
    const ConfigStatus data_bits = readIntExact(view, "can.data_bits", kExpectedDataBits);
    if (data_bits != ConfigStatus::OK) {
        return data_bits;
    }

    const ConfigStatus start_mode = readExactString(view, "can.start_mode", "CLASSIC_CAN_FD_OFF");
    if (start_mode != ConfigStatus::OK) {
        return start_mode;
    }

    const ConfigStatus rx_filter = readExactString(view, "can.rx_filter", "NONE");
    if (rx_filter != ConfigStatus::OK) {
        return rx_filter;
    }

    const ConfigStatus error_handling =
        readExactString(view, "can.error_frame_handling", "DISCARD");
    if (error_handling != ConfigStatus::OK) {
        return error_handling;
    }

    const ConfigStatus send_mode = readExactString(view, "can.zero_torque.send_mode", "OPEN_LOOP");
    if (send_mode != ConfigStatus::OK) {
        return send_mode;
    }

    const ConfigStatus silence =
        readExactString(view, "can.silence_trigger", "ZERO_TORQUE_SEND_FAILED");
    if (silence != ConfigStatus::OK) {
        return silence;
    }

    // ---- 组装到局部对象；任何失败都不得部分提交 ----
    CanRuntimeConfig loaded{};
    loaded.interface_name = kSocketCanInterfaceName;

    ConfigStatus status = readPeriod(view, "timing.can_io_period_ms", loaded.rx_period_ms);
    if (status != ConfigStatus::OK) {
        return status;
    }
    loaded.tx_period_ms = loaded.rx_period_ms;

    status = readPeriod(view, "timing.can_comm_timeout_ms", loaded.comm_timeout_ms);
    if (status != ConfigStatus::OK) {
        return status;
    }

    status = readPeriod(view, "timing.zero_torque_send_period_ms", loaded.zero_torque_period_ms);
    if (status != ConfigStatus::OK) {
        return status;
    }

    status = readMotors(view, loaded);
    if (status != ConfigStatus::OK) {
        return status;
    }

    loaded.zero_torque_frame_ids[0] = marshland::can::kMotorControlFrameIdA;
    loaded.zero_torque_frame_ids[1] = marshland::can::kMotorControlFrameIdB;

    config = loaded;
    return ConfigStatus::OK;
}

} // namespace marshland::atom04
