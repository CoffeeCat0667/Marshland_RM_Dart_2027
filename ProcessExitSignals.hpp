#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <pthread.h>

namespace marshland::app {

class ProcessExitSignals final {
public:
    ProcessExitSignals() noexcept = default;
    ProcessExitSignals(const ProcessExitSignals&) = delete;
    ProcessExitSignals& operator=(const ProcessExitSignals&) = delete;

    ~ProcessExitSignals() noexcept
    {
        if (prepared_) {
            pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
        }
    }

    int Prepare() noexcept
    {
        if (prepared_) {
            return 0;
        }
        if (sigemptyset(&exit_signals_) != 0 ||
            sigaddset(&exit_signals_, SIGINT) != 0 ||
            sigaddset(&exit_signals_, SIGTERM) != 0) {
            return errno;
        }
        const int result = pthread_sigmask(SIG_BLOCK, &exit_signals_, &previous_mask_);
        prepared_ = (result == 0);
        return result;
    }

    int Wait(int& signal) const noexcept
    {
        if (!prepared_) {
            return EINVAL;
        }
        return sigwait(&exit_signals_, &signal);
    }

    // EAGAIN means the deadline elapsed without an exit signal.
    int WaitFor(int& signal, std::chrono::nanoseconds timeout) const noexcept
    {
        if (!prepared_ || timeout.count() < 0) {
            return EINVAL;
        }
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
        const timespec duration{
            static_cast<time_t>(seconds.count()),
            static_cast<long>((timeout - seconds).count())};
        const int received = sigtimedwait(&exit_signals_, nullptr, &duration);
        if (received < 0) {
            return errno;
        }
        signal = received;
        return 0;
    }

private:
    sigset_t exit_signals_{};
    sigset_t previous_mask_{};
    bool prepared_{false};
};

} // namespace marshland::app
