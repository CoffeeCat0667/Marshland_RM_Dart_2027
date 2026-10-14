#include "atom04_can/motor_frame_codec.hpp"

namespace marshland::atom04 {
namespace {

using marshland::can::CanError;
using marshland::can::CanFrame;

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

CanError buildControlFrame(std::uint32_t frame_id, const MotorSlotValues& currents,
                           CanFrame& out) noexcept
{
    if (!marshland::can::isMotorControlFrameId(frame_id)) {
        return CanError::PARAM_INVALID;
    }

    CanFrame frame{};
    frame.can_id = frame_id;
    frame.dlc = marshland::can::kMotorControlDlc;

    for (std::size_t slot = 0; slot < marshland::can::kMotorSlotsPerFrame; ++slot) {
        // 缺陷 #3 修复：只编码**确有对应 CAN 节点**的槽位。
        // 0x1FF 的 slot2/slot3 在 Design/Config.md §6.2 中无定义节点，
        // 必须恒为 0x0000，不得把调用方传入的值当作虚构的 0x207/0x208 发出。
        std::uint32_t node_id = 0;
        if (!marshland::can::motorNodeOf(frame_id, slot, node_id)) {
            encodeBigEndianInt16(0, frame.payload.data() + (slot * 2U));
            continue;
        }
        encodeBigEndianInt16(currents[slot], frame.payload.data() + (slot * 2U));
    }

    out = frame;
    return CanError::OK;
}

CanError buildZeroTorqueFrame(std::uint32_t frame_id, CanFrame& out) noexcept
{
    if (!marshland::can::isMotorControlFrameId(frame_id)) {
        return CanError::PARAM_INVALID;
    }

    CanFrame frame{};
    frame.can_id = frame_id;
    frame.dlc = marshland::can::kMotorControlDlc;
    frame.payload.fill(0U);

    out = frame;
    return CanError::OK;
}

bool isValidControlFrame(const CanFrame& frame) noexcept
{
    return marshland::can::isMotorControlFrameId(frame.can_id) &&
           frame.dlc == marshland::can::kMotorControlDlc;
}

bool isZeroTorquePayload(const CanFrame& frame) noexcept
{
    if (frame.dlc != marshland::can::kMotorControlDlc) {
        return false;
    }
    for (std::size_t i = 0; i < marshland::can::kCanPayloadBytes; ++i) {
        if (frame.payload[i] != 0U) {
            return false;
        }
    }
    return true;
}

} // namespace marshland::atom04
