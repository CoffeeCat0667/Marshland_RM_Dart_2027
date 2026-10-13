#include "atom04_can/can_core.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>

namespace marshland::atom04 {

namespace {

using marshland::can::CanError;
using marshland::can::CanEvent;
using marshland::can::CanEventKind;
using marshland::can::CanFrame;
using marshland::can::CanIoResult;
using marshland::can::CanNodeState;
using marshland::can::CanNodeStatus;

constexpr int kRxPollSliceMs = 50;

// 持续故障时的轮询退避：防止总线持续错误导致 RX 线程空转占满 CPU。
constexpr auto kErrorBackoff = std::chrono::milliseconds(5);

// 同一总线错误的最大事件发布次数，超出后只计数（诊断仍可查 busStatus）。
constexpr std::uint64_t kMaxBusErrorEvents = 64ULL;

// 零力矩安全帧的固定 ID 集合（Design/Config.md §6.2 零力矩帧 1/2）。
constexpr std::uint32_t kControlledFrameIds[] = {marshland::can::kMotorControlFrameIdA,
                                                 marshland::can::kMotorControlFrameIdB};

constexpr std::size_t frameIndex(std::uint32_t frame_id) noexcept
{
    return frame_id == marshland::can::kMotorControlFrameIdA ? 0U : 1U;
}

constexpr bool isControlledFrameId(std::uint32_t frame_id) noexcept
{
    return frame_id == marshland::can::kMotorControlFrameIdA ||
           frame_id == marshland::can::kMotorControlFrameIdB;
}

} // namespace

CanCore::CanCore(const Dependencies& dependencies) noexcept
    : dependencies_(dependencies)
    , dispatcher_(dependencies.frame_sink, dependencies.node_store)
{
}

CanCore::~CanCore()
{
    stop();
}

std::uint64_t CanCore::nowMicroseconds() noexcept
{
    const auto now = Clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

bool CanCore::start(const CanRuntimeConfig& config, std::string& error_detail) noexcept
{
    if (running_.load(std::memory_order_acquire)) {
        return true;  // 幂等
    }
    if (config.motor_count == 0U || config.rx_period_ms <= 0 || config.tx_period_ms <= 0 ||
        config.comm_timeout_ms <= 0 || config.zero_torque_period_ms <= 0) {
        error_detail = "can runtime config is incomplete (motor table or timing periods)";
        return false;
    }

    config_ = config;
    dispatcher_.configure(config_);

    if (!dispatcher_.registerNodes()) {
        error_detail = "failed to register CAN node status entries";
        return false;
    }

    const CanIoResult opened = dependencies_.transport.open(config_.interface_name.c_str());
    if (!opened) {
        error_detail = "SocketCAN open failed: ";
        error_detail += marshland::can::toString(opened.error);
        error_detail += " (errno=";
        error_detail += std::to_string(opened.system_errno);
        error_detail += ", interface='";
        error_detail += config_.interface_name;
        error_detail += "')";
        return false;
    }

    running_.store(true, std::memory_order_release);

    try {
        receive_thread_ = std::thread(&CanCore::receiveLoop, this);
        transmit_thread_ = std::thread(&CanCore::transmitLoop, this);
    } catch (const std::system_error& error) {
        running_.store(false, std::memory_order_release);
        dependencies_.transport.close();
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        error_detail = "failed to start CAN worker threads: ";
        error_detail += error.what();
        return false;
    }

    return true;
}

void CanCore::stop() noexcept
{
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        // 未运行：仍确保 fd 与线程状态干净（对重复调用安全）。
        if (receive_thread_.joinable()) {
            receive_thread_.join();
        }
        if (transmit_thread_.joinable()) {
            transmit_thread_.join();
        }
        return;
    }

    // 先关闭传输以打断可能阻塞在 poll/recv 的接收线程。
    dependencies_.transport.close();

    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
    if (transmit_thread_.joinable()) {
        transmit_thread_.join();
    }
}

void CanCore::publishEvent(const CanEvent& event) noexcept
{
    if (dependencies_.event_sink != nullptr) {
        dependencies_.event_sink->onCanEvent(event);
    }
}

void CanCore::notifyZeroTorqueFailure(const CanIoResult& result) noexcept
{
    if (dependencies_.safety_notifier != nullptr) {
        dependencies_.safety_notifier->onZeroTorqueSendFailed(result);
    }
}

// 持续性总线错误按"错误码变化或出现次数为 2 的幂"发布，避免故障时刷爆事件通道。
bool CanCore::shouldPublishBusError(CanError error) noexcept
{
    const std::uint64_t occurrences =
        bus_error_count_.fetch_add(1ULL, std::memory_order_relaxed) + 1ULL;

    if (occurrences == 1ULL) {
        last_published_error_.store(static_cast<int>(error), std::memory_order_relaxed);
        return true;  // 首次立即发布
    }
    const int previous = last_published_error_.exchange(
        static_cast<int>(error), std::memory_order_relaxed);
    if (previous != static_cast<int>(error)) {
        return true;  // 错误码变化立即发布
    }
    if (occurrences >= kMaxBusErrorEvents) {
        return false;  // 已达上限：只计数，不再刷事件
    }
    return (occurrences & (occurrences - 1ULL)) == 0ULL;  // 2 的幂时发布
}

// 节点状态跃变是按边沿产生的一次性事件，不做节流（否则会丢真实跃变）。
void CanCore::publishNodeEvent(const CanEvent& event) noexcept
{
    published_node_count_.fetch_add(1ULL, std::memory_order_relaxed);
    publishEvent(event);
}

void CanCore::sendLatchedFrames() noexcept
{
    std::array<MotorSlotValues, marshland::can::kMotorControlFrameCount> snapshot{};
    {
        std::lock_guard<std::mutex> lock(latch_mutex_);
        snapshot = latched_;
    }

    for (const std::uint32_t frame_id : kControlledFrameIds) {
        CanFrame frame{};
        if (buildControlFrame(frame_id, snapshot[frameIndex(frame_id)], frame) != CanError::OK) {
            continue;
        }
        const CanIoResult sent = dependencies_.transport.send(frame);
        if (sent) {
            continue;
        }

        CanEvent event{};
        event.kind = CanEventKind::BUS_ERROR;
        event.error = sent.error;
        event.system_errno = sent.system_errno;
        event.timestamp_us = nowMicroseconds();
        if (shouldPublishBusError(event.error)) {
            publishEvent(event);
        }

        // 发送失败后锁存值立即归零：不允许后续周期继续重发可能失效的控制量。
        std::lock_guard<std::mutex> lock(latch_mutex_);
        for (std::size_t slot = 0; slot < marshland::can::kMotorSlotsPerFrame; ++slot) {
            latched_[frameIndex(frame_id)][slot] = 0;
        }
    }
}

marshland::can::CanBusStatus CanCore::snapshotBusStatus() const noexcept
{
    marshland::can::CanBusStatus status = dependencies_.transport.busStatus();
    status.unknown_id_count = unknown_id_count_.load(std::memory_order_relaxed);
    // config() 为 const 访问器；dispatcher 的 config() 同为 const。
    const CanRuntimeConfig& config =
        const_cast<ProtocolDispatcher&>(dispatcher_).config();
    if (config.motor_count == 0U) {
        status.open = false;
    }
    return status;
}

void CanCore::receiveLoop() noexcept
{
    while (running_.load(std::memory_order_acquire)) {
        CanFrame frame{};
        const CanIoResult received = dependencies_.transport.receive(frame, kRxPollSliceMs);

        if (received.error == CanError::NOT_OPEN) {
            break;  // stop() 已关闭传输，或接口被移除
        }

        if (received) {
            frame.timestamp_us = nowMicroseconds();
            const DispatchOutcome outcome = dispatcher_.onReceived(frame);
            if (outcome.error == CanError::UNKNOWN_ID) {
                unknown_id_count_.fetch_add(1ULL, std::memory_order_relaxed);
            } else if (outcome.node_became_online) {
                CanEvent event{};
                event.kind = CanEventKind::NODE_STATE_CHANGE;
                event.error = CanError::OK;
                event.motor_id = outcome.motor_id;
                event.node_id = outcome.node_id;
                event.timestamp_us = frame.timestamp_us;
                publishNodeEvent(event);
            }
        } else if (received.error != CanError::RECEIVE_TIMEOUT) {
            // 持续性总线错误：节流发布 + 退避，避免事件风暴与 CPU 空转。
            CanEvent event{};
            event.kind = CanEventKind::BUS_ERROR;
            event.error = received.error;
            event.system_errno = received.system_errno;
            event.timestamp_us = nowMicroseconds();
            if (shouldPublishBusError(event.error)) {
                publishEvent(event);
            }
        }

        // 缺陷 D2 修复：超时检查原先只在"读超时"分支执行，持续帧流会饿死离线判定。
        // 现在每个循环周期都无条件执行。
        CanNodeStatus expired[marshland::can::kMaxMotorNodes]{};
        const std::size_t count =
            dispatcher_.pollTimeouts(nowMicroseconds(), expired, marshland::can::kMaxMotorNodes);
        for (std::size_t i = 0; i < count; ++i) {
            CanEvent event{};
            event.kind = CanEventKind::NODE_STATE_CHANGE;
            event.error = CanError::RECEIVE_TIMEOUT;
            event.motor_id = expired[i].motor_id;
            event.node_id = expired[i].node_id;
            event.timestamp_us = nowMicroseconds();
            publishNodeEvent(event);
        }

        if (!received && received.error != CanError::RECEIVE_TIMEOUT) {
            std::this_thread::sleep_for(kErrorBackoff);
        }
    }
}

void CanCore::transmitLoop() noexcept
{
    const auto control_period = std::chrono::milliseconds(config_.tx_period_ms);
    const auto zero_torque_period = std::chrono::milliseconds(config_.zero_torque_period_ms);

    // Config.md §15：固定周期任务使用单调时钟**绝对期限**推进，并监测超期。
    auto next_control = Clock::now() + control_period;
    auto next_zero_torque = Clock::now() + zero_torque_period;

    while (running_.load(std::memory_order_acquire)) {
        const auto now = Clock::now();

        // 安全优先：零力矩帧总是允许发送（Design/Config.md §15 安全动作例外）。
        if (now >= next_zero_torque) {
            (void)sendZeroTorque();
            next_zero_torque += zero_torque_period;
            if (next_zero_torque <= now) {
                next_zero_torque = now + zero_torque_period;  // 超期后重新对齐，避免追赶风暴
            }
        }

        if (now >= next_control) {
            // 缺陷 D3 修复：控制帧周期**必须真正发送已锁存的槽位值**。
            // 初值全 0 即 Config.md §6.2 的零力矩帧；一旦 ATOM-05 锁存控制量，
            // 本周期发送的就是控制帧，而不会被零力矩帧覆盖。
            sendLatchedFrames();
            next_control += control_period;
            if (next_control <= now) {
                next_control = now + control_period;
            }
        }

        const auto wake_at = next_control < next_zero_torque ? next_control : next_zero_torque;
        const auto sleep_for = wake_at - Clock::now();
        if (sleep_for > std::chrono::steady_clock::duration::zero()) {
            std::this_thread::sleep_for(sleep_for);
        }
    }
}

bool CanCore::latchMotorCurrents(std::uint32_t frame_id, const MotorSlotValues& currents) noexcept
{
    if (!isControlledFrameId(frame_id)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(latch_mutex_);
    latched_[frameIndex(frame_id)] = currents;
    return true;
}

bool CanCore::latchedCurrents(std::uint32_t frame_id, MotorSlotValues& out) const noexcept
{
    if (!isControlledFrameId(frame_id)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(latch_mutex_);
    out = latched_[frameIndex(frame_id)];
    return true;
}

void CanCore::forceZeroCurrents() noexcept
{
    std::lock_guard<std::mutex> lock(latch_mutex_);
    for (auto& frame_slots : latched_) {
        frame_slots.fill(0);
    }
}

CanError CanCore::flushLatchedFrames() noexcept
{
    sendLatchedFrames();
    return CanError::OK;
}

CanError CanCore::sendZeroTorque() noexcept
{
    CanError first_error = CanError::OK;

    for (const std::uint32_t frame_id : kControlledFrameIds) {
        CanFrame frame{};
        const CanError built = buildZeroTorqueFrame(frame_id, frame);
        if (built != CanError::OK) {
            if (first_error == CanError::OK) {
                first_error = built;
            }
            continue;
        }

        const CanIoResult sent = dependencies_.transport.send(frame);
        if (!sent) {
            if (first_error == CanError::OK) {
                first_error = sent.error;
            }
            // Design/Config.md §6.3 + §14.2 F-CAN-002：零力矩发送失败必须上报并触发全机静默。
            // 同时强制锁存归零，避免后续周期再发出可能失效的控制量。
            forceZeroCurrents();
            notifyZeroTorqueFailure(sent);

            CanEvent event{};
            event.kind = CanEventKind::ZERO_TORQUE_FAILED;
            event.error = sent.error;
            event.system_errno = sent.system_errno;
            event.timestamp_us = nowMicroseconds();
            publishEvent(event);
        }
    }

    return first_error;
}

marshland::can::CanBusStatus CanCore::busStatus() const noexcept
{
    return snapshotBusStatus();
}

} // namespace marshland::atom04
