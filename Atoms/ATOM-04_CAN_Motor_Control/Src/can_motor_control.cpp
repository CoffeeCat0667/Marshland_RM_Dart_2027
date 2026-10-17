// ============================================================================
// can_motor_control.cpp — ATOM-04 闭环控制实现
//
// 结构：open/bind 一次，此后全部收发都在**唯一的接收线程**内完成。
//   * poll(timeout) 使线程既能及时收取反馈帧，又能每 20ms 醒来执行一次 PID；
//   * 反馈帧用 MSG_DONTWAIT 一次排空，绝不让收帧阻塞驱动周期；
//   * PID 与组帧发送都在该线程内，因此 PID 状态无需加锁；
//   * 只有"反馈状态表"需要跨线程，用一把互斥量保护。
//
// 安全网：无（用户裁决）。stop() 不发送任何帧；发送失败被静默忽略。
// ============================================================================
#include "atom04_can/can_motor_control.hpp"

#include <chrono>
#include <cmath>
#include <cstring>

#if defined(__linux__)
#  include <net/if.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <cerrno>
#  include <system_error>
#  include <linux/can.h>
#  include <linux/can/raw.h>
#endif

namespace marshland::atom04 {

namespace {

using can::CanError;
using can::CanFrame;
using can::CanResult;

CanResult failure(CanError error, int error_number) noexcept
{
    CanResult r{};
    r.error = error;
    r.system_errno = error_number;
    r.failed = (error != CanError::OK);
    return r;
}

CanResult success() noexcept
{
    return CanResult{};
}

#if defined(__linux__)

// Linux errno -> 模块错误。接口不存在/不可达**不等于**控制器 BUS_OFF。
constexpr CanError classifyErrno(int error_number) noexcept
{
    switch (error_number) {
    case 0:
        return CanError::OK;
    case ENETDOWN:
    case ENETUNREACH:
    case ENODEV:
    case ECONNREFUSED:
        return CanError::BUS_ERROR;
    case EIO:
    case EPROTO:
    case EBADMSG:
        return CanError::BUS_ERROR;
    case EAGAIN:  // Linux 上 EWOULDBLOCK == EAGAIN，不可重复列 case
    case ENOBUFS:
    case EINTR:
        return CanError::RECEIVE_TIMEOUT;
    case EINVAL:
    case EMSGSIZE:
        return CanError::PARAM_INVALID;
    default:
        return CanError::BUS_ERROR;
    }
}

// PID 输出的电流指令 -> 控制帧槽位值（int16），超范围时饱和而非回绕。
constexpr std::int16_t kSlotMaxValue = 32767;
constexpr std::int16_t kSlotMinValue = -32768;

std::int16_t toSlotValue(double output) noexcept
{
    if (output >= static_cast<double>(kSlotMaxValue)) {
        return kSlotMaxValue;
    }
    if (output <= static_cast<double>(kSlotMinValue)) {
        return kSlotMinValue;
    }
    return static_cast<std::int16_t>(output);
}

#endif

} // namespace

CanMotorControl::~CanMotorControl()
{
    stop();
}

std::uint64_t CanMotorControl::nowMicroseconds() noexcept
{
#if defined(__linux__)
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
#else
    return 0U;
#endif
}

std::int64_t CanMotorControl::nodeIndex(std::uint32_t node_id) noexcept
{
    if (node_id < can::kMotorNodeMin) {
        return -1;
    }
    const std::uint32_t offset = node_id - can::kMotorNodeMin;
    if (offset >= can::kMaxMotorNodes) {
        return -1;
    }
    return static_cast<std::int64_t>(offset);
}

// ---------------------------------------------------------------------------
// 入口 1：启动接收线程
// ---------------------------------------------------------------------------
can::CanResult CanMotorControl::start(
    const char* interface_name,
    const motor::MotorPidParams (&gains)[can::kMaxMotorNodes]) noexcept
{
#if !defined(__linux__)
    (void)interface_name;
    (void)gains;
    return failure(CanError::UNSUPPORTED, 0);
#else
    if (interface_name == nullptr || interface_name[0] == '\0') {
        return failure(CanError::PARAM_INVALID, EINVAL);
    }
    if (running_.load(std::memory_order_acquire)) {
        return success();  // 幂等
    }

    // 参数先行校验：限幅非法会让 PID 恒输出 0（静默失效），宁可明确拒绝。
    for (std::size_t i = 0; i < can::kMaxMotorNodes; ++i) {
        const motor::MotorPidParams& params = gains[i];
        if (!(params.max > params.min)) {
            return failure(CanError::PARAM_INVALID, EINVAL);
        }
        if (!std::isfinite(params.kp) || !std::isfinite(params.ki) ||
            !std::isfinite(params.kd) || !std::isfinite(params.max) ||
            !std::isfinite(params.min)) {
            return failure(CanError::PARAM_INVALID, EINVAL);
        }
    }

    const CanResult opened = openSocket(interface_name);
    if (opened.failed) {
        return opened;
    }

    for (std::size_t i = 0; i < can::kMaxMotorNodes; ++i) {
        Pid::Gains pid_gains{};
        pid_gains.kp = gains[i].kp;
        pid_gains.ki = gains[i].ki;
        pid_gains.kd = gains[i].kd;
        pid_gains.max = gains[i].max;
        pid_gains.min = gains[i].min;
        pid_[i].configure(pid_gains, static_cast<double>(motor::kDrivePeriodMs) / 1000.0);
        target_rpm_[i].store(0, std::memory_order_relaxed);
        enabled_[i].store(false, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (std::size_t i = 0; i < can::kMaxMotorNodes; ++i) {
            states_[i] = motor::MotorState{};
            states_[i].node_id = can::kMotorNodeMin + static_cast<std::uint32_t>(i);
        }
    }

    running_.store(true, std::memory_order_release);
    try {
        receive_thread_ = std::thread(&CanMotorControl::receiveLoop, this);
    } catch (const std::system_error&) {
        running_.store(false, std::memory_order_release);
        closeSocket();
        return failure(CanError::UNSUPPORTED, 0);
    }

    return success();
#endif
}

// ---------------------------------------------------------------------------
// 入口 2：设置目标转速
// ---------------------------------------------------------------------------
can::CanResult CanMotorControl::setRpm(std::uint32_t node_id, std::int16_t rpm) noexcept
{
    const std::int64_t index = nodeIndex(node_id);
    if (index < 0) {
        return failure(CanError::PARAM_INVALID, EINVAL);
    }
    if (rpm < motor::kMinTargetRpm || rpm > motor::kMaxTargetRpm) {
        return failure(CanError::PARAM_INVALID, EINVAL);
    }
    if (!running_.load(std::memory_order_acquire)) {
        return failure(CanError::NOT_OPEN, 0);
    }

    const std::size_t slot = static_cast<std::size_t>(index);
    target_rpm_[slot].store(rpm, std::memory_order_relaxed);
    enabled_[slot].store(true, std::memory_order_release);
    return success();
}

// ---------------------------------------------------------------------------
// 读数入口
// ---------------------------------------------------------------------------
can::CanResult CanMotorControl::state(std::uint32_t node_id,
                                      motor::MotorState& out) const noexcept
{
    const std::int64_t index = nodeIndex(node_id);
    if (index < 0) {
        return failure(CanError::PARAM_INVALID, EINVAL);
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    out = states_[static_cast<std::size_t>(index)];
    // 身份来自查询参数：未 start()（或已 stop()）时也返回一致的 node_id，
    // 不因生命周期阶段而变成 0。
    out.node_id = node_id;
    return success();
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
void CanMotorControl::stop() noexcept
{
    running_.store(false, std::memory_order_release);

    if (receive_thread_.joinable()) {
        // 接收线程最多在一个驱动周期（20ms）内观察到 running_==false 并退出。
        receive_thread_.join();
    }

    closeSocket();  // 线程已退出，此后不存在对该描述符的并发访问

    std::lock_guard<std::mutex> lock(state_mutex_);
    for (std::size_t i = 0; i < can::kMaxMotorNodes; ++i) {
        states_[i] = motor::MotorState{};
        states_[i].node_id = can::kMotorNodeMin + static_cast<std::uint32_t>(i);
    }
    for (std::size_t i = 0; i < can::kMaxMotorNodes; ++i) {
        // 清空意图：重新 start() 后不会沿用上一次的目标转速。
        target_rpm_[i].store(0, std::memory_order_relaxed);
        enabled_[i].store(false, std::memory_order_relaxed);
        pid_[i].reset();
    }
}

// ---------------------------------------------------------------------------
// SocketCAN 打开 / 关闭
// ---------------------------------------------------------------------------
can::CanResult CanMotorControl::openSocket(const char* interface_name) noexcept
{
#if !defined(__linux__)
    (void)interface_name;
    return failure(CanError::UNSUPPORTED, 0);
#else
    const int fd = ::socket(AF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        const int err = errno;
        return failure(classifyErrno(err), err);
    }

    {
        const int disabled = 0;
        // 经典 CAN（非 FD）。
        (void)::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &disabled, sizeof(disabled));
        // 不回显自身发送的帧：0x200/0x1FF 本就不会被当作反馈，但可省去无谓唤醒。
        (void)::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &disabled, sizeof(disabled));
        // 不订阅错误帧：本层没有上报通道。无论内核默认投递与否，
        // drainFrames() 都会用 CAN_ERR_FLAG 二次过滤。
    }

    {
        // 固定发送超时，避免一次发送失败拖垮 20ms 驱动周期。
        struct timeval send_timeout;
        send_timeout.tv_sec = 0;
        send_timeout.tv_usec = 200000;  // 200 ms
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    const std::size_t name_length = std::strlen(interface_name);
    if (name_length >= sizeof(ifr.ifr_name)) {
        (void)::close(fd);
        return failure(CanError::PARAM_INVALID, EINVAL);
    }
    std::memcpy(ifr.ifr_name, interface_name, name_length + 1U);

    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        const int err = errno;
        (void)::close(fd);
        return failure(classifyErrno(err), err);
    }

    struct sockaddr_can address;
    std::memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = ifr.ifr_ifindex;

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        const int err = errno;
        (void)::close(fd);
        return failure(classifyErrno(err), err);
    }

    socket_fd_.store(fd, std::memory_order_release);
    return success();
#endif
}

void CanMotorControl::closeSocket() noexcept
{
#if defined(__linux__)
    const int fd = socket_fd_.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
        (void)::close(fd);
    }
#endif
}

// ---------------------------------------------------------------------------
// 接收线程主循环：收帧 + 20ms 驱动
// ---------------------------------------------------------------------------
void CanMotorControl::receiveLoop() noexcept
{
#if defined(__linux__)
    const int fd = socket_fd_.load(std::memory_order_acquire);
    if (fd < 0) {
        running_.store(false, std::memory_order_release);
        return;
    }

    const auto period = std::chrono::milliseconds(motor::kDrivePeriodMs);
    auto next_tick = std::chrono::steady_clock::now() + period;

    while (running_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        int timeout_ms = 0;
        if (next_tick > now) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(next_tick - now);
            timeout_ms = static_cast<int>(remaining.count());
        }

        struct pollfd descriptor;
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;

        const int ready = ::poll(&descriptor, 1U, timeout_ms);
        if (ready < 0 && errno != EINTR) {
            break;  // 描述符异常：退出接收线程
        }
        if (ready > 0 && (descriptor.revents & POLLIN) != 0) {
            drainFrames();
        }

        const auto after = std::chrono::steady_clock::now();
        if (after >= next_tick) {
            driveOnce();
            next_tick += period;
            if (next_tick <= after) {
                next_tick = after + period;  // 超期后重新对齐，避免追赶风暴
            }
        }
    }

    running_.store(false, std::memory_order_release);
#endif
}

void CanMotorControl::drainFrames() noexcept
{
#if defined(__linux__)
    const int fd = socket_fd_.load(std::memory_order_acquire);
    if (fd < 0) {
        return;
    }

    for (;;) {
        struct can_frame raw;
        std::memset(&raw, 0, sizeof(raw));
        const ssize_t received = ::recv(fd, &raw, sizeof(raw), MSG_DONTWAIT);
        if (received < 0) {
            return;  // EAGAIN：本轮已无更多帧；其余错误同样结束本轮
        }
        if (static_cast<std::size_t>(received) < sizeof(struct can_frame)) {
            continue;  // 部分读：帧结构不完整
        }
        if ((raw.can_id & CAN_ERR_FLAG) != 0U) {
            continue;  // 错误帧不参与反馈
        }

        can::CanFrame frame{};
        frame.can_id = static_cast<std::uint32_t>(raw.can_id) & CAN_SFF_MASK;
        const std::uint8_t length =
            (raw.can_dlc > 8U) ? static_cast<std::uint8_t>(8U) : raw.can_dlc;
        frame.dlc = length;
        for (std::size_t i = 0; i < can::kCanPayloadBytes; ++i) {
            frame.payload[i] = raw.data[i];
        }

        const std::int64_t index = nodeIndex(frame.can_id);
        if (index < 0) {
            continue;  // 非 0x201~0x206：本层不处理
        }

        FeedbackPayload decoded{};
        if (decodeFeedbackPayload(frame, decoded).failed) {
            continue;  // DLC 不符：不更新状态，也不参与闭环
        }

        const std::uint64_t timestamp = nowMicroseconds();
        std::lock_guard<std::mutex> lock(state_mutex_);
        motor::MotorState& entry = states_[static_cast<std::size_t>(index)];
        entry.node_id = frame.can_id;
        entry.online = true;
        entry.rpm = decoded.rpm;
        entry.rotor_position = decoded.rotor_position;
        entry.current = decoded.current;
        entry.temperature = decoded.temperature;
        entry.timestamp_us = timestamp;
    }
#endif
}

void CanMotorControl::driveOnce() noexcept
{
#if defined(__linux__)
    std::array<can::MotorSlotValues, 2U> slots{};

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (std::size_t i = 0; i < can::kMaxMotorNodes; ++i) {
            if (!enabled_[i].load(std::memory_order_acquire)) {
                // 调用方从未对这台电机下达过目标：保持 0 输出，不擅自"调节到 0 转速"。
                // 否则上电后 PID 会为一个自由旋转的电机灌入大电流试图把它拉住。
                continue;
            }

            const double measured = static_cast<double>(states_[i].rpm);
            const double setpoint =
                static_cast<double>(target_rpm_[i].load(std::memory_order_relaxed));
            const double output = pid_[i].update(setpoint, measured);

            const can::MotorSlotQuery query =
                can::queryMotorSlot(can::kMotorNodeMin + static_cast<std::uint32_t>(i));
            if (!query.valid) {
                continue;
            }
            const std::size_t frame_index =
                (query.frame_id == can::kMotorControlFrameIdA) ? 0U : 1U;
            slots[frame_index][query.slot] = toSlotValue(output);
        }
    }

    // 组帧与发送不持有互斥量：I/O 不应阻塞 state() 的读者。
    for (std::size_t f = 0; f < slots.size(); ++f) {
        const std::uint32_t frame_id =
            (f == 0U) ? can::kMotorControlFrameIdA : can::kMotorControlFrameIdB;
        can::CanFrame frame{};
        if (buildControlFrame(frame_id, slots[f], frame).failed) {
            continue;
        }
        // 发送失败被忽略：本模块已无事件通道，且用户明确选择不做安全网。
        (void)sendFrame(frame);
    }
#endif
}

can::CanResult CanMotorControl::sendFrame(const can::CanFrame& frame) noexcept
{
#if !defined(__linux__)
    (void)frame;
    return failure(CanError::UNSUPPORTED, 0);
#else
    const int fd = socket_fd_.load(std::memory_order_acquire);
    if (fd < 0) {
        return failure(CanError::NOT_OPEN, 0);
    }
    if (frame.dlc > 8U) {
        return failure(CanError::PARAM_INVALID, EINVAL);
    }

    struct can_frame raw;
    std::memset(&raw, 0, sizeof(raw));
    raw.can_id = frame.can_id & CAN_SFF_MASK;
    raw.can_dlc = frame.dlc;
    for (std::size_t i = 0; i < can::kCanPayloadBytes; ++i) {
        raw.data[i] = frame.payload[i];
    }

    const ssize_t written = ::send(fd, &raw, sizeof(raw), 0);
    if (written < 0) {
        const int err = errno;
        const CanError mapped = (err == EAGAIN || err == EWOULDBLOCK || err == ENOBUFS)
                                    ? CanError::SEND_FAILED
                                    : classifyErrno(err);
        return failure(mapped, err);  // 失败绝不允许被当作成功
    }
    if (static_cast<std::size_t>(written) != sizeof(struct can_frame)) {
        return failure(CanError::SEND_INCOMPLETE, 0);  // 内核未确认完整写入
    }

    return success();
#endif
}

} // namespace marshland::atom04
