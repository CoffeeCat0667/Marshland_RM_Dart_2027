#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "CanInterfaces.hpp"
#include "IMonotonicClock.hpp"
#include "motor_config.hpp"
#include "motor_interfaces.hpp"
#include "pid_controller.hpp"

namespace marshland::atom05 {

class MotorCore final : public can::ICanFrameSink {
public:
    struct Dependencies final {
        const app::IMonotonicClock& clock;
        IMotorSafetyGate& safety;
        IMotorCurrentOutput& output;
        IMotorFaultSink& faults;
        IPidPersistence& persistence;
    };
    explicit MotorCore(const Dependencies& dependencies) noexcept;
    ~MotorCore() noexcept;
    MotorCore(const MotorCore&) = delete;
    MotorCore& operator=(const MotorCore&) = delete;

    MotorStatus configure(const MotorRuntimeConfig& config) noexcept;
    MotorStatus start() noexcept;
    void stop() noexcept;
    bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }
    std::uint64_t configurationVersion() const noexcept;
    void onCanFrame(const can::CanFrame& frame) noexcept override;
    MotorSnapshot snapshot(std::uint8_t motor_id) const noexcept;
    MotorStatus submitCommand(const MotorCommand& command) noexcept;
    MotorStatus enable(std::uint8_t motor_id, ControlSource source) noexcept;
    MotorStatus disable(std::uint8_t motor_id) noexcept;
    MotorStatus stopMotor(std::uint8_t motor_id) noexcept;
    void stopAll() noexcept;
    MotorStatus clearFault(std::uint8_t motor_id) noexcept;
    MotorStatus readPidConfig(std::uint8_t motor_id, PidConfig& output) const noexcept;
    PidUpdateResult updatePid(const PidUpdateRequest& request) noexcept;
    MotorStatus updateOnce(std::uint8_t motor_id) noexcept;
    void tick() noexcept;

private:
    struct Channel final {
        MotorSnapshot state{};
        PidController controller{};
        MotorFault pending_feedback_fault{MotorFault::NONE};
        bool notification_pending{false};
        bool notification_in_flight{false};
        FaultEvent event{};
        bool has_step{false};
        std::uint64_t last_step_us{0};
        std::uint64_t next_due_us{0};
    };
    static constexpr std::size_t kNotFound = kMotorCount;
    std::size_t findLocked(std::uint8_t motor_id) const noexcept;
    std::size_t peerLocked(std::size_t index) const noexcept;
    bool now(std::uint64_t& output) const noexcept;
    MotorStatus feedbackStatusLocked(const Channel& channel, std::uint64_t now_us) const noexcept;
    void revokeLocked(std::size_t index) noexcept;
    void revokePairLocked(std::size_t index) noexcept;
    void faultLocked(std::size_t index, MotorFault fault, std::uint64_t now_us) noexcept;
    bool commitLocked(std::uint64_t now_us) noexcept;
    MotorStatus stepLocked(std::size_t index, std::uint64_t now_us) noexcept;
    void publishFaults() noexcept;
    void run() noexcept;
    void wake() noexcept;

    Dependencies dependencies_;
    mutable std::mutex mutex_;
    std::mutex lifecycle_mutex_;
    std::mutex pid_update_mutex_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    std::array<Channel, kMotorCount> channels_{};
    MotorRuntimeConfig config_{};
    bool configured_{false};
    bool stopped_{true};
    std::uint64_t output_generation_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> wake_requested_{false};
    std::thread worker_;
};

} // namespace marshland::atom05
