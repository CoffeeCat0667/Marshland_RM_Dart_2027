#include "atom04_can/protocol_dispatcher.hpp"

namespace marshland::atom04 {

using marshland::can::CanError;
using marshland::can::CanFrame;
using marshland::can::CanNodeState;
using marshland::can::CanNodeStatus;

const MotorEndpoint* ProtocolDispatcher::findMotor(std::uint32_t node_id) const noexcept
{
    for (std::size_t i = 0; i < config_.motor_count; ++i) {
        if (config_.motors[i].node_id == node_id) {
            return &config_.motors[i];
        }
    }
    return nullptr;
}

bool ProtocolDispatcher::registerNodes() noexcept
{
    if (config_.motor_count == 0U || config_.motor_count > marshland::can::kMaxMotorNodes) {
        return false;
    }

    CanNodeStatus nodes[marshland::can::kMaxMotorNodes]{};
    for (std::size_t i = 0; i < config_.motor_count; ++i) {
        nodes[i].motor_id = config_.motors[i].motor_id;
        nodes[i].node_id = config_.motors[i].node_id;
        nodes[i].state = CanNodeState::UNKNOWN;
        nodes[i].has_received = false;
        nodes[i].last_frame_time_us = 0;
        nodes[i].frame_count = 0;
    }

    // comm_timeout_ms 已在配置装载时校验为正 bounded int（1~60000），
    // 先转 uint64_t 再乘，避免 int * unsigned 的符号转换告警。
    const std::uint64_t timeout_ms = static_cast<std::uint64_t>(config_.comm_timeout_ms);
    timeout_us_ = timeout_ms * 1000ULL;
    return store_.replaceAll(nodes, config_.motor_count);
}

DispatchOutcome ProtocolDispatcher::onReceived(const CanFrame& frame) noexcept
{
    DispatchOutcome outcome{};
    outcome.error = CanError::OK;

    if (!marshland::can::isMotorNode(frame.can_id)) {
        // 未识别 ID：不投递、不解析、不静默丢弃（交由调用方计数与诊断）。
        outcome.error = CanError::UNKNOWN_ID;
        return outcome;
    }

    const std::uint32_t node_id = frame.can_id;
    const MotorEndpoint* motor = findMotor(node_id);
    if (motor == nullptr) {
        // 落在 0x201~0x206 但配置未登记：不得臆造电机，也不得投递。
        outcome.error = CanError::UNKNOWN_ID;
        return outcome;
    }

    outcome.node_id = node_id;
    outcome.motor_id = motor->motor_id;

    CanNodeStatus current{};
    current.motor_id = motor->motor_id;
    current.node_id = node_id;
    current.state = CanNodeState::ONLINE;
    current.has_received = true;
    current.last_frame_time_us = frame.timestamp_us;
    current.frame_count = 1;

    // 读取旧状态只为判断是否发生 ONLINE 跃变；读取失败视为首次在线。
    CanNodeStatus existing[marshland::can::kMaxMotorNodes]{};
    const std::size_t found = store_.snapshot(existing, marshland::can::kMaxMotorNodes);
    bool seen = false;
    for (std::size_t i = 0; i < found; ++i) {
        if (existing[i].node_id == node_id) {
            current.frame_count = existing[i].frame_count + 1U;
            outcome.node_became_online = existing[i].state != CanNodeState::ONLINE;
            seen = true;
            break;
        }
    }
    if (!seen) {
        outcome.node_became_online = true;
    }

    if (!store_.replaceOne(node_id, current)) {
        return outcome;
    }

    // 原样投递：不做任何字段解析（反馈解析归 ATOM-05）。
    sink_.onCanFrame(frame);
    return outcome;
}

std::size_t ProtocolDispatcher::pollTimeouts(std::uint64_t now_us,
                                             CanNodeStatus* expired_output,
                                             std::size_t capacity) noexcept
{
    if (timeout_us_ == 0U) {
        return 0;
    }

    CanNodeStatus nodes[marshland::can::kMaxMotorNodes]{};
    const std::size_t count = store_.snapshot(nodes, marshland::can::kMaxMotorNodes);
    std::size_t expired = 0;

    for (std::size_t i = 0; i < count; ++i) {
        if (nodes[i].state != CanNodeState::ONLINE) {
            continue;
        }
        // 未收到过反馈的节点由 registerNodes 置为 UNKNOWN，不会被本函数处理；
        // 因此这里可以安全地使用"已收到过"标志，避免时钟起点附近的误判。
        const bool timed_out = nodes[i].has_received &&
                               (now_us > nodes[i].last_frame_time_us &&
                                (now_us - nodes[i].last_frame_time_us) >= timeout_us_);
        if (!timed_out) {
            continue;
        }

        CanNodeStatus updated = nodes[i];
        updated.state = CanNodeState::TIMEOUT;
        if (!store_.replaceOne(updated.node_id, updated)) {
            continue;
        }
        if (expired_output != nullptr && expired < capacity) {
            expired_output[expired] = updated;
        }
        ++expired;
    }

    return expired;
}

bool ProtocolDispatcher::markFault(std::uint32_t node_id) noexcept
{
    CanNodeStatus nodes[marshland::can::kMaxMotorNodes]{};
    const std::size_t count = store_.snapshot(nodes, marshland::can::kMaxMotorNodes);
    for (std::size_t i = 0; i < count; ++i) {
        if (nodes[i].node_id != node_id) {
            continue;
        }
        CanNodeStatus updated = nodes[i];
        updated.state = CanNodeState::FAULT;
        return store_.replaceOne(node_id, updated);
    }
    return false;
}

} // namespace marshland::atom04
