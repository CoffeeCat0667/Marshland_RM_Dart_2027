// Include the entry translation unit to exercise its real loop and shared data
// without adding test-only hooks to the production interface.
#define main MarshlandEntryMain
#include "../Entry.cpp"
#undef main

namespace {

bool awaitSource(state::ControlSource source)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (state::getGlobalStateMachine() != nullptr &&
            g_state_machine.control_source() == source) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

} // namespace

int main()
{
    app::ProcessExitSignals signals;
    if (signals.Prepare() != 0) {
        return 1;
    }
    const pthread_t main_thread = pthread_self();
    bool passed = true;
    std::thread producer([&] {
        {
            std::lock_guard lock(g_sbus_data.mutex);
            g_sbus_data.online = true;
            g_sbus_data.scaled_channels[4] = 660;
        }
        passed = awaitSource(state::ControlSource::SBUS);
        {
            std::lock_guard lock(g_sbus_data.mutex);
            g_sbus_data.scaled_channels[4] = -660;
        }
        passed = awaitSource(state::ControlSource::AUTO) && passed;
        {
            std::lock_guard lock(g_web_data.mutex);
            g_web_data.online = true;
            g_web_data.scaled_channels[4] = 0;
        }
        {
            std::lock_guard lock(g_sbus_data.mutex);
            g_sbus_data.online = false;
        }
        passed = awaitSource(state::ControlSource::WEB) && passed;
        {
            std::lock_guard lock(g_web_data.mutex);
            g_web_data.online = false;
        }
        passed = awaitSource(state::ControlSource::AUTO) && passed;
        if (pthread_kill(main_thread, SIGTERM) != 0) {
            passed = false;
        }
    });

    const int result = MarshlandEntryMain();
    producer.join();
    if (result != 0 || !passed || state::getGlobalStateMachine() != nullptr) {
        std::cerr << "FAIL entry polling and accessor lifetime\n";
        return 1;
    }
    std::cout << "PASS entry polls shared SBUS/Web data and clears accessor after shutdown\n";
    return 0;
}
