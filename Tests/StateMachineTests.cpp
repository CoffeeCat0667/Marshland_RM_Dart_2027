#include "Atoms/ATOM-01_Runtime/Inc/RuntimeCoordinator.hpp"
#include "Atoms/ATOM-02_State_Arbitration/Inc/StateArbitration.hpp"
#include "ProcessExitSignals.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

using marshland::state::ControlSource;
using marshland::state::StateMachine;
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<class Data>
void publish(Data& data, bool online, std::int32_t ch5, std::int32_t ch7 = 0)
{
    std::lock_guard lock(data.mutex);
    data.online = online;
    data.scaled_channels[4] = ch5;
    data.scaled_channels[6] = ch7;
}

void defaultsAndExplicitUpdate()
{
    marshland::sbus::SbusData sbus;
    marshland::web::WebData web;
    publish(sbus, true, 660);
    StateMachine machine{sbus, web};
    require(machine.control_source() == ControlSource::AUTO, "construction must not poll");
    machine.updateControlSource();
    require(machine.control_source() == ControlSource::SBUS, "explicit update must poll");
    publish(sbus, true, -660);
    require(machine.control_source() == ControlSource::SBUS, "input writes must not update source");
    machine.updateControlSource();
    require(machine.control_source() == ControlSource::AUTO, "next poll must see new CH5");

    require(marshland::state::getGlobalStateMachine() == nullptr, "accessor starts empty");
    marshland::state::setGlobalStateMachine(&machine);
    require(marshland::state::getGlobalStateMachine() == &machine, "accessor registration");
    marshland::state::setGlobalStateMachine(nullptr);
    require(marshland::state::getGlobalStateMachine() == nullptr, "accessor teardown");
    require(std::string_view(marshland::state::toString(ControlSource::WEB)) == "WEB",
            "control source string");
}

void sourceMatrix()
{
    marshland::sbus::SbusData sbus;
    marshland::web::WebData web;
    StateMachine machine{sbus, web};
    constexpr std::array<std::int32_t, 3> values{-660, 0, 660};
    constexpr std::array<ControlSource, 3> sbus_sources{
        ControlSource::AUTO, ControlSource::WEB, ControlSource::SBUS};
    constexpr std::array<ControlSource, 3> web_sources{
        ControlSource::AUTO, ControlSource::WEB, ControlSource::WEB};
    for (const bool sbus_online : {false, true}) {
        for (const bool web_online : {false, true}) {
            for (std::size_t s = 0; s < values.size(); ++s) {
                for (std::size_t w = 0; w < values.size(); ++w) {
                    publish(sbus, sbus_online, values[s], -values[s]);
                    publish(web, web_online, values[w], -values[w]);
                    machine.updateControlSource();
                    const auto expected = sbus_online ? sbus_sources[s]
                                        : web_online ? web_sources[w] : ControlSource::AUTO;
                    require(machine.control_source() == expected, "online/CH5 matrix");
                    require(sbus.online == sbus_online && web.online == web_online,
                            "poll must not change online flags");
                    require(sbus.scaled_channels[4] == values[s] &&
                            web.scaled_channels[4] == values[w], "poll must not change CH5");
                }
            }
        }
    }
}

void scaleAndChannelSelection()
{
    marshland::sbus::SbusData sbus;
    marshland::web::WebData web;
    StateMachine machine{sbus, web};
    for (std::int32_t scale = 1; scale <= 10; ++scale) {
        for (const std::int32_t ch7 : {-660, 0, 660}) {
            publish(sbus, true, (364 - 1024) * scale, ch7);
            machine.updateControlSource();
            require(machine.control_source() == ControlSource::AUTO, "raw 364 maps to AUTO");
            publish(sbus, true, (1024 - 1024) * scale, ch7);
            machine.updateControlSource();
            require(machine.control_source() == ControlSource::WEB, "raw 1024 maps to WEB");
            publish(sbus, true, (1684 - 1024) * scale, ch7);
            machine.updateControlSource();
            require(machine.control_source() == ControlSource::SBUS, "raw 1684 maps to SBUS");
        }
    }
    publish(sbus, true, std::numeric_limits<std::int32_t>::min());
    machine.updateControlSource();
    require(machine.control_source() == ControlSource::AUTO, "negative CH5 without overflow");
    publish(sbus, true, std::numeric_limits<std::int32_t>::max());
    machine.updateControlSource();
    require(machine.control_source() == ControlSource::SBUS, "positive CH5 without overflow");
}

void disconnectAndReconnect()
{
    marshland::sbus::SbusData sbus;
    marshland::web::WebData web;
    StateMachine machine{sbus, web};
    publish(sbus, true, 660);
    publish(web, true, -660);
    machine.updateControlSource();
    require(machine.control_source() == ControlSource::SBUS, "SBUS owns selection");
    publish(sbus, false, 660);
    machine.updateControlSource();
    require(machine.control_source() == ControlSource::AUTO, "offline SBUS uses Web CH5");
    publish(web, true, 0);
    machine.updateControlSource();
    require(machine.control_source() == ControlSource::WEB, "Web can select WEB");
    publish(web, false, 0);
    machine.updateControlSource();
    require(machine.control_source() == ControlSource::AUTO, "both offline ignores stale CH5");
    publish(sbus, true, 0);
    machine.updateControlSource();
    require(machine.control_source() == ControlSource::WEB, "SBUS can select offline WEB");
    publish(sbus, true, 660);
    machine.updateControlSource();
    require(machine.control_source() == ControlSource::SBUS, "reconnect reapplies current CH5");
}

void onlineSbusDoesNotReadWeb()
{
    marshland::sbus::SbusData sbus;
    marshland::web::WebData web;
    StateMachine machine{sbus, web};
    publish(sbus, true, -660);
    // An online SBUS must not wait for a Web writer, even when selecting AUTO.
    std::lock_guard lock(web.mutex);
    machine.updateControlSource();
    require(machine.control_source() == ControlSource::AUTO, "SBUS selects without Web lock");
}

void concurrentPublishingAndReads()
{
    marshland::sbus::SbusData sbus;
    marshland::web::WebData web;
    StateMachine machine{sbus, web};
    publish(sbus, true, 660);
    publish(web, true, 0);
    machine.updateControlSource();
    std::atomic<bool> valid{true};
    std::thread sbus_writer([&] {
        for (int i = 0; i < 5000; ++i) {
            publish(sbus, i % 2 == 0, 660);
        }
    });
    std::thread web_writer([&] {
        for (int i = 0; i < 5000; ++i) {
            publish(web, true, 0, i);
        }
    });
    std::thread reader([&] {
        for (int i = 0; i < 10000; ++i) {
            const auto source = machine.control_source();
            if (source != ControlSource::SBUS && source != ControlSource::WEB) {
                valid.store(false);
            }
        }
    });
    for (int i = 0; i < 10000; ++i) {
        machine.updateControlSource();
    }
    sbus_writer.join();
    web_writer.join();
    reader.join();
    require(valid.load(), "concurrent readers must see a published control source");
}

void timedExitSignals()
{
    marshland::app::ProcessExitSignals signals;
    int signal = 0;
    require(signals.WaitFor(signal, 0ns) == EINVAL, "unprepared wait rejected");
    require(signals.Prepare() == 0, "prepare exit signals");
    require(signals.Prepare() == 0, "prepare is idempotent");
    require(signals.WaitFor(signal, -1ns) == EINVAL, "negative wait rejected");
    require(signals.WaitFor(signal, 1ms) == EAGAIN && signal == 0, "timed wait expires");
    require(std::raise(SIGTERM) == 0, "queue SIGTERM");
    require(signals.WaitFor(signal, 20ms) == 0 && signal == SIGTERM, "main consumes SIGTERM");
    require(std::raise(SIGINT) == 0, "queue SIGINT");
    require(signals.Wait(signal) == 0 && signal == SIGINT, "blocking wait remains compatible");
}

void shutdownOnMainThread()
{
    const auto main_thread = std::this_thread::get_id();
    std::atomic<bool> started{false};
    std::atomic<bool> exited{false};
    bool cleaned_on_main = false;
    marshland::runtime::RuntimeCoordinator runtime;
    runtime.registerModule("state-test-worker", [] { return true; },
        [&](std::atomic<bool>& running) {
            started.store(true);
            while (running.load()) {
                std::this_thread::yield();
            }
            exited.store(true);
        },
        [&] {
            cleaned_on_main = std::this_thread::get_id() == main_thread && exited.load();
        });
    require(runtime.start(), "start test worker");
    while (!started.load()) {
        std::this_thread::yield();
    }
    runtime.shutdown();
    require(exited.load() && cleaned_on_main, "main must join before cleanup");
}

} // namespace

int main()
{
    struct Test { const char* name; void (*run)(); };
    const std::array tests{
        Test{"defaults and explicit updates", defaultsAndExplicitUpdate},
        Test{"online/CH5 matrix (36 combinations)", sourceMatrix},
        Test{"CH5 scaling and CH7 independence", scaleAndChannelSelection},
        Test{"disconnect and reconnect", disconnectAndReconnect},
        Test{"online SBUS does not read Web", onlineSbusDoesNotReadWeb},
        Test{"concurrent publishing and reads", concurrentPublishingAndReads},
        Test{"timed exit signals", timedExitSignals},
        Test{"main-thread shutdown", shutdownOnMainThread}};
    try {
        for (const auto& test : tests) {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
