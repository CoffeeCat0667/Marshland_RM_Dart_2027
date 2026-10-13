#include "atom03_SBUS/linux_sbus_byte_stream.hpp"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <climits>
#include <cstddef>

namespace marshland::atom03 {
namespace {

#ifndef BOTHER
#define BOTHER 0010000
#endif

struct LinuxTermios2 final {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[19];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

static_assert(sizeof(LinuxTermios2) == 44U, "unexpected termios2 size");
static_assert(offsetof(LinuxTermios2, c_cc) == 17U, "unexpected termios2 c_cc offset");
static_assert(offsetof(LinuxTermios2, c_ispeed) == 36U,
              "unexpected termios2 c_ispeed offset");
static_assert(offsetof(LinuxTermios2, c_ospeed) == 40U,
              "unexpected termios2 c_ospeed offset");

#ifdef TCGETS2
#undef TCGETS2
#endif
#define TCGETS2 _IOR('T', 0x2A, struct LinuxTermios2)

#ifdef TCSETS2
#undef TCSETS2
#endif
#define TCSETS2 _IOW('T', 0x2B, struct LinuxTermios2)

constexpr speed_t kSbusBaudrate = 100000U;

} // namespace

LinuxSbusByteStream::LinuxSbusByteStream() noexcept
{
    if (::pipe(cancel_pipe_) != 0) {
        cancel_pipe_[0] = -1;
        cancel_pipe_[1] = -1;
        return;
    }

    const int read_flags = ::fcntl(cancel_pipe_[0], F_GETFL, 0);
    const int write_flags = ::fcntl(cancel_pipe_[1], F_GETFL, 0);
    const int read_descriptor_flags = ::fcntl(cancel_pipe_[0], F_GETFD, 0);
    const int write_descriptor_flags = ::fcntl(cancel_pipe_[1], F_GETFD, 0);
    if (read_flags < 0 || write_flags < 0 || read_descriptor_flags < 0 ||
        write_descriptor_flags < 0 ||
        ::fcntl(cancel_pipe_[0], F_SETFL, read_flags | O_NONBLOCK) < 0 ||
        ::fcntl(cancel_pipe_[1], F_SETFL, write_flags | O_NONBLOCK) < 0 ||
        ::fcntl(cancel_pipe_[0], F_SETFD, read_descriptor_flags | FD_CLOEXEC) < 0 ||
        ::fcntl(cancel_pipe_[1], F_SETFD, write_descriptor_flags | FD_CLOEXEC) < 0) {
        ::close(cancel_pipe_[0]);
        ::close(cancel_pipe_[1]);
        cancel_pipe_[0] = -1;
        cancel_pipe_[1] = -1;
    }
}

LinuxSbusByteStream::~LinuxSbusByteStream() noexcept
{
    closeDevice();
    if (cancel_pipe_[0] >= 0) {
        ::close(cancel_pipe_[0]);
    }
    if (cancel_pipe_[1] >= 0) {
        ::close(cancel_pipe_[1]);
    }
}

bool LinuxSbusByteStream::configure(
    const marshland::sbus::SbusSerialParameters& parameters) noexcept
{
    if (parameters.serial_device.empty() || cancel_pipe_[0] < 0 ||
        cancel_pipe_[1] < 0 || parameters.read_timeout_ms >
            static_cast<std::uint32_t>(INT_MAX)) {
        return false;
    }

    const int new_fd = ::open(parameters.serial_device.c_str(),
                               O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (new_fd < 0) {
        return false;
    }

    LinuxTermios2 settings{};
    if (::ioctl(new_fd, TCGETS2, &settings) != 0) {
        ::close(new_fd);
        return false;
    }

    settings.c_cflag &= ~(CBAUD | CSIZE | CSTOPB | PARENB | PARODD | CRTSCTS);
    settings.c_cflag |= BOTHER | CS8 | CSTOPB | PARENB | CREAD | CLOCAL;
    settings.c_ispeed = kSbusBaudrate;
    settings.c_ospeed = kSbusBaudrate;

    settings.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR |
                          ICRNL | IXON | IXOFF | IXANY | INPCK | IGNPAR);
    settings.c_oflag &= ~OPOST;
    settings.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 1;

    if (::ioctl(new_fd, TCSETS2, &settings) != 0) {
        ::close(new_fd);
        return false;
    }
    ::tcflush(new_fd, TCIOFLUSH);

    closeDevice();
    device_fd_ = new_fd;
    read_timeout_ms_ = static_cast<int>(parameters.read_timeout_ms);
    drainCancelPipe();
    return true;
}

marshland::sbus::UartReadStatus LinuxSbusByteStream::read(
    std::uint8_t* destination, std::size_t capacity,
    std::size_t& bytes_read) noexcept
{
    bytes_read = 0;
    if (destination == nullptr || capacity == 0U || device_fd_ < 0 ||
        cancel_pipe_[0] < 0) {
        return marshland::sbus::UartReadStatus::DEVICE_ERROR;
    }

    struct pollfd descriptors[2]{};
    descriptors[0].fd = device_fd_;
    descriptors[0].events = POLLIN;
    descriptors[1].fd = cancel_pipe_[0];
    descriptors[1].events = POLLIN;

    const int result = ::poll(descriptors, 2, read_timeout_ms_);
    if (result < 0) {
        return errno == EINTR ? marshland::sbus::UartReadStatus::TEMPORARY_ERROR
                              : marshland::sbus::UartReadStatus::DEVICE_ERROR;
    }
    if (result == 0) {
        return marshland::sbus::UartReadStatus::TIMEOUT;
    }
    if ((descriptors[1].revents & POLLIN) != 0) {
        drainCancelPipe();
        return marshland::sbus::UartReadStatus::CANCELLED;
    }
    if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return marshland::sbus::UartReadStatus::DEVICE_ERROR;
    }
    if ((descriptors[0].revents & POLLIN) == 0) {
        return marshland::sbus::UartReadStatus::TIMEOUT;
    }

    const ssize_t result_bytes = ::read(device_fd_, destination, capacity);
    if (result_bytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return errno == EINTR ? marshland::sbus::UartReadStatus::TEMPORARY_ERROR
                                  : marshland::sbus::UartReadStatus::TEMPORARY_ERROR;
        }
        return marshland::sbus::UartReadStatus::DEVICE_ERROR;
    }
    bytes_read = static_cast<std::size_t>(result_bytes);
    return bytes_read == 0U ? marshland::sbus::UartReadStatus::DEVICE_ERROR
                            : marshland::sbus::UartReadStatus::DATA;
}

void LinuxSbusByteStream::cancel() noexcept
{
    if (cancel_pipe_[1] < 0) {
        return;
    }
    const std::uint8_t signal = 1U;
    (void)::write(cancel_pipe_[1], &signal, sizeof(signal));
}

void LinuxSbusByteStream::closeDevice() noexcept
{
    if (device_fd_ >= 0) {
        ::close(device_fd_);
        device_fd_ = -1;
    }
}

void LinuxSbusByteStream::drainCancelPipe() noexcept
{
    if (cancel_pipe_[0] < 0) {
        return;
    }
    std::uint8_t buffer[32]{};
    while (::read(cancel_pipe_[0], buffer, sizeof(buffer)) > 0) {
    }
}

} // namespace marshland::atom03

