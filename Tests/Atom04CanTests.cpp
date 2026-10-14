// ============================================================================
// Atom04CanTests.cpp — ATOM-04_CAN 验证（按 Tests/ 全局约定：命名用例 + Check 抛异常）
//
// 位置约定：本文件位于仓库 Tests/，由顶层 CMake 的 MARSHLAND_BUILD_TESTS 段
// 注册为 marshland_atom04_tests，并通过具名用例注册为 CTest 的 marshland.<case>。
// ATOM 模块目录内**不**包含任何 main() 入口（原 Atoms/ATOM-04_CAN/Tests 已移除）。
//
// 运行：marshland_atom04_tests <case-name>
// ============================================================================
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
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

using namespace marshland;
using namespace marshland::atom04;

using can::CanError;
using can::CanEventKind;
using can::CanFrame;
using can::CanNodeState;
using can::CanNodeStatus;
using can::CanNodeStatusStore;

void Check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// ---------------------------------------------------------------------------
// IConfigView 测试替身：不读任何文件，只回放注入的键值。
// ---------------------------------------------------------------------------
class FakeConfigView final : public config::IConfigView {
public:
    using Status = config::ConfigStatus;

    void setInt(const std::string& key, std::int64_t value) { ints_[key] = value; }
    void setString(const std::string& key, const std::string& value) { strings_[key] = value; }
    void setLoaded(bool loaded) { loaded_ = loaded; }
    void setAbsent(const std::string& key)
    {
        ints_.erase(key);
        strings_.erase(key);
    }
    void clearMotors() { motors_.clear(); }
    void addMotor(int id, const std::string& node) { motors_.push_back({id, node}); }

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
    Status getString(const char* key, const char*& out_data,
                     std::uint64_t& out_size) const noexcept override
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
        if (std::strcmp(key, "motor") != 0) return Status::MISSING;
        out_size = motors_.size();
        return Status::OK;
    }
    Status getIntAt(const char* key, std::uint64_t index, const char* element_key,
                    std::int64_t& out) const noexcept override
    {
        if (std::strcmp(key, "motor") != 0 || index >= motors_.size()) return Status::MISSING;
        if (std::strcmp(element_key, "id") != 0) return Status::MISSING;
        out = motors_[index].first;
        return Status::OK;
    }
    Status getDoubleAt(const char*, std::uint64_t, const char*, double&) const noexcept override
    {
        return Status::MISSING;
    }
    Status getBoolAt(const char*, std::uint64_t, const char*, bool&) const noexcept override
    {
        return Status::MISSING;
    }
    Status getStringAt(const char* key, std::uint64_t index, const char* element_key,
                       const char*& out_data, std::uint64_t& out_size) const noexcept override
    {
        if (std::strcmp(key, "motor") != 0 || index >= motors_.size()) return Status::MISSING;
        if (std::strcmp(element_key, "node") != 0) return Status::MISSING;
        out_data = motors_[index].second.c_str();
        out_size = motors_[index].second.size();
        return Status::OK;
    }
    std::uint64_t configVersion() const noexcept override { return version_; }
    bool isLoaded() const noexcept override { return loaded_; }

private:
    std::unordered_map<std::string, std::int64_t> ints_;
    std::unordered_map<std::string, std::string> strings_;
    // 用 vector 而非 unordered_map：保持写入顺序，便于构造乱序/缺失节点场景。
    std::vector<std::pair<int, std::string>> motors_;
    bool loaded_{true};
    std::uint64_t version_{1};
};

class RecordingSink final : public can::ICanFrameSink {
public:
    void onCanFrame(const CanFrame& frame) noexcept override
    {
        ++count;
        last = frame;
    }
    int count{0};
    CanFrame last{};
};

class RecordingEventSink final : public can::ICanEventSink {
public:
    void onCanEvent(const can::CanEvent& event) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events.push_back(event);
    }
    std::size_t countKind(CanEventKind kind) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& e : events) {
            if (e.kind == kind) ++n;
        }
        return n;
    }
    mutable std::mutex mutex_;
    std::vector<can::CanEvent> events;
};

class RecordingTransport final : public can::ICanTransport {
public:
    can::CanIoResult open(const char*) noexcept override
    {
        open_ = true;
        can::CanIoResult r{};
        return r;
    }
    void close() noexcept override { open_ = false; }
    can::CanIoResult receive(CanFrame&, int) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        can::CanIoResult r{};
        r.error = CanError::RECEIVE_TIMEOUT;
        return r;
    }
    can::CanIoResult send(const CanFrame& frame) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sent.push_back(frame);
        can::CanIoResult r{};
        if (fail_countdown > 0) {
            --fail_countdown;
            r.error = CanError::BUS_ERROR;
            r.system_errno = 5;
        }
        return r;
    }
    can::CanBusStatus busStatus() const noexcept override
    {
        can::CanBusStatus s{};
        s.open = open_;
        return s;
    }

    // 统计某一 CAN ID 上"全零载荷"的帧数（零力矩帧判据）。
    std::size_t countZeroPayload(std::uint32_t can_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& f : sent) {
            if (f.can_id != can_id) continue;
            bool all_zero = true;
            for (std::size_t i = 0; i < can::kCanPayloadBytes; ++i) {
                if (f.payload[i] != 0U) { all_zero = false; break; }
            }
            if (all_zero) ++n;
        }
        return n;
    }
    std::size_t countNonZero(std::uint32_t can_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& f : sent) {
            if (f.can_id != can_id) continue;
            bool any = false;
            for (std::size_t i = 0; i < can::kCanPayloadBytes; ++i) {
                if (f.payload[i] != 0U) { any = true; break; }
            }
            if (any) ++n;
        }
        return n;
    }
    std::size_t total() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent.size();
    }
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sent.clear();
    }

    mutable std::mutex mutex_;
    std::vector<CanFrame> sent;
    bool open_{false};
    int fail_countdown{0};   // 由测试线程在 stop() 后设置，不与 TX 线程竞争
};

// 依据 Config/dart_control.json 的实际取值构造合法配置（恰好 6 个节点）。
void FillValidConfig(FakeConfigView& view)
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
    view.clearMotors();
    view.addMotor(1, "0x201");
    view.addMotor(2, "0x202");
    view.addMotor(3, "0x203");
    view.addMotor(4, "0x204");
    view.addMotor(5, "0x205");
    view.addMotor(6, "0x206");
}

// ---------------------------------------------------------------------------
// 用例：槽位契约
// ---------------------------------------------------------------------------
void TestSlotContract()
{
    const auto yaw = can::queryMotorSlot(0x205U);
    Check(yaw.valid, "0x205 must map to a control slot");
    Check(yaw.frame_id == can::kMotorControlFrameIdB, "YAW must live in 0x1FF");
    Check(yaw.slot == 0U, "YAW must be 0x1FF slot 0");

    const auto reload_right = can::queryMotorSlot(0x203U);
    Check(reload_right.frame_id == can::kMotorControlFrameIdA, "reload_right in 0x200");
    Check(reload_right.slot == 2U, "reload_right must be 0x200 slot 2");

    const auto charge_right = can::queryMotorSlot(0x201U);
    Check(charge_right.slot == 0U, "charge_right must be 0x200 slot 0");

    Check(!can::queryMotorSlot(0x207U).valid, "0x207 has no control slot");
    Check(!can::queryMotorSlot(0x1FFU).valid, "0x1FF is a frame ID, not a node");
    Check(can::isMotorNode(0x206U), "0x206 is a motor node");
    Check(!can::isMotorNode(0x200U), "0x200 is not a motor node");
}

// ---------------------------------------------------------------------------
// 用例：大端序编解码
// ---------------------------------------------------------------------------
void TestBigEndianCodec()
{
    std::uint8_t buffer[2] = {0, 0};

    encodeBigEndianInt16(1000, buffer);
    Check(buffer[0] == 0x03U, "big-endian high byte first (1000)");
    Check(buffer[1] == 0xE8U, "big-endian low byte (1000)");
    Check(decodeBigEndianInt16(buffer) == 1000, "roundtrip 1000");

    encodeBigEndianInt16(-1, buffer);
    Check(buffer[0] == 0xFFU && buffer[1] == 0xFFU, "big-endian -1 is FF FF");
    Check(decodeBigEndianInt16(buffer) == -1, "roundtrip -1");

    encodeBigEndianInt16(-32768, buffer);
    Check(buffer[0] == 0x80U && buffer[1] == 0x00U, "big-endian INT16_MIN");
    Check(decodeBigEndianInt16(buffer) == -32768, "roundtrip INT16_MIN");
}

// ---------------------------------------------------------------------------
// 用例：控制帧与保留槽位
// ---------------------------------------------------------------------------
void TestControlFrame()
{
    MotorSlotValues currents{1000, -1000, 0, 32767};
    CanFrame frame{};
    Check(buildControlFrame(0x200U, currents, frame) == CanError::OK, "build 0x200");
    Check(frame.can_id == 0x200U, "0x200 id");
    Check(frame.dlc == 8U, "0x200 dlc");
    Check(frame.payload[0] == 0x03U && frame.payload[1] == 0xE8U, "slot0 big-endian 1000");
    Check(frame.payload[2] == 0xFCU && frame.payload[3] == 0x18U, "slot1 big-endian -1000");
    Check(frame.payload[6] == 0x7FU && frame.payload[7] == 0xFFU, "slot3 big-endian 32767");
    Check(isValidControlFrame(frame), "0x200 frame is a valid control frame");

    // 缺陷 #3：0x1FF 的 slot2/slot3 无对应节点，必须恒为 0x0000，
    // 即使调用方传入非零值也不得发出虚构的 0x207/0x208 数据。
    MotorSlotValues bogus{1, 2, 0x7FFF, -0x7FFF};
    CanFrame frame_b{};
    Check(buildControlFrame(0x1FFU, bogus, frame_b) == CanError::OK, "build 0x1FF");
    Check(frame_b.can_id == 0x1FFU, "0x1FF id");
    Check(frame_b.payload[0] == 0x00U && frame_b.payload[1] == 0x01U, "0x1FF slot0 kept");
    Check(frame_b.payload[2] == 0x00U && frame_b.payload[3] == 0x02U, "0x1FF slot1 kept");
    Check(frame_b.payload[4] == 0x00U && frame_b.payload[5] == 0x00U,
          "0x1FF slot2 must be forced to zero");
    Check(frame_b.payload[6] == 0x00U && frame_b.payload[7] == 0x00U,
          "0x1FF slot3 must be forced to zero");

    // 非法 ID 被拒且不修改 out
    CanFrame untouched{};
    untouched.can_id = 0x123U;
    Check(buildControlFrame(0x123U, currents, untouched) == CanError::PARAM_INVALID, "reject bad id");
    Check(buildControlFrame(0x201U, currents, untouched) == CanError::PARAM_INVALID,
          "reject feedback id as control frame");
    Check(untouched.can_id == 0x123U, "rejected build must not modify out");

    CanFrame feedback{};
    feedback.can_id = 0x201U;
    feedback.dlc = 8U;
    Check(!isValidControlFrame(feedback), "feedback frame is not a control frame");
}

// ---------------------------------------------------------------------------
// 用例：零力矩帧
// ---------------------------------------------------------------------------
void TestZeroTorqueFrame()
{
    const std::uint32_t ids[2] = {0x200U, 0x1FFU};
    for (const std::uint32_t id : ids) {
        CanFrame frame{};
        frame.payload.fill(0xABU);
        Check(buildZeroTorqueFrame(id, frame) == CanError::OK, "build zero torque frame");
        Check(frame.can_id == id, "zero torque id");
        Check(frame.dlc == 8U, "zero torque dlc");
        Check(isZeroTorquePayload(frame), "zero torque payload is all zero");
        for (std::size_t i = 0; i < can::kCanPayloadBytes; ++i) {
            Check(frame.payload[i] == 0x00U, "zero torque byte must be 0x00");
        }
    }
    CanFrame bad{};
    Check(buildZeroTorqueFrame(0x202U, bad) == CanError::PARAM_INVALID, "reject bad zero torque id");
}

// ---------------------------------------------------------------------------
// 用例：接收侧校验（DLC / 未识别 ID）
// ---------------------------------------------------------------------------
void TestReceiveValidation()
{
    CanRuntimeConfig config{};
    config.comm_timeout_ms = 200;
    config.motor_count = 2;
    config.motors[0] = {1, 0x201U};
    config.motors[1] = {5, 0x205U};

    CanNodeStatusStore store;
    RecordingSink sink;
    ProtocolDispatcher dispatcher(sink, store);
    dispatcher.configure(config);
    Check(dispatcher.registerNodes(), "registerNodes");

    CanNodeStatus nodes[can::kMaxMotorNodes]{};

    // 缺陷 #4：非 8 字节帧必须被拒，不得刷新在线状态，也不得投递。
    for (const std::uint8_t dlc : {0U, 1U, 3U, 7U}) {
        CanFrame truncated{};
        truncated.can_id = 0x201U;
        truncated.dlc = dlc;
        truncated.timestamp_us = 1000U;
        const DispatchOutcome outcome = dispatcher.onReceived(truncated);
        Check(outcome.error == CanError::FRAME_INVALID, "truncated frame must be FRAME_INVALID");
    }
    Check(sink.count == 0, "truncated frames must not be delivered");
    store.snapshot(nodes, can::kMaxMotorNodes);
    Check(nodes[0].state == CanNodeState::UNKNOWN, "truncated frame must not mark ONLINE");
    Check(!nodes[0].has_received, "truncated frame must not set has_received");

    // 合法反馈帧：原样投递，零字段解析
    CanFrame feedback{};
    feedback.can_id = 0x201U;
    feedback.dlc = 8U;
    feedback.payload = {1, 2, 3, 4, 5, 6, 7, 8};
    feedback.timestamp_us = 1000U;
    DispatchOutcome outcome = dispatcher.onReceived(feedback);
    Check(outcome.error == CanError::OK, "valid feedback accepted");
    Check(outcome.node_became_online, "first frame marks online transition");
    Check(outcome.motor_id == 1, "feedback mapped to motor 1");
    Check(sink.count == 1, "valid feedback delivered once");
    Check(sink.last.payload[0] == 1U && sink.last.payload[7] == 8U, "payload delivered verbatim");

    store.snapshot(nodes, can::kMaxMotorNodes);
    Check(nodes[0].state == CanNodeState::ONLINE, "node online");
    Check(nodes[0].frame_count == 1U, "frame counted");

    // 未识别 ID：不投递、不臆造
    CanFrame unknown{};
    unknown.can_id = 0x300U;
    unknown.dlc = 8U;
    Check(dispatcher.onReceived(unknown).error == CanError::UNKNOWN_ID, "unknown id rejected");
    Check(sink.count == 1, "unknown id not delivered");

    // 落在 0x201~0x206 但配置未登记：同样不投递
    CanFrame unregistered{};
    unregistered.can_id = 0x202U;
    unregistered.dlc = 8U;
    Check(dispatcher.onReceived(unregistered).error == CanError::UNKNOWN_ID,
          "unregistered node rejected");
    Check(sink.count == 1, "unregistered node not delivered");
}

// ---------------------------------------------------------------------------
// 用例：节点状态迁移（超时 / FAULT 粘滞 / 快照语义）
// ---------------------------------------------------------------------------
void TestNodeStateTransitions()
{
    CanRuntimeConfig config{};
    config.comm_timeout_ms = 200;
    config.motor_count = 2;
    config.motors[0] = {1, 0x201U};
    config.motors[1] = {5, 0x205U};

    CanNodeStatusStore store;
    RecordingSink sink;
    ProtocolDispatcher dispatcher(sink, store);
    dispatcher.configure(config);
    Check(dispatcher.registerNodes(), "registerNodes");

    CanFrame feedback{};
    feedback.can_id = 0x201U;
    feedback.dlc = 8U;
    feedback.timestamp_us = 1000U;
    dispatcher.onReceived(feedback);

    CanNodeStatus nodes[can::kMaxMotorNodes]{};
    CanNodeStatus expired[can::kMaxMotorNodes]{};

    Check(dispatcher.pollTimeouts(190000U, expired, can::kMaxMotorNodes) == 0U,
          "189 ms must not time out");
    Check(dispatcher.pollTimeouts(200999U, expired, can::kMaxMotorNodes) == 0U,
          "199.999 ms must not time out");
    Check(dispatcher.pollTimeouts(201000U, expired, can::kMaxMotorNodes) == 1U,
          "exactly 200 ms must time out");
    store.snapshot(nodes, can::kMaxMotorNodes);
    Check(nodes[0].state == CanNodeState::TIMEOUT, "node marked TIMEOUT later");

    // 反馈恢复 -> ONLINE
    CanFrame recovered = feedback;
    recovered.timestamp_us = 300000U;
    Check(dispatcher.onReceived(recovered).node_became_online, "recovery reports transition");
    store.snapshot(nodes, can::kMaxMotorNodes);
    Check(nodes[0].state == CanNodeState::ONLINE, "recovered to ONLINE");
    Check(nodes[0].frame_count == 2U, "frame count keeps incrementing");

    // 缺陷 #5：FAULT 为粘滞终态。故障后收到反馈仍必须保持 FAULT。
    Check(dispatcher.markFault(0x201U), "markFault");
    store.snapshot(nodes, can::kMaxMotorNodes);
    Check(nodes[0].state == CanNodeState::FAULT, "node in FAULT");

    Check(dispatcher.pollTimeouts(900000U, expired, can::kMaxMotorNodes) == 0U,
          "timeout poll must not touch FAULT node");
    store.snapshot(nodes, can::kMaxMotorNodes);
    Check(nodes[0].state == CanNodeState::FAULT, "FAULT survives timeout poll");

    CanFrame after_fault = feedback;
    after_fault.timestamp_us = 950000U;
    const DispatchOutcome faulted = dispatcher.onReceived(after_fault);
    Check(faulted.error == CanError::OK, "faulted node still delivers frames");
    Check(!faulted.node_became_online, "faulted node must not announce recovery");
    store.snapshot(nodes, can::kMaxMotorNodes);
    Check(nodes[0].state == CanNodeState::FAULT, "FAULT survives subsequent feedback (sticky)");
    Check(sink.count == 3, "faulted node frames still forwarded to ATOM-05");

    // 只有显式 markOnline 才能解除故障
    Check(dispatcher.markOnline(0x201U, 1000000U), "markOnline clears FAULT");
    store.snapshot(nodes, can::kMaxMotorNodes);
    Check(nodes[0].state == CanNodeState::ONLINE, "explicit clear restores ONLINE");
    Check(!dispatcher.markOnline(0x201U, 1000001U), "markOnline is only for faulted nodes");

    // 缺陷 D5：时钟起点附近的真实帧不得被误判超时
    CanNodeStatusStore store2;
    RecordingSink sink2;
    ProtocolDispatcher dispatcher2(sink2, store2);
    dispatcher2.configure(config);
    Check(dispatcher2.registerNodes(), "registerNodes (origin)");

    CanFrame at_origin{};
    at_origin.can_id = 0x201U;
    at_origin.dlc = 8U;
    at_origin.timestamp_us = 0U;
    Check(dispatcher2.onReceived(at_origin).error == CanError::OK, "frame at t=0 accepted");
    CanNodeStatus origin[can::kMaxMotorNodes]{};
    store2.snapshot(origin, can::kMaxMotorNodes);
    Check(origin[0].has_received, "has_received set for t=0 frame");
    Check(dispatcher2.pollTimeouts(100000U, expired, can::kMaxMotorNodes) == 0U,
          "t=0 frame must not be treated as already timed out");

    // 从未收到反馈的节点不得被超时轮询处理
    Check(!origin[1].has_received, "second node never received");
    Check(origin[1].state == CanNodeState::UNKNOWN, "second node stays UNKNOWN");
    Check(dispatcher2.pollTimeouts(99999999U, expired, can::kMaxMotorNodes) == 1U,
          "only the received node can time out");
    store2.snapshot(origin, can::kMaxMotorNodes);
    Check(origin[1].state == CanNodeState::UNKNOWN, "UNKNOWN node untouched by timeout poll");
}

// ---------------------------------------------------------------------------
// 用例：快照接口语义（缺陷 #7）
// ---------------------------------------------------------------------------
void TestSnapshotSemantics()
{
    CanNodeStatusStore store;
    CanNodeStatus nodes[can::kMaxMotorNodes]{};
    for (std::size_t i = 0; i < 3U; ++i) {
        nodes[i].motor_id = static_cast<int>(i) + 1;
        nodes[i].node_id = 0x201U + static_cast<std::uint32_t>(i);
    }
    Check(store.replaceAll(nodes, 3U), "replaceAll");

    CanNodeStatus out[can::kMaxMotorNodes]{};
    // 返回值必须是**实际复制数量**，不是已登记总数
    Check(store.snapshot(out, 1U) == 1U, "snapshot returns copied count when capacity is smaller");
    Check(store.snapshot(out, 3U) == 3U, "snapshot returns copied count when capacity is exact");
    Check(store.snapshot(out, can::kMaxMotorNodes) == 3U, "snapshot copies all registered");
    Check(store.size() == 3U, "size returns registered total");

    // 空指针 / 零容量必须安全返回 0（不得解引用空指针）
    Check(store.snapshot(nullptr, 3U) == 0U, "nullptr output must be safe");
    Check(store.snapshot(out, 0U) == 0U, "zero capacity must be safe");
    Check(store.snapshot(nullptr, 0U) == 0U, "nullptr + zero capacity must be safe");
}

// ---------------------------------------------------------------------------
// 用例：配置校验
// ---------------------------------------------------------------------------
void TestConfigValidation()
{
    using Status = config::ConfigStatus;
    CanRuntimeConfig config{};

    {
        FakeConfigView view;
        FillValidConfig(view);
        Check(loadCanRuntimeConfig(view, config) == Status::OK, "valid config loads");
        Check(config.motor_count == 6U, "all six motors registered");
        Check(config.interface_name == "can0", "interface name is can0");
        Check(config.rx_period_ms == 20, "rx period");
        Check(config.comm_timeout_ms == 200, "comm timeout");
        Check(config.zero_torque_period_ms == 20, "zero torque period");
    }
    {
        FakeConfigView view;
        view.setLoaded(false);
        Check(loadCanRuntimeConfig(view, config) == Status::NOT_LOADED, "not loaded");
    }
    {
        FakeConfigView view;
        FillValidConfig(view);
        view.setAbsent("can.silence_trigger");
        Check(loadCanRuntimeConfig(view, config) == Status::MISSING, "missing key is MISSING");
    }
    {
        FakeConfigView view;
        FillValidConfig(view);
        view.setInt("can.data_bits", 7);
        Check(loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE, "wrong data bits");
    }
    {
        FakeConfigView view;
        FillValidConfig(view);
        view.setString("can.zero_torque.send_mode", "CLOSED_LOOP");
        Check(loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE, "wrong send mode");
    }
    {
        FakeConfigView view;
        FillValidConfig(view);
        view.setInt("timing.can_io_period_ms", 0);
        Check(loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE, "zero period");
    }
    {
        FakeConfigView view;
        FillValidConfig(view);
        view.setInt("timing.can_comm_timeout_ms", 0);
        Check(loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE, "zero comm timeout");
    }
    {
        FakeConfigView view;
        FillValidConfig(view);
        view.setInt("timing.zero_torque_send_period_ms", 99999);
        Check(loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE, "period too large");
    }
    {
        FakeConfigView view;
        FillValidConfig(view);
        view.addMotor(7, "0x208");
        Check(loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE, "undefined node rejected");
    }
    {
        FakeConfigView view;
        FillValidConfig(view);
        view.addMotor(7, "0x201");
        Check(loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE, "duplicate node rejected");
    }
    {
        FakeConfigView view;
        FillValidConfig(view);
        view.addMotor(2, "0x206");
        Check(loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE, "duplicate motor id");
    }
    // 缺陷 #6：必须恰好 6 个节点，且构成 0x201~0x206 的完整集合
    {
        FakeConfigView view;
        FillValidConfig(view);
        view.clearMotors();
        view.addMotor(1, "0x201");
        Check(loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE,
              "single motor config must be rejected");
    }
    {
        FakeConfigView view;
        FillValidConfig(view);
        view.clearMotors();
        view.addMotor(1, "0x201");
        view.addMotor(2, "0x202");
        view.addMotor(3, "0x203");
        view.addMotor(4, "0x204");
        view.addMotor(5, "0x205");   // 缺少 0x206
        Check(loadCanRuntimeConfig(view, config) == Status::OUT_OF_RANGE,
              "incomplete node set must be rejected");
    }
    {
        // 失败加载不得部分提交：先建立 good 状态，再改坏一个键
        FakeConfigView good;
        FillValidConfig(good);
        CanRuntimeConfig stable{};
        Check(loadCanRuntimeConfig(good, stable) == Status::OK, "stable load");
        const std::size_t before = stable.motor_count;

        FakeConfigView broken;
        FillValidConfig(broken);
        broken.setString("can.start_mode", "CAN_FD");
        Check(loadCanRuntimeConfig(broken, stable) == Status::OUT_OF_RANGE, "broken load rejected");
        Check(stable.motor_count == before, "failed load must not partially commit");
        Check(stable.interface_name == "can0", "previous config preserved");
    }
}

// ---------------------------------------------------------------------------
// 用例：力矩输出门控 + 发送结果（缺陷 #1 / #2）
// ---------------------------------------------------------------------------
void TestTorqueOutputGating()
{
    FakeConfigView view;
    FillValidConfig(view);
    CanRuntimeConfig config{};
    Check(loadCanRuntimeConfig(view, config) == config::ConfigStatus::OK, "load config");

    config.tx_period_ms = 5;
    config.zero_torque_period_ms = 5;
    config.rx_period_ms = 5;

    RecordingTransport transport;
    CanNodeStatusStore store;
    RecordingSink sink;
    RecordingEventSink events;

    CanCore::Dependencies deps{transport, store, sink, &events, nullptr};
    CanCore core(deps);
    std::string detail;
    Check(core.start(config, detail), "core start");

    // 默认必须是 fail-safe 零力矩模式
    Check(core.torqueOutputMode() == can::TorqueOutputMode::ZERO_TORQUE,
          "default torque mode must be ZERO_TORQUE (fail-safe)");

    // 锁存真实控制量：在 ZERO_TORQUE 模式下**不得**出现在总线上
    MotorSlotValues command{1000, -1000, 500, 0};
    Check(core.latchMotorCurrents(0x200U, command), "latch control command");
    Check(!core.latchMotorCurrents(0x201U, command), "reject latch on non-control id");

    transport.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(transport.countNonZero(0x200U) == 0U,
          "zero-torque mode must never emit non-zero control frames");
    Check(transport.countZeroPayload(0x200U) > 0U, "zero-torque mode must emit zero frames");

    // 显式授权进入 CONTROL 模式后，控制量才允许上线
    core.setTorqueOutputMode(can::TorqueOutputMode::CONTROL);
    Check(core.torqueOutputMode() == can::TorqueOutputMode::CONTROL, "mode switched to CONTROL");
    // 切换时必须清零锁存，避免残余控制量立即发出
    MotorSlotValues cleared{};
    Check(core.latchedCurrents(0x200U, cleared), "read latch after switch");
    for (std::size_t i = 0; i < can::kMotorSlotsPerFrame; ++i) {
        Check(cleared[i] == 0, "mode switch must clear latched command");
    }

    Check(core.latchMotorCurrents(0x200U, command), "re-latch command");
    transport.clear();
    bool saw_control = false;
    for (int attempt = 0; attempt < 60 && !saw_control; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (transport.countNonZero(0x200U) > 0U) {
            saw_control = true;
        }
    }
    Check(saw_control, "CONTROL mode must emit non-zero control frames");

    // CONTROL 模式下不得同时发零力矩帧（两者共用同一组 CAN ID，必须互斥）
    transport.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    Check(transport.countZeroPayload(0x200U) == 0U,
          "CONTROL mode must not interleave zero-torque frames");

    // 回到 fail-safe 模式
    core.setTorqueOutputMode(can::TorqueOutputMode::ZERO_TORQUE);
    transport.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    Check(transport.countNonZero(0x200U) == 0U, "back to zero-torque emits only zero frames");

    // 缺陷 #2：发送失败必须回传，不得虚报成功
    core.stop();
    Check(!core.isRunning(), "stopped");
    transport.fail_countdown = 2;
    const CanError failed = core.flushLatchedFrames();
    Check(failed != CanError::OK, "flushLatchedFrames must report send failure");
    Check(events.countKind(CanEventKind::ZERO_TORQUE_FAILED) == 2U,
          "zero-torque failure must publish exactly one event per frame");

    // 成功路径必须返回 OK
    transport.fail_countdown = 0;
    Check(core.flushLatchedFrames() == CanError::OK, "flushLatchedFrames reports success");

    // stop() 幂等
    core.stop();
    core.stop();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <test-case>\n", argv[0]);
        return 1;
    }

    const std::string_view case_name{argv[1]};

    try {
        if (case_name == "can.slot_contract") {
            TestSlotContract();
        } else if (case_name == "can.big_endian_codec") {
            TestBigEndianCodec();
        } else if (case_name == "can.control_frame") {
            TestControlFrame();
        } else if (case_name == "can.zero_torque_frame") {
            TestZeroTorqueFrame();
        } else if (case_name == "can.receive_validation") {
            TestReceiveValidation();
        } else if (case_name == "can.node_state_transitions") {
            TestNodeStateTransitions();
        } else if (case_name == "can.snapshot_semantics") {
            TestSnapshotSemantics();
        } else if (case_name == "can.config_validation") {
            TestConfigValidation();
        } else if (case_name == "can.torque_output_gating") {
            TestTorqueOutputGating();
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
