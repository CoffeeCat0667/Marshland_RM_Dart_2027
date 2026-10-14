#include "atom07_servo_driver/linux_i2c_bus.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <unistd.h>

#include <utility>

namespace marshland::atom07 {

LinuxI2cBus::LinuxI2cBus(std::string device_path,
                         std::uint8_t device_address,
                         unsigned transaction_timeout_ms) noexcept
    : device_path_(std::move(device_path)),
      address_(device_address),
      timeout_ms_(transaction_timeout_ms == 0U ? 1U : transaction_timeout_ms)
{
}

LinuxI2cBus::~LinuxI2cBus() noexcept
{
    close();
}

bool LinuxI2cBus::open() noexcept
{
    if (fd_ >= 0) {
        return true;
    }
    fd_ = ::open(device_path_.c_str(), O_RDWR);
    if (fd_ < 0) {
        last_system_error_ = errno;
        return false;
    }
    if (::ioctl(fd_, I2C_SLAVE, static_cast<long>(address_)) < 0) {
        last_system_error_ = errno;
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    selected_address_ = static_cast<int>(address_);
    last_system_error_ = 0;
    return true;
}

void LinuxI2cBus::close() noexcept
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        selected_address_ = -1;
    }
}

bool LinuxI2cBus::isOpen() const noexcept
{
    return fd_ >= 0;
}

bool LinuxI2cBus::selectAddress(std::uint8_t device_address) noexcept
{
    if (fd_ < 0) {
        last_system_error_ = ENODEV;
        return false;
    }
    if (selected_address_ == static_cast<int>(device_address)) {
        return true;
    }
    if (::ioctl(fd_, I2C_SLAVE, static_cast<long>(device_address)) < 0) {
        last_system_error_ = errno;
        return false;
    }
    selected_address_ = static_cast<int>(device_address);
    return true;
}

bool LinuxI2cBus::waitReady(bool readable) noexcept
{
    if (fd_ < 0) {
        last_system_error_ = ENODEV;
        return false;
    }
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    if (readable) {
        FD_SET(fd_, &read_set);
    } else {
        FD_SET(fd_, &write_set);
    }

    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(timeout_ms_ / 1000U);
    timeout.tv_usec = static_cast<suseconds_t>((timeout_ms_ % 1000U) * 1000U);

    const int ready = ::select(fd_ + 1,
                               readable ? &read_set : nullptr,
                               readable ? nullptr : &write_set,
                               nullptr, &timeout);
    if (ready > 0) {
        return true;
    }
    last_system_error_ = (ready == 0) ? ETIMEDOUT : errno;
    return false;
}

I2cStatus LinuxI2cBus::write(std::uint8_t device_address, std::uint8_t reg,
                             const std::uint8_t* data, std::size_t size) noexcept
{
    if (data == nullptr || size == 0U || size > kMaxPayloadBytes) {
        return {false, EINVAL};
    }
    if (!selectAddress(device_address) || !waitReady(false)) {
        return {false, last_system_error_};
    }

    std::array<std::uint8_t, kMaxPayloadBytes + 1U> buffer{};
    buffer[0] = reg;
    std::memcpy(buffer.data() + 1U, data, size);

    const ssize_t written = ::write(fd_, buffer.data(), size + 1U);
    if (written != static_cast<ssize_t>(size + 1U)) {
        last_system_error_ = (written < 0) ? errno : EIO;
        return {false, last_system_error_};
    }
    last_system_error_ = 0;
    return {true, 0};
}

I2cStatus LinuxI2cBus::read(std::uint8_t device_address, std::uint8_t reg,
                            std::uint8_t* out, std::size_t size) noexcept
{
    if (out == nullptr || size == 0U || size > kMaxPayloadBytes) {
        return {false, EINVAL};
    }
    if (!selectAddress(device_address) || !waitReady(false)) {
        return {false, last_system_error_};
    }
    if (::write(fd_, &reg, 1U) != 1) {
        last_system_error_ = errno;
        return {false, last_system_error_};
    }

    std::size_t received = 0U;
    while (received < size) {
        if (!waitReady(true)) {
            return {false, last_system_error_};
        }
        const ssize_t got = ::read(fd_, out + received, size - received);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            last_system_error_ = errno;
            return {false, last_system_error_};
        }
        if (got == 0) {
            last_system_error_ = EIO;
            return {false, last_system_error_};
        }
        received += static_cast<std::size_t>(got);
    }
    last_system_error_ = 0;
    return {true, 0};
}

I2cStatus LinuxI2cBus::writeRaw(std::uint8_t device_address,
                                const std::uint8_t* data, std::size_t size) noexcept
{
    if (data == nullptr || size == 0U || size > kMaxPayloadBytes) {
        return {false, EINVAL};
    }
    if (!selectAddress(device_address) || !waitReady(false)) {
        return {false, last_system_error_};
    }
    const ssize_t written = ::write(fd_, data, size);
    if (written != static_cast<ssize_t>(size)) {
        last_system_error_ = (written < 0) ? errno : EIO;
        return {false, last_system_error_};
    }
    last_system_error_ = 0;
    return {true, 0};
}

} // namespace marshland::atom07
