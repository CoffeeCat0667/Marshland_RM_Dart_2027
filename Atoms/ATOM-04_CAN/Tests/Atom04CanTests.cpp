// ============================================================================
// Atom04CanTests.cpp — ATOM-04 纯逻辑与形式化验证（无设备、无 SocketCAN 绑定）
//
// 覆盖 Plan 验收表：槽位映射、大端序、DLC/ID 校验、零力矩 payload、
// 未识别 ID 不投递、节点 ONLINE/TIMEOUT/FAULT 迁移、配置严格校验。
// ============================================================================
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CanTypes.hpp"
#include "IConfigView.hpp"
#include "atom04_can/can_config.hpp"
#include "atom04_can/can_core.hpp"
#include "atom04_can/motor_frame_codec.hpp"
#include "atom04_can/protocol_dispatcher.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* expression, int line)
{
    if (!condition) {
        std::printf("FAIL line %d: %s\n", line, expression);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

using marshland::can::CanError;
using marshland::can::CanFrame;
using marshland::can::CanNodeState;
using marshland::can::CanNodeStatus;
using marshland::can::CanNodeStatusStore;

// IConfigView 测试替身：不读任何文件，只回放注入的键值。
class FakeConfigView final : public marshland::config::IConfigView {
public:
    using Status = marshland::config::ConfigStatus;

    void setInt(const std::string& key, std::int64_t value) { ints_[key] = value; }
    void setString(const std::string& key, const std::string& value) { strings_[key] = value; }
    void setLoaded(bool loaded) { loaded_ = loaded; }
    void setAbsent(const std::string& key)
    {
        ints_.erase(key);
        strings_.erase(key);
    }

    Status getInt(const char* key, std::int64_t& out) const noexcept override
    {
        if (!loaded_) return Status::NOT_LOADED;
        const auto it = ints_.find(std::string(key));
        if (it == ints_.end()) return Status::MISSING;
        out = it->second;
        return Status::OK;
    }

    Status getDouble(const char*, double&) const noexcept override { return Status::INVALID_TYPE; }
    Status getBool(const char*, bool&) const noexcept override { return Status::INVALID_TYPE; }

    Status getString(const char* key, const char*& out_data, std::uint64_t& out_size) const noexcept override
    {
        if (!loaded_) return Status::NOT_LOADED;
        const auto it = strings_.find(std::string(key));
        if (it == strings_.end()) return Status::MISSING;
        out_data = it->second.c_str();
        out_size = it->second.size();
        return Status::OK;
    }

    bool has(const char* key) const noexcept override
    {
        return ints_.count(std::string(key)) != 0U || strings_.count(std::string(key)) != 0U;
    }

    Status getArraySize(const char* key, std::uint64_t& out_size) const noexcept override
    {
        if (!loaded_) return Status::NOT_LOADED;
        const auto it = arrays_.find(std::string(key));
        if (it == arrays_.end()) return Status::MISSING;
        out_size = it->second.size();
        return Status::OK;
    }

    void addMotor(int id, const std::string& node) { arrays_["motor"].push_back({id, node}); }

    Status getIntAt(const char*, std::uint64_t index, const char* element_key,
                    std::int64_t& out) const noexcept override
    {
        const auto it = arrays_.find("motor");
        if (it == arrays_.end() || index >= it->second.size()) return Status::MISSING;
        if (std::strcmp(element_key, "id") != 0) return Status::MISSING;
        out = it->second[index].first;
        return Status::OK;
    }

    Status getDoubleAt(const char*, std::uint64_t, const char*, double&) const noexcept override { return Status::MISSING; }
    Status getBoolAt(const char*, std::uint64_t, const char*, bool&) const noexcept override { return Status::MISSING; }

    Status getStringAt(const char*, std::uint64_t index, const char* element_key,
                       const char*& out_data, std::uint64_t& out_size) const noexcept override
    {
        const auto it = arrays_.find("motor");
        if (it == arrays_.end() || index >= it->second.size()) return Status::MISSING;
        if (std::strcmp(element_key, "node") != 0) return Status::MISSING;
        out_data = it->second[index].second.c_str();
        out_size = it->second[index].second.size();
        return Status::OK;
    }

    std::uint64_t configVersion() const noexcept override { return version_; }
    bool isLoaded() const noexcept override { return loaded_; }

private:
    std::unordered_map<std::string, std::int64_t> ints_;
    std::unordered_map<std::string, std::string> strings_;
    std::unordered_map<std::string, std::vector<std::pair<int, std::string>>> arrays_;
    bool loaded_{true};
    std::uint64_t version_{1};
};

class RecordingSink final : public marshland::can::ICanFrameSink {
public:
    void onCanFrame(const CanFrame& frame) noexcept override
    {
        ++count;
        last = frame;
    }
    int count{0};
    CanFrame last{};
};

// ---- CanCore 测试替身：记录发送内容，不接触任何真实设备 ----
class RecordingTransport final : public marshland::can::ICanTransport {
public:
    marshland::can::CanIoResult open(const char*) noexcept override
    {
        open_ = true;
        marshland::can::CanIoResult r{};
        return r;
    }
    void close() noexcept override { open_ = false; }

    marshland::can::CanIoResult receive(CanFrame&, int) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        marshland::can::CanIoResult r{};
        r.error = CanError::RECEIVE_TIMEOUT;
        return r;
    }

    marshland::can::CanIoResult send(const CanFrame& frame) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sent.push_back(frame);
        marshland::can::CanIoResult r{};
        if (fail_countdown > 0) {
            --fail_countdown;
            r.error = CanError::BUS_ERROR;
            r.system_errno = 5;
        }
        return r;
    }

    marshland::can::CanBusStatus busStatus() const noexcept override
    {
        marshland::can::CanBusStatus s{};
        s.open = open_;
        return s;
    }

    std::size_t countFrames(std::uint32_t can_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& f : sent) {
            if (f.can_id == can_id) ++n;
        }
        return n;
    }

    mutable std::mutex mutex_;
    std::vector<CanFrame> sent;
    bool open_{false};
    int fail_countdown{0};   // 由测试线程在 stop() 后设置，不会与 TX 线程竞争
};

class RecordingEventSink final : public marshland::can::ICanEventSink {
public:
    void onCanEvent(const marshland::can::CanEvent& event) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events.push_back(event);
    }
    std::size_t countKind(marshland::can::CanEventKind kind) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& e : events) {
            if (e.kind == kind) ++n;
        }
        return n;
    }
    mutable std::mutex mutex_;
    std::vector<marshland::can::CanEvent> events;
};

// 依据 Config/dart_control.json 的实际取值构造合法配置。
void fillValidConfig(FakeConfigView& view)
{
    view.setInt("can.data_bits", 8);
    view.setString("can.start_mode", "CLASSIC_CAN_FD_OFF");
    view.setString("can.rx_filter", "NONE");
    view.setString("can.error_frame_handling", "DISCARD");
    view.setString("can.zero_torque.send_mode", "OPEN_LOOP");
    view.setString("can.silence_trigger", "ZERO_TORQUE_SEND_FAILED");
    view.setInt("timing.can_io_period_ms", 20);
    view.setInt("timing.can_comm_timeout_ms", 200);
    view.setInt("timing.zero_torque_send_period_ms", 20);
    view.addMotor(1, "0x201");
    view.addMotor(2, "0x202");
    view.addMotor(3, "0x203");
    view.addMotor(4, "0x204");
    view.addMotor(5, "0x205");
    view.addMotor(6, "0x206");
}

void testSlotContract()
{
    const auto yaw = marshland::can::queryMotorSlot(0x205U);
    CHECK(yaw.valid);
    CHECK(yaw.frame_id == marshland::can::kMotorControlFrameIdB);
    CHECK(yaw.slot == 0U);

    const auto reload_right = marshland::can::queryMotorSlot(0x203U);
    CHECK(reload_right.frame_id == marshland::can::kMotorControlFrameIdA);
    CHECK(reload_right.slot == 2U);

    CHECK(!marshland::can::queryMotorSlot(0x1FFU).valid);
    CHECK(marshland::can::isMotorNode(0x206U));
    CHECK(!marshland::can::isMotorNode(0x207U));
}

void testBigEndianCodec()
{
    std::uint8_t buffer[2] = {0, 0};

    marshland::atom04::encodeBigEndianInt16(1000, buffer);
    CHECK(buffer[0] == 0x03U);  // 高字节先发
    CHECK(buffer[1] == 0xE8U);
    CHECK(marshland::atom04::decodeBigEndianInt16(buffer) == 1000);

    marshland::atom04::encodeBigEndianInt16(-1, buffer);
    CHECK(buffer[0] == 0xFFU);
    CHECK(buffer[1] == 0xFFU);
    CHECK(marshland::atom04::decodeBigEndianInt16(buffer) == -1);

    marshland::atom04::encodeBigEndianInt16(-32768, buffer);
    CHECK(buffer[0] == 0x80U);
    CHECK(buffer[1] == 0x00U);
    CHECK(marshland::atom04::decodeBigEndianInt16(buffer) == -32768);
}

void testControlFrame()
{
    marshland::atom04::MotorSlotValues currents{1000, -1000, 0, 32767};
    CanFrame frame{};
    CHECK(marshland::atom04::buildControlFrame(0x200U, currents, frame) == CanError::OK);
    CHECK(frame.can_id == 0x200U);
    CHECK(frame.dlc == 8U);
    CHECK(frame.payload[0] == 0x03U && frame.payload[1] == 0xE8U);
    CHECK(frame.payload[2] == 0xFCU && frame.payload[3] == 0x18U);
    CHECK(frame.payload[4] == 0x00U && frame.payload[5] == 0x00U);
    CHECK(frame.payload[6] == 0x7FU && frame.payload[7] == 0xFFU);
    CHECK(marshland::atom04::isValidControlFrame(frame));

    // 未使用槽位（0x1FF 的 slot2/slot3）必须保持 0x0000
    CanFrame frame_b{};
    CHECK(marshland::atom04::buildControlFrame(0x1FFU, currents, frame_b) == CanError::OK);
    CHECK(frame_b.can_id == 0x1FFU);

    // 非法 ID 必须被拒绝且不修改 out
    CanFrame untouched{};
    untouched.can_id = 0x123U;
    CHECK(marshland::atom04::buildControlFrame(0x123U, currents, untouched) == CanError::PARAM_INVALID);
    CHECK(untouched.can_id == 0x123U);
    CHECK(marshland::atom04::buildControlFrame(0x201U, currents, untouched) == CanError::PARAM_INVALID);

    // 反馈帧不得被判为控制帧
    CanFrame feedback{};
    feedback.can_id = 0x201U;
    feedback.dlc = 8U;
    CHECK(!marshland::atom04::isValidControlFrame(feedback));
}

void testZeroTorqueFrame()
{
    for (const std::uint32_t id : {0x200U, 0x1FFU}) {
        CanFrame frame{};
        frame.payload.fill(0xABU);  // 先污染，确认构造会清零
        CHECK(marshland::atom04::buildZeroTorqueFrame(id, frame) == CanError::OK);
        CHECK(frame.can_id == id);
        CHECK(frame.dlc == 8U);
        CHECK(marshland::atom04::isZeroTorquePayload(frame));
        for (std::size_t i = 0; i < 8U; ++i) CHECK(frame.payload[i] == 0x00U);
    }

    CanFrame bad{};
    CHECK(marshland::atom04::buildZeroTorqueFrame(0x202U, bad) == CanError::PARAM_INVALID);
}

void testDispatchAndNodeStates()
{
    marshland::atom04::CanRuntimeConfig config{};
    config.comm_timeout_ms = 200;
    config.motor_count = 2;
    config.motors[0] = {1, 0x201U};
    config.motors[1] = {5, 0x205U};

    CanNodeStatusStore store;
    RecordingSink sink;
    marshland::atom04::ProtocolDispatcher dispatcher(sink, store);
    dispatcher.configure(config);
    CHECK(dispatcher.registerNodes());

    // 反馈帧原样投递，不做字段解析
    CanFrame feedback{};
    feedback.can_id = 0x201U;
    feedback.dlc = 8U;
    feedback.payload = {1, 2, 3, 4, 5, 6, 7, 8};
    feedback.timestamp_us = 1000;
    auto outcome = dispatcher.onReceived(feedback);
    CHECK(outcome.error == CanError::OK);
    CHECK(outcome.node_became_online);
    CHECK(outcome.motor_id == 1 && outcome.motor_id == config.motors[0].motor_id);
    CHECK(sink.count == 1);
    CHECK(sink.last.payload[0] == 1U && sink.last.payload[7] == 8U);

    CanNodeStatus nodes[8]{};
    std::size_t count = store.snapshot(nodes, 8);
    CHECK(count == 2U);
    CHECK(nodes[0].state == CanNodeState::ONLINE);
    CHECK(nodes[0].frame_count == 1U);

    // 未识别 ID：不投递、不臆造
    CanFrame unknown{};
    unknown.can_id = 0x300U;
    unknown.dlc = 8U;
    outcome = dispatcher.onReceived(unknown);
    CHECK(outcome.error == CanError::UNKNOWN_ID);
    CHECK(sink.count == 1);

    // 落入 0x201~0x206 但未登记的节点：不得投递
    CanFrame unregistered{};
    unregistered.can_id = 0x202U;
    unregistered.dlc = 8U;
    outcome = dispatcher.onReceived(unregistered);
    CHECK(outcome.error == CanError::UNKNOWN_ID);
    CHECK(sink.count == 1);

    // 超时：200 ms 阈值；边界前不超时
    CanNodeStatus expired[8]{};
    CHECK(dispatcher.pollTimeouts(190000U, expired, 8) == 0U);       // 距 1000us 已过 189 ms
    CHECK(dispatcher.pollTimeouts(200999U, expired, 8) == 0U);       // 199.999 ms，未达阈值
    CHECK(dispatcher.pollTimeouts(201000U, expired, 8) == 1U);       // 恰好 200 ms，判超时
    count = store.snapshot(nodes, 8);
    CHECK(nodes[0].state == CanNodeState::TIMEOUT);
    CHECK(nodes[1].state == CanNodeState::UNKNOWN);

    // 反馈恢复 -> ONLINE，并再次上报跃变
    CanFrame recovered = feedback;
    recovered.timestamp_us = 300000;
    outcome = dispatcher.onReceived(recovered);
    CHECK(outcome.node_became_online);
    store.snapshot(nodes, 8);
    CHECK(nodes[0].state == CanNodeState::ONLINE);
    CHECK(nodes[0].frame_count == 2U);

    // FAULT 为终态：静默不得把 FAULT 改写为 TIMEOUT
    CHECK(dispatcher.markFault(0x201U));
    store.snapshot(nodes, 8);
    CHECK(nodes[0].state == CanNodeState::FAULT);
    CHECK(dispatcher.pollTimeouts(900000U, expired, 8) == 0U);
    store.snapshot(nodes, 8);
    CHECK(nodes[0].state == CanNodeState::FAULT);

    // ---- 缺陷 D5 回归：时钟起点附近的真实帧（timestamp_us == 0）----
    // 旧实现用 last_frame_time_us == 0 表示"从未收到"，会把这类真实帧误判为
    // 已超时；现在由显式 has_received 标志判定。
    CanNodeStatusStore store2;
    RecordingSink sink2;
    marshland::atom04::ProtocolDispatcher dispatcher2(sink2, store2);
    dispatcher2.configure(config);
    CHECK(dispatcher2.registerNodes());

    CanFrame at_origin{};
    at_origin.can_id = 0x201U;
    at_origin.dlc = 8U;
    at_origin.timestamp_us = 0U;   // 单调时钟起点附近
    CHECK(dispatcher2.onReceived(at_origin).error == CanError::OK);

    CanNodeStatus origin_nodes[8]{};
    store2.snapshot(origin_nodes, 8);
    CHECK(origin_nodes[0].has_received);
    CHECK(origin_nodes[0].last_frame_time_us == 0U);
    // 仅过了 100 ms：不得超时（旧实现会在此处误报超时）
    CHECK(dispatcher2.pollTimeouts(100000U, expired, 8) == 0U);
    store2.snapshot(origin_nodes, 8);
    CHECK(origin_nodes[0].state == CanNodeState::ONLINE);

    // 未收到过反馈的节点不得被超时轮询处理
    CanNodeStatus fresh[8]{};
    store2.snapshot(fresh, 8);
    CHECK(!fresh[1].has_received);
    CHECK(fresh[1].state == CanNodeState::UNKNOWN);
    CHECK(dispatcher2.pollTimeouts(99999999U, expired, 8) == 1U);  // 只应命中节点 0
    store2.snapshot(fresh, 8);
    CHECK(fresh[1].state == CanNodeState::UNKNOWN);
}

void testConfigValidation()
{
    using Status = marshland::config::ConfigStatus;
    marshland::atom04::CanRuntimeConfig config{};

    {
        FakeConfigView view;
        fillValidConfig(view);
        CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == Status::OK);
        CHECK(config.motor_count == 6U);
        CHECK(config.interface_name == "can0");   // 授权偏差：全部使用 can0
        CHECK(config.rx_period_ms == 20);
        CHECK(config.comm_timeout_ms == 200);
        CHECK(config.zero_torque_period_ms == 20);
    }
    {
        FakeConfigView view;
        view.setLoaded(false);
        CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == Status::NOT_LOADED);
    }
    {
        FakeConfigView view;
        fillValidConfig(view);
        view.setAbsent("can.silence_trigger");               // 缺失必须失败
        CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == Status::MISSING);
    }
    {
        FakeConfigView view;
        fillValidConfig(view);
        view.setInt("can.data_bits", 7);                     // 取值不符
        CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE);
    }
    {
        FakeConfigView view;
        fillValidConfig(view);
        view.setString("can.zero_torque.send_mode", "CLOSED_LOOP");
        CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE);
    }
    {
        FakeConfigView view;
        fillValidConfig(view);
        view.setInt("timing.can_io_period_ms", 0);           // 周期越界
        CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE);
    }
    {
        FakeConfigView view;
        fillValidConfig(view);
        view.setInt("timing.can_comm_timeout_ms", 0);
        CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE);
    }
    {
        FakeConfigView view;
        fillValidConfig(view);
        view.addMotor(7, "0x208");                           // 未定义节点
        CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE);
    }
    {
        FakeConfigView view;
        fillValidConfig(view);
        view.addMotor(7, "0x201");                           // 重复 node
        CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE);
    }
    {
        FakeConfigView view;
        fillValidConfig(view);
        view.addMotor(2, "0x206");                           // 重复 motor id
        CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE);
    }
    {
        FakeConfigView view;
        fillValidConfig(view);
        view.setInt("timing.zero_torque_send_period_ms", 99999);
        CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE);
    }
    {
        // 失败加载不得部分提交：先建立 good 状态，再改坏一个键。
        FakeConfigView good;
        fillValidConfig(good);
        marshland::atom04::CanRuntimeConfig stable{};
        CHECK(marshland::atom04::loadCanRuntimeConfig(good, stable) == Status::OK);
        const std::size_t before = stable.motor_count;

        FakeConfigView broken;
        fillValidConfig(broken);
        broken.setString("can.start_mode", "CAN_FD");
        CHECK(marshland::atom04::loadCanRuntimeConfig(broken, stable) == Status::OUT_OF_RANGE);
        CHECK(stable.motor_count == before);
        CHECK(stable.interface_name == "can0");
    }
}

// 缺陷 D3 回归：发送线程必须发射已锁存的控制帧，而不是每周期都发零力矩帧，
// 否则真实电流会被覆盖，电机永远无法出力。
void testCoreLatchAndTransmit()
{
    FakeConfigView view;
    fillValidConfig(view);
    marshland::atom04::CanRuntimeConfig config{};
    CHECK(marshland::atom04::loadCanRuntimeConfig(view, config) == marshland::config::ConfigStatus::OK);

    // 缩短周期，让测试不必等 20 ms × 多轮
    config.tx_period_ms = 5;
    config.zero_torque_period_ms = 5;
    config.rx_period_ms = 5;

    RecordingTransport transport;
    CanNodeStatusStore store;
    RecordingSink sink;
    RecordingEventSink events;

    marshland::atom04::CanCore::Dependencies deps{transport, store, sink, &events, nullptr};
    marshland::atom04::CanCore core(deps);
    std::string detail;
    CHECK(core.start(config, detail));
    CHECK(core.isRunning());

    // 初始锁存值必须为全零（等价于零力矩帧），且查询接口可用
    marshland::atom04::MotorSlotValues latched{};
    latched.fill(123);
    CHECK(core.latchedCurrents(0x200U, latched));
    for (std::size_t i = 0; i < marshland::can::kMotorSlotsPerFrame; ++i) CHECK(latched[i] == 0);
    CHECK(core.latchedCurrents(0x1FFU, latched));
    for (std::size_t i = 0; i < marshland::can::kMotorSlotsPerFrame; ++i) CHECK(latched[i] == 0);

    // 非法帧 ID 不得被锁存
    CHECK(!core.latchMotorCurrents(0x201U, latched));

    // 锁存真实电流：0x200 承载电机 1..4
    marshland::atom04::MotorSlotValues command{1000, -1000, 500, 0};
    CHECK(core.latchMotorCurrents(0x200U, command));
    CHECK(core.latchedCurrents(0x200U, latched));
    CHECK(latched[0] == 1000 && latched[1] == -1000 && latched[2] == 500 && latched[3] == 0);

    // 等待若干发送周期后，总线上必须出现承载大端序控制量的 0x200 帧。
    // 缺陷 D3 的旧实现只会发全零帧，因此这里断言"非零载荷"是有效回归判据。
    bool saw_nonzero = false;
    for (int attempt = 0; attempt < 200 && !saw_nonzero; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::lock_guard<std::mutex> lock(transport.mutex_);
        for (const auto& f : transport.sent) {
            if (f.can_id == 0x200U && f.payload[0] == 0x03U && f.payload[1] == 0xE8U) {
                saw_nonzero = true;
                CHECK(f.dlc == 8U);
                break;
            }
        }
    }
    CHECK(saw_nonzero);
    CHECK(transport.countFrames(0x200U) > 0U);
    CHECK(transport.countFrames(0x1FFU) > 0U);   // 未锁存的帧按全零发送

    // 强制归零后，必须回到全零载荷
    core.forceZeroCurrents();
    CHECK(core.latchedCurrents(0x200U, latched));
    for (std::size_t i = 0; i < marshland::can::kMotorSlotsPerFrame; ++i) CHECK(latched[i] == 0);

    // 先停止工作线程，使失败注入是确定性的（避免与 TX 线程竞争）
    core.stop();
    CHECK(!core.isRunning());

    // 发送失败的安全路径：零力矩发送失败必须发布 ZERO_TORQUE_FAILED 事件
    transport.fail_countdown = 2;   // 0x200 与 0x1FF 两帧都失败
    (void)core.sendZeroTorque();
    CHECK(events.countKind(marshland::can::CanEventKind::ZERO_TORQUE_FAILED) == 2U);

    // 失败后锁存必须已被强制归零（安全路径）
    CHECK(core.latchedCurrents(0x200U, latched));
    for (std::size_t i = 0; i < marshland::can::kMotorSlotsPerFrame; ++i) CHECK(latched[i] == 0);
    // 幂等：重复 stop() 不得崩溃或抛异常
    core.stop();
    core.stop();
}

} // namespace

int main()
{
    testSlotContract();
    testBigEndianCodec();
    testControlFrame();
    testZeroTorqueFrame();
    testDispatchAndNodeStates();
    testConfigValidation();
    testCoreLatchAndTransmit();

    if (g_failures == 0) {
        std::printf("atom04: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("atom04: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
