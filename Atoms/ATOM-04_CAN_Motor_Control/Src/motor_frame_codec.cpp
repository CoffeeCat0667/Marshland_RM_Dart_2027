#include "atom04_can/motor_frame_codec.hpp"

namespace marshland::atom04 {
namespace {
// 本地：控制帧 ID 判定（不再作为公共 API 暴露）。
constexpr bool isControlFrameId(std::uint32_t can_id) noexcept
{
    return can_id == can::kMotorControlFrameIdA || can_id == can::kMotorControlFrameIdB;
}


using can::CanError;
using can::CanFrame;
using can::CanResult;

// Design/Config.md §6.2：int16 先写高字节、再写低字节。
// 用位运算而非 reinterpret_cast，避免对齐与严格别名问题。
constexpr std::uint8_t highByte(std::int16_t value) noexcept
{
    return static_cast<std::uint8_t>((static_cast<std::uint16_t>(value) >> 8U) & 0xFFU);
}

constexpr std::uint8_t lowByte(std::int16_t value) noexcept
{
    return static_cast<std::uint8_t>(static_cast<std::uint16_t>(value) & 0xFFU);
}

// C620 反馈帧字节偏移（用户 2026-10-16 更正为 C620 通行布局）：
//   字节 0..1 转子机械角度（0~8191）
//   字节 2..3 实际转速（PID 的实测值来源）
constexpr std::size_t kPositionOffset = 0U;
constexpr std::size_t kRpmOffset = 2U;
constexpr std::size_t kCurrentOffset = 4U;
constexpr std::size_t kTemperatureOffset = 6U;

} // namespace

void encodeBigEndianInt16(std::int16_t value, std::uint8_t* destination) noexcept
{
    if (destination == nullptr) {
        return;
    }
    destination[0] = highByte(value);
    destination[1] = lowByte(value);
}

std::int16_t decodeBigEndianInt16(const std::uint8_t* source) noexcept
{
    if (source == nullptr) {
        return 0;
    }
    const std::uint16_t raw = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(source[0]) << 8U) | static_cast<std::uint16_t>(source[1]));
    return static_cast<std::int16_t>(raw);
}

CanResult decodeFeedbackPayload(const CanFrame& frame, FeedbackPayload& out) noexcept
{
    if (frame.dlc != can::kCanPayloadBytes) {
        return CanResult{true, CanError::FRAME_INVALID, 0};
    }

    FeedbackPayload decoded{};
    // 偏移量与 motor_frame_codec.hpp 中的布局说明严格对应。
    decoded.rpm = decodeBigEndianInt16(frame.payload.data() + kRpmOffset);
    decoded.rotor_position = decodeBigEndianInt16(frame.payload.data() + kPositionOffset);
    decoded.current = decodeBigEndianInt16(frame.payload.data() + kCurrentOffset);
    decoded.temperature = frame.payload[kTemperatureOffset];

    out = decoded;
    return CanResult{};
}

CanResult buildControlFrame(std::uint32_t frame_id, const MotorSlotValues& currents,
                           CanFrame& out) noexcept
{
    if (!isControlFrameId(frame_id)) {
        return CanResult{true, CanError::PARAM_INVALID, 0};
    }

    CanFrame frame{};
    frame.can_id = frame_id;
    frame.dlc = can::kCanPayloadBytes;

    for (std::size_t slot = 0; slot < can::kMotorSlotsPerFrame; ++slot) {
        // 只编码**确有对应 CAN 节点**的槽位。
        // Design/Config.md §6.2：0x200 承载 0x201~0x204（slot 0..3）；
        // 0x1FF 承载 0x205/0x206（slot 0/1），slot2/slot3 无定义节点，
        // 必须恒为 0x0000，不得把调用方传入的值当作虚构的 0x207/0x208 发出。
        const bool slot_has_node =
            (frame_id == can::kMotorControlFrameIdA) ||
            (frame_id == can::kMotorControlFrameIdB && slot < 2U);
        if (!slot_has_node) {
            encodeBigEndianInt16(0, frame.payload.data() + (slot * 2U));
            continue;
        }
        encodeBigEndianInt16(currents[slot], frame.payload.data() + (slot * 2U));
    }

    out = frame;
    return CanResult{};
}

CanResult buildZeroTorqueFrame(std::uint32_t frame_id, CanFrame& out) noexcept
{
    if (!isControlFrameId(frame_id)) {
        return CanResult{true, CanError::PARAM_INVALID, 0};
    }

    CanFrame frame{};
    frame.can_id = frame_id;
    frame.dlc = can::kCanPayloadBytes;
    frame.payload.fill(0U);

    out = frame;
    return CanResult{};
}

bool isValidControlFrame(const CanFrame& frame) noexcept
{
    return isControlFrameId(frame.can_id) &&
           frame.dlc == can::kCanPayloadBytes;
}

bool isZeroTorquePayload(const CanFrame& frame) noexcept
{
    if (frame.dlc != can::kCanPayloadBytes) {
        return false;
    }
    for (std::size_t i = 0; i < can::kCanPayloadBytes; ++i) {
        if (frame.payload[i] != 0U) {
            return false;
        }
    }
    return true;
}

} // namespace marshland::atom04
