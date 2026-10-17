// ============================================================================
// motor_frame_codec.hpp — 电机控制帧大端序编解码（纯逻辑内核）
//
// 权威来源：Design/Config.md §6.2 报文接口表与字节序规则；
//           Design/Atomic/ATOM-04_CAN.md 电机 CAN 帧语义 / 零力矩安全帧。
//
// 依赖：仅 Atoms/Public/CanTypes.hpp 与 C++ 标准库。
// 本文件不访问设备、不持有线程、不读写共享状态。
// ============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "CanTypes.hpp"

namespace marshland::atom04 {

// 一组电机控制帧的载荷：4 个 int16 电流值，按槽位排列。
using MotorSlotValues = std::array<std::int16_t, marshland::can::kMotorSlotsPerFrame>;

// ---- 大端序原语（高字节先发） ---------------------------------------------
void encodeBigEndianInt16(std::int16_t value, std::uint8_t* destination) noexcept;
std::int16_t decodeBigEndianInt16(const std::uint8_t* source) noexcept;

// ---- 反馈帧解析（C620） ---------------------------------------------------
// 字节布局（用户 2026-10-16 更正为 C620 通行布局），全部大端序：
//   payload[0..1] 转子机械角度 int16（0~8191）  —— **不是转速**，切勿当作实测值
//   payload[2..3] 实际转速     int16            <- PID 的实测值来源
//   payload[4..5] 实际电流     int16
//   payload[6]    温度         uint8（℃）
//   payload[7]    空（忽略）
struct FeedbackPayload final {
    std::int16_t rpm{0};
    std::int16_t rotor_position{0};
    std::int16_t current{0};
    std::uint8_t temperature{0};
};

// 仅接受 DLC==8 的帧；DLC 不符返回 FRAME_INVALID 且不修改 out。
can::CanResult decodeFeedbackPayload(const can::CanFrame& frame,
                                     FeedbackPayload& out) noexcept;

// ---- 帧构建 ---------------------------------------------------------------
// frame_id 必须为 0x200 或 0x1FF，否则返回 PARAM_INVALID 且不修改 out。
// 无对应 CAN 节点的保留槽位（0x1FF 的 slot2/slot3）恒编码为 0x0000。
can::CanResult buildControlFrame(std::uint32_t frame_id,
                                           const MotorSlotValues& currents,
                                           can::CanFrame& out) noexcept;

// 零力矩帧：payload 严格为 8 个 0x00（Design/Config.md §6.2 零力矩帧 1/2）。
can::CanResult buildZeroTorqueFrame(std::uint32_t frame_id,
                                              can::CanFrame& out) noexcept;

// ---- 校验 ----------------------------------------------------------------
// 仅接受 0x200 / 0x1FF 且 DLC==8。反馈帧 0x201~0x206 必须判为 false。
bool isValidControlFrame(const can::CanFrame& frame) noexcept;
bool isZeroTorquePayload(const can::CanFrame& frame) noexcept;

} // namespace marshland::atom04
