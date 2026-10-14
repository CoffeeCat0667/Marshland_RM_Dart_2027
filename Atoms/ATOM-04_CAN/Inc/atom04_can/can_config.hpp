// ============================================================================
// can_config.hpp — ATOM-04 运行参数装载（IConfigView -> CanRuntimeConfig）
//
// 权威来源：Rule/ConfigSchema.md（键名的唯一来源）、Design/Config.md §5/§6（数值）
//           Rule/AtomIntegration.md §2.3 第 4 条、§5（配置使用）
//
// 硬约束：
//   * 只经 IConfigView 查询，禁止自解析 JSON；
//   * 逐项检查 ConfigStatus，非 OK 一律失败并区分原因；
//   * 禁止臆造默认值；禁止把缺失键当作可选而静默填值。
//
// 接口名偏差（已授权）：Config/dart_control.nlohmann 与 Design/Config.md §6.1 写
// "CAN0"，但 Linux SocketCAN 接口名区分大小写，本模块固定使用 "can0"。
// 该偏差登记于 Rule/ATOM4ConnectProject.md 与 PLAN.md。
// ============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "CanTypes.hpp"
#include "IConfigView.hpp"

namespace marshland::atom04 {

// 实际使用的 Linux 接口名。
// 偏差登记：Design/Config.md §6.1 与 Config/dart_control.nlohmann 写 "CAN0"，
// 但 Linux SocketCAN 接口名区分大小写；用户裁决使用 "can0"。
// 详见 Rule/ATOM4ConnectProject.md §10 与 PLAN.md §12。
inline constexpr const char* kSocketCanInterfaceName = "can0";

struct MotorEndpoint final {
    int motor_id{0};
    std::uint32_t node_id{0};
};

struct CanRuntimeConfig final {
    std::string interface_name{kSocketCanInterfaceName};
    int rx_period_ms{0};            // timing.can_io_period_ms（接收阻塞/轮询周期）
    int tx_period_ms{0};            // timing.can_io_period_ms（控制帧发送周期）
    int comm_timeout_ms{0};         // timing.can_comm_timeout_ms（节点离线判定，200）
    int zero_torque_period_ms{0};   // timing.zero_torque_send_period_ms（零力矩周期，20）

    std::size_t motor_count{0};
    std::array<MotorEndpoint, marshland::can::kMaxMotorNodes> motors{};

    std::array<std::uint32_t, marshland::can::kMotorControlFrameCount> zero_torque_frame_ids{
        marshland::can::kMotorControlFrameIdA, marshland::can::kMotorControlFrameIdB};
};

// 返回 ConfigStatus::OK 表示全部配置项已校验通过且 config 被完整写入。
// 任何非 OK 返回都意味着 config 不可用于驱动执行器，调用方必须让 initialize() 失败。
config::ConfigStatus loadCanRuntimeConfig(const config::IConfigView& view,
                                          CanRuntimeConfig& config) noexcept;

} // namespace marshland::atom04
