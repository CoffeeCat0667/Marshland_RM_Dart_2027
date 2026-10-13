#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "atom03_SBUS/sbus_config.hpp"
#include "SbusTypes.hpp"
#include "SbusInterfaces.hpp"

namespace {

using namespace marshland;
using namespace marshland::atom03;
using namespace marshland::sbus;

void Check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class MockUartByteStream final : public IUartByteStream {
public:
    bool configure(const SbusSerialParameters& parameters) noexcept override
    {
        configured = true;
        device_path = parameters.serial_device;
        return configure_succeeds;
    }

    UartReadStatus read(std::uint8_t* destination, std::size_t capacity,
                       std::size_t& bytes_read) noexcept override
    {
        if (!configured || !destination || capacity == 0) {
            bytes_read = 0;
            return UartReadStatus::DEVICE_ERROR;
        }
        if (read_data.empty()) {
            bytes_read = 0;
            return UartReadStatus::TIMEOUT;
        }
        bytes_read = std::min(capacity, read_data.size());
        std::memcpy(destination, read_data.data(), bytes_read);
        read_data.erase(read_data.begin(), read_data.begin() + bytes_read);
        return UartReadStatus::DATA;
    }

    void cancel() noexcept override
    {
        cancelled = true;
    }

    bool configured = false;
    bool configure_succeeds = true;
    bool cancelled = false;
    std::string device_path;
    std::vector<std::uint8_t> read_data;
};

} // namespace

namespace marshland::tests {

void TestSbusConfigDefaults()
{
    SbusConfig config;
    Check(config.channel_scale_x == 1, "default channel_scale_x");
    Check(config.receive_period_ms == 20, "default receive_period_ms");
    Check(config.loss_timeout_ms == 200, "default loss_timeout_ms");
}

void TestSbusChannelSnapshotStructure()
{
    SbusChannelSnapshot snapshot{};
    Check(snapshot.raw_channels.size() == kChannelCount, "raw_channels size");
    Check(snapshot.scaled_channels.size() == kChannelCount, "scaled_channels size");
    Check(snapshot.control_source == ControlSource::NONE, "default control_source");
    Check(snapshot.estop_state == EstopState::CENTER, "default estop_state");
    Check(snapshot.frame_validity == FrameValidity::INVALID, "default frame_validity");
    Check(!snapshot.online, "default online false");
    Check(!snapshot.fire_request_confirmed, "default fire_request_confirmed false");
}

void TestSbusChannelStoreReadWrite()
{
    SbusChannelStore store;

    SbusChannelSnapshot write_snapshot{};
    write_snapshot.last_valid_frame_time_us = 123456;
    write_snapshot.frame_sequence = 42;
    write_snapshot.raw_channels[0] = 1024;
    write_snapshot.raw_channels[1] = 512;
    write_snapshot.frame_validity = FrameValidity::VALID;
    write_snapshot.online = true;

    store.replace(write_snapshot);

    const auto read_snapshot = store.snapshot();
    Check(read_snapshot.last_valid_frame_time_us == 123456, "timestamp preserved");
    Check(read_snapshot.frame_sequence == 42, "frame_sequence preserved");
    Check(read_snapshot.raw_channels[0] == 1024, "channel 0 preserved");
    Check(read_snapshot.raw_channels[1] == 512, "channel 1 preserved");
    Check(read_snapshot.frame_validity == FrameValidity::VALID, "frame_validity preserved");
    Check(read_snapshot.online == true, "online preserved");
}

void TestSbusChannelStoreConcurrency()
{
    SbusChannelStore store;
    std::atomic<bool> stop{false};
    std::atomic<int> write_count{0};
    std::atomic<int> read_count{0};

    // Writer thread
    std::thread writer([&]() {
        while (!stop.load()) {
            SbusChannelSnapshot snapshot{};
            snapshot.last_valid_frame_time_us = write_count.load();
            snapshot.frame_sequence = write_count.load();
            store.replace(snapshot);
            ++write_count;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    // Reader thread
    std::thread reader([&]() {
        while (!stop.load()) {
            const auto snapshot = store.snapshot();
            (void)snapshot;  // Use snapshot to avoid warning
            ++read_count;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop = true;

    writer.join();
    reader.join();

    Check(write_count.load() > 0, "writes occurred");
    Check(read_count.load() > 0, "reads occurred");
}

void TestMockUartConfigure()
{
    MockUartByteStream uart;

    SbusSerialParameters params;
    params.serial_device = "/dev/ttyS10";
    params.read_timeout_ms = 100;

    const bool configured = uart.configure(params);
    Check(configured, "configure succeeds");
    Check(uart.configured, "uart marked configured");
    Check(uart.device_path == "/dev/ttyS10", "device path stored");
}

void TestMockUartReadData()
{
    MockUartByteStream uart;

    SbusSerialParameters params;
    params.serial_device = "/dev/ttyS10";
    uart.configure(params);

    // Feed some data
    uart.read_data = {0x01, 0x02, 0x03, 0x04};

    std::uint8_t buffer[10];
    std::size_t bytes_read = 0;
    const auto status = uart.read(buffer, sizeof(buffer), bytes_read);

    Check(status == UartReadStatus::DATA, "read returns DATA");
    Check(bytes_read == 4, "bytes_read correct");
    Check(buffer[0] == 0x01 && buffer[1] == 0x02, "data correct");
}

void TestMockUartReadTimeout()
{
    MockUartByteStream uart;

    SbusSerialParameters params;
    params.serial_device = "/dev/ttyS10";
    uart.configure(params);

    // No data available
    std::uint8_t buffer[10];
    std::size_t bytes_read = 0;
    const auto status = uart.read(buffer, sizeof(buffer), bytes_read);

    Check(status == UartReadStatus::TIMEOUT, "read returns TIMEOUT");
    Check(bytes_read == 0, "no bytes read");
}

void TestMockUartCancel()
{
    MockUartByteStream uart;
    Check(!uart.cancelled, "not cancelled initially");

    uart.cancel();
    Check(uart.cancelled, "cancelled after cancel()");
}

void TestSbusSerialParameters()
{
    SbusSerialParameters params;
    params.serial_device = "/dev/ttyS10";
    params.read_timeout_ms = 100;

    Check(params.serial_device == "/dev/ttyS10", "serial_device set");
    Check(params.read_timeout_ms == 100, "read_timeout_ms set");
}

void TestSbusEstopStateEnum()
{
    Check(EstopState::CENTER != EstopState::NORMAL, "CENTER != NORMAL");
    Check(EstopState::NORMAL != EstopState::ESTOP, "NORMAL != ESTOP");
    Check(EstopState::ESTOP != EstopState::CENTER, "ESTOP != CENTER");
}

void TestSbusFrameValidityEnum()
{
    Check(FrameValidity::INVALID != FrameValidity::VALID, "INVALID != VALID");

    SbusChannelSnapshot snapshot{};
    snapshot.frame_validity = FrameValidity::VALID;
    Check(snapshot.frame_validity == FrameValidity::VALID, "frame_validity assignment");
}

void TestSbusControlSourceMapping()
{
    SbusChannelSnapshot snapshot{};
    snapshot.control_source = ControlSource::SBUS;
    Check(snapshot.control_source == ControlSource::SBUS, "control_source SBUS");

    snapshot.control_source = ControlSource::WEB;
    Check(snapshot.control_source == ControlSource::WEB, "control_source WEB");

    snapshot.control_source = ControlSource::AUTO;
    Check(snapshot.control_source == ControlSource::AUTO, "control_source AUTO");
}

} // namespace marshland::tests

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <test-case>\n", argv[0]);
        return 1;
    }

    const std::string_view case_name{argv[1]};

    try {
        using namespace marshland::tests;

        if (case_name == "sbus.config_defaults") {
            TestSbusConfigDefaults();
        } else if (case_name == "sbus.snapshot_structure") {
            TestSbusChannelSnapshotStructure();
        } else if (case_name == "sbus.store_read_write") {
            TestSbusChannelStoreReadWrite();
        } else if (case_name == "sbus.store_concurrency") {
            TestSbusChannelStoreConcurrency();
        } else if (case_name == "sbus.uart_configure") {
            TestMockUartConfigure();
        } else if (case_name == "sbus.uart_read_data") {
            TestMockUartReadData();
        } else if (case_name == "sbus.uart_read_timeout") {
            TestMockUartReadTimeout();
        } else if (case_name == "sbus.uart_cancel") {
            TestMockUartCancel();
        } else if (case_name == "sbus.serial_parameters") {
            TestSbusSerialParameters();
        } else if (case_name == "sbus.estop_state_enum") {
            TestSbusEstopStateEnum();
        } else if (case_name == "sbus.frame_validity_enum") {
            TestSbusFrameValidityEnum();
        } else if (case_name == "sbus.control_source_mapping") {
            TestSbusControlSourceMapping();
        } else {
            std::fprintf(stderr, "unknown test case: %s\n", argv[1]);
            return 1;
        }

        std::printf("[PASS] %s\n", argv[1]);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[FAIL] %s: %s\n", argv[1], e.what());
        return 1;
    }
}
