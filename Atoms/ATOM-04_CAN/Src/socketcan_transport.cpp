#include "atom04_can/socketcan_transport.hpp"

#include "CanTypes.hpp"

#if defined(__linux__)
#  include <net/if.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  include <cerrno>
#  include <cstring>
#  include <linux/can.h>
#  include <linux/can/error.h>
#  include <linux/can/raw.h>
#endif

namespace marshland::atom04 {

namespace {

#if defined(__linux__)
// Linux errno -> 模块错误。分类必须能区分超时、BUS_OFF 与总线错误。
constexpr marshland::can::CanError classifyErrno(int error_number) noexcept
{
    using marshland::can::CanError;
    switch (error_number) {
    case 0:
        return CanError::OK;
    // 缺陷 #8 修复：接口不存在/不可达**不等于** CAN 控制器进入 BUS_OFF。
    // BUS_OFF 只能由内核错误帧 CAN_ERR_BUSOFF 判定（见 receive）。
    // 这里统一归为 BUS_ERROR，并保留 errno 供诊断定位。
    case ENETDOWN:
    case ENETUNREACH:
    case ENODEV:
    case ECONNREFUSED:
        return CanError::BUS_ERROR;
    // 协议/设备层错误
    case EIO:
    case EPROTO:
    case EBADMSG:
        return CanError::BUS_ERROR;
    // 暂无数据 / 短暂不可写
    case EAGAIN:  // Linux 上 EWOULDBLOCK == EAGAIN，不可重复列 case
    case ENOBUFS:
    case EINTR:
        return CanError::RECEIVE_TIMEOUT;
    // 参数不合法
    case EINVAL:
    case EMSGSIZE:
        return CanError::PARAM_INVALID;
    default:
        return CanError::BUS_ERROR;
    }
}
#endif

marshland::can::CanIoResult failure(marshland::can::CanError error, int error_number) noexcept
{
    marshland::can::CanIoResult result{};
    result.error = error;
    result.system_errno = error_number;
    return result;
}

marshland::can::CanIoResult success() noexcept
{
    marshland::can::CanIoResult result{};
    result.error = marshland::can::CanError::OK;
    result.system_errno = 0;
    return result;
}

} // namespace

marshland::can::CanError LinuxSocketCanTransport::mapErrno(int error_number) noexcept
{
#if defined(__linux__)
    return classifyErrno(error_number);
#else
    (void)error_number;
    return marshland::can::CanError::UNSUPPORTED;
#endif
}

LinuxSocketCanTransport::~LinuxSocketCanTransport()
{
    close();
}

marshland::can::CanIoResult LinuxSocketCanTransport::open(const char* interface_name) noexcept
{
    using marshland::can::CanError;
#if !defined(__linux__)
    (void)interface_name;
    last_error_code_.store(static_cast<int>(CanError::UNSUPPORTED), std::memory_order_release);
    return failure(CanError::UNSUPPORTED, 0);
#else
    if (interface_name == nullptr || interface_name[0] == '\0') {
        last_error_code_.store(static_cast<int>(CanError::PARAM_INVALID), std::memory_order_release);
        return failure(CanError::PARAM_INVALID, EINVAL);
    }

    // 幂等：已打开则视为成功，不重复创建 fd。
    if (socket_fd_.load(std::memory_order_acquire) >= 0) {
        return success();
    }

    const int fd = ::socket(AF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        const int err = errno;
        last_error_code_.store(static_cast<int>(classifyErrno(err)), std::memory_order_release);
        last_errno_.store(err, std::memory_order_release);
        return failure(classifyErrno(err), err);
    }

    // Classic CAN（Design/Config.md §6.1：显式声明 fd off）。
    // CAN_RAW_FD_FRAMES 默认关闭；显式写入 0 以固定该行为，避免被上层改动。
    {
        const int disabled = 0;
        (void)::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &disabled, sizeof(disabled));
        // 接收自身发送的帧：关闭，避免把控制帧回显误判为节点反馈。
        (void)::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &disabled, sizeof(disabled));
        // 缺陷 #8 修复：Design/Config.md §6.1 要求"错误帧处理=丢弃"，但 ATOM-04 的
        // 验收条件又要求能区分 BUS_OFF 与总线错误。SocketCAN 下 BUS_OFF 的**唯一**
        // 可靠观测来源就是内核错误帧 CAN_ERR_BUSOFF，因此必须订阅它：
        //   * 只向内核请求 CAN_ERR_BUSOFF，不请求其他错误帧；
        //   * 收到后提取状态即丢弃（符合 DISCARD），不会把错误帧交给上层业务。
        {
            const can_err_mask_t err_mask = CAN_ERR_BUSOFF;
            (void)::setsockopt(fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &err_mask, sizeof(err_mask));
        }
    }

    // 读写超时：使用 poll 控制接收超时，这里只固定发送超时，避免 TX 线程无限阻塞。
    {
        struct timeval send_timeout;
        send_timeout.tv_sec = 0;
        send_timeout.tv_usec = 200000;  // 200 ms
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
    }

    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    // Design/Config.md §6.1 写 "CAN0"，但 Linux 接口名区分大小写；
    // 本模块固定使用调用方给出的 interface_name（装配层按 can0 传入）。
    const std::size_t name_length = std::strlen(interface_name);
    if (name_length >= sizeof(ifr.ifr_name)) {
        (void)::close(fd);
        last_error_code_.store(static_cast<int>(CanError::PARAM_INVALID), std::memory_order_release);
        return failure(CanError::PARAM_INVALID, EINVAL);
    }
    std::memcpy(ifr.ifr_name, interface_name, name_length + 1U);

    if (::ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        const int err = errno;
        (void)::close(fd);
        last_error_code_.store(static_cast<int>(classifyErrno(err)), std::memory_order_release);
        last_errno_.store(err, std::memory_order_release);
        return failure(classifyErrno(err), err);
    }

    struct sockaddr_can address;
    std::memset(&address, 0, sizeof(address));
    address.can_family = AF_CAN;
    address.can_ifindex = ifr.ifr_ifindex;

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) < 0) {
        const int err = errno;
        (void)::close(fd);
        last_error_code_.store(static_cast<int>(classifyErrno(err)), std::memory_order_release);
        last_errno_.store(err, std::memory_order_release);
        return failure(classifyErrno(err), err);
    }

    socket_fd_.store(fd, std::memory_order_release);
    opened_.store(true, std::memory_order_release);
    last_error_code_.store(static_cast<int>(CanError::OK), std::memory_order_release);
    last_errno_.store(0, std::memory_order_release);
    return success();
#endif
}

void LinuxSocketCanTransport::close() noexcept
{
    const int fd = socket_fd_.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) {
#if defined(__linux__)
        // shutdown 先唤醒可能阻塞在 recv/poll 的线程，再关闭 fd。
        (void)::shutdown(fd, SHUT_RDWR);
        (void)::close(fd);
#endif
    }
    opened_.store(false, std::memory_order_release);
}

marshland::can::CanIoResult LinuxSocketCanTransport::receive(marshland::can::CanFrame& out,
                                                             int timeout_ms) noexcept
{
    using marshland::can::CanError;
#if !defined(__linux__)
    (void)out;
    (void)timeout_ms;
    return failure(CanError::UNSUPPORTED, 0);
#else
    const int fd = socket_fd_.load(std::memory_order_acquire);
    if (fd < 0) {
        return failure(CanError::NOT_OPEN, 0);
    }

    const int bounded_timeout = timeout_ms < 0 ? 0 : timeout_ms;

    struct pollfd descriptor;
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;

    const int ready = ::poll(&descriptor, 1, bounded_timeout);
    if (ready < 0) {
        const int err = errno;
        if (err == EINTR) {
            return failure(CanError::RECEIVE_TIMEOUT, err);
        }
        const CanError mapped = classifyErrno(err);
        last_error_code_.store(static_cast<int>(mapped), std::memory_order_release);
        last_errno_.store(err, std::memory_order_release);
        return failure(mapped, err);
    }
    if (ready == 0) {
        // 无帧可读：必须是超时，绝不能返回成功。
        return failure(CanError::RECEIVE_TIMEOUT, 0);
    }
    if ((descriptor.revents & POLLIN) == 0) {
        // POLLERR / POLLHUP / POLLNVAL：接口被删除或总线进入异常。
        // 缺陷 #8：这里不再猜测 BUS_OFF；BUS_OFF 由 CAN_ERR_BUSOFF 错误帧确定。
        const CanError mapped = ((descriptor.revents & POLLNVAL) != 0) ? CanError::NOT_OPEN
                                                                      : CanError::BUS_ERROR;
        last_error_code_.store(static_cast<int>(mapped), std::memory_order_release);
        return failure(mapped, 0);
    }

    struct can_frame raw;
    std::memset(&raw, 0, sizeof(raw));
    const ssize_t received = ::recv(fd, &raw, sizeof(raw), 0);
    if (received < 0) {
        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK) {
            return failure(CanError::RECEIVE_TIMEOUT, err);
        }
        if (err == EINTR) {
            return failure(CanError::RECEIVE_TIMEOUT, err);
        }
        const CanError mapped = classifyErrno(err);
        last_error_code_.store(static_cast<int>(mapped), std::memory_order_release);
        last_errno_.store(err, std::memory_order_release);
        return failure(mapped, err);
    }
    if (static_cast<std::size_t>(received) < sizeof(struct can_frame)) {
        // 部分读：帧结构不完整，丢弃，不得当作有效帧。
        last_error_code_.store(static_cast<int>(CanError::FRAME_INVALID), std::memory_order_release);
        return failure(CanError::FRAME_INVALID, 0);
    }

    // 缺陷 #8：识别内核错误帧。CAN_ERR_BUSOFF -> BUS_OFF；其余错误帧丢弃。
    if ((raw.can_id & CAN_ERR_FLAG) != 0U) {
        // CAN_ERR_MASK / CAN_ERR_BUSOFF 已是 canid_t（uint32_t），无需再转换。
        const canid_t error_class = raw.can_id & CAN_ERR_MASK;

        if (error_class == CAN_ERR_BUSOFF) {
            last_error_code_.store(static_cast<int>(CanError::BUS_OFF), std::memory_order_release);
            last_errno_.store(0, std::memory_order_release);
            return failure(CanError::BUS_OFF, 0);
        }

        // 其他错误帧（控制器/协议/收发器）：按 DISCARD 语义丢弃内容，仅记录分类。
        last_error_code_.store(static_cast<int>(CanError::BUS_ERROR), std::memory_order_release);
        return failure(CanError::BUS_ERROR, 0);
    }

    marshland::can::CanFrame frame{};
    // 去掉 SocketCAN 标志位，公共 CanFrame 只保存标识符本身。
    frame.can_id = static_cast<std::uint32_t>(raw.can_id) & CAN_SFF_MASK;
    const std::uint8_t length = raw.can_dlc > 8U ? 8U : static_cast<std::uint8_t>(raw.can_dlc);
    frame.dlc = length;
    for (std::size_t i = 0; i < marshland::can::kCanPayloadBytes; ++i) {
        frame.payload[i] = raw.data[i];
    }

    rx_count_.fetch_add(1ULL, std::memory_order_relaxed);
    out = frame;
    return success();
#endif
}

marshland::can::CanIoResult LinuxSocketCanTransport::send(const marshland::can::CanFrame& frame) noexcept
{
    using marshland::can::CanError;
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
    for (std::size_t i = 0; i < marshland::can::kCanPayloadBytes; ++i) {
        raw.data[i] = frame.payload[i];
    }

    const ssize_t written = ::send(fd, &raw, sizeof(raw), 0);
    if (written < 0) {
        const int err = errno;
        const CanError mapped = (err == EAGAIN || err == EWOULDBLOCK || err == ENOBUFS)
                                    ? CanError::SEND_FAILED
                                    : classifyErrno(err);
        // 发送失败绝不允许被当作成功。
        last_error_code_.store(static_cast<int>(mapped), std::memory_order_release);
        last_errno_.store(err, std::memory_order_release);
        tx_fail_count_.fetch_add(1ULL, std::memory_order_relaxed);
        return failure(mapped, err);
    }
    if (static_cast<std::size_t>(written) != sizeof(struct can_frame)) {
        // 内核未确认完整写入：必须报告为失败，交由零力矩安全路径处理。
        last_error_code_.store(static_cast<int>(CanError::SEND_INCOMPLETE), std::memory_order_release);
        tx_fail_count_.fetch_add(1ULL, std::memory_order_relaxed);
        return failure(CanError::SEND_INCOMPLETE, 0);
    }

    tx_count_.fetch_add(1ULL, std::memory_order_relaxed);
    last_error_code_.store(static_cast<int>(CanError::OK), std::memory_order_release);
    return success();
#endif
}

marshland::can::CanBusStatus LinuxSocketCanTransport::busStatus() const noexcept
{
    marshland::can::CanBusStatus status{};
    status.open = opened_.load(std::memory_order_acquire);
    status.last_error = static_cast<marshland::can::CanError>(last_error_code_.load(std::memory_order_acquire));
    status.last_errno = last_errno_.load(std::memory_order_acquire);
    status.rx_count = rx_count_.load(std::memory_order_relaxed);
    status.tx_count = tx_count_.load(std::memory_order_relaxed);
    status.tx_fail_count = tx_fail_count_.load(std::memory_order_relaxed);
    return status;
}

} // namespace marshland::atom04
