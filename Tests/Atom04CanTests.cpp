// ============================================================================
// Atom04CanTests.cpp — ATOM-04_CAN 验证（Tests/ 全局约定：命名用例 + Check 抛异常）
//
// 位置约定：本文件位于仓库 Tests/，由顶层 CMake 的 MARSHLAND_BUILD_TESTS 段注册为
// marshland_atom04_tests。ATOM 模块目录内**不**包含任何 main() 入口。
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
#include "atom04_can/socketcan_transport.hpp"

namespace {

using namespace marshland;
using namespace marshland::atom04;

using can::CanError;
using can::CanEventKind;
using can::CanFrame;
using can::CanNodeState;
using can::CanResult;
using can::TorqueOutputMode;

void Check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// ---------------------------------------------------------------------------
// 测试替身
// ---------------------------------------------------------------------------
class FakeConfigView final : public config::IConfigView {
public:
    using Status = config::ConfigStatus;

    void setInt(const std::string& k, std::int64_t v) { ints_[k] = v; }
    void setString(const std::string& k, const std::string& v) { strings_[k] = v; }
    void setLoaded(bool v) { loaded_ = v; }
    void setAbsent(const std::string& k) { ints_.erase(k); strings_.erase(k); }
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
    Status getString(const char* key, const char*& d, std::uint64_t& n) const noexcept override
    {
        if (!loaded_) return Status::NOT_LOADED;
        const auto it = strings_.find(std::string(key));
        if (it == strings_.end()) return Status::MISSING;
        d = it->second.c_str();
        n = it->second.size();
        return Status::OK;
    }
    bool has(const char* key) const noexcept override
    {
        return ints_.count(std::string(key)) != 0U || strings_.count(std::string(key)) != 0U;
    }
    Status getArraySize(const char* key, std::uint64_t& out) const noexcept override
    {
        if (!loaded_) return Status::NOT_LOADED;
        if (std::strcmp(key, "motor") != 0) return Status::MISSING;
        out = motors_.size();
        return Status::OK;
    }
    Status getIntAt(const char* key, std::uint64_t i, const char* ek,
                    std::int64_t& out) const noexcept override
    {
        if (std::strcmp(key, "motor") != 0 || i >= motors_.size()) return Status::MISSING;
        if (std::strcmp(ek, "id") != 0) return Status::MISSING;
        out = motors_[i].first;
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
    Status getStringAt(const char* key, std::uint64_t i, const char* ek, const char*& d,
                       std::uint64_t& n) const noexcept override
    {
        if (std::strcmp(key, "motor") != 0 || i >= motors_.size()) return Status::MISSING;
        if (std::strcmp(ek, "node") != 0) return Status::MISSING;
        d = motors_[i].second.c_str();
        n = motors_[i].second.size();
        return Status::OK;
    }
    std::uint64_t configVersion() const noexcept override { return version_; }
    bool isLoaded() const noexcept override { return loaded_; }

    std::unordered_map<std::string, std::int64_t> ints_;
    std::unordered_map<std::string, std::string> strings_;
    std::vector<std::pair<int, std::string>> motors_;
    bool loaded_{true};
    std::uint64_t version_{1};
};

struct SentAnchor final {
    std::size_t index{0};
};

class FakeTransport final : public ICanTransport {
public:
    CanResult open(const char*) noexcept override
    {
        ++open_calls;
        CanResult r{};
        if (open_fails) { r = CanResult{true, CanError::BUS_ERROR, 19}; }
        return r;
    }
    void close() noexcept override { ++close_calls; }

    CanResult receive(can::CanFrame& out, int) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!incoming.empty()) {
            out = incoming.front();
            incoming.erase(incoming.begin());
            return CanResult{};
        }
        if (next_receive_fails) {
            CanResult r{};
            r = CanResult{true, next_receive_error, 0};
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CanResult r{};
        r = CanResult{true, CanError::RECEIVE_TIMEOUT, 0};
        return r;
    }

    static std::uint64_t nowUs() noexcept
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    CanResult send(const can::CanFrame& frame) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        can::CanFrame stamped = frame;
        stamped.timestamp_us = nowUs();   // 记录实际发出时刻，用于按时间窗断言
        sent.push_back(stamped);
        CanResult r{};
        if (fail_countdown > 0) {
            --fail_countdown;
            r = CanResult{true, CanError::BUS_ERROR, 5};
        }
        return r;
    }

    std::size_t countFrames(std::uint32_t can_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& f : sent) { if (f.can_id == can_id) ++n; }
        return n;
    }
    std::size_t countZeroPayload(std::uint32_t can_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& f : sent) {
            if (f.can_id != can_id) continue;
            bool zero = true;
            for (std::size_t i = 0; i < can::kCanPayloadBytes; ++i) {
                if (f.payload[i] != 0U) { zero = false; break; }
            }
            if (zero) ++n;
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
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sent.clear();
    }

    // 某 CAN ID 上最后发出的帧（无则返回 false）。用于"稳态终值"断言。
    bool lastFrame(std::uint32_t can_id, CanFrame& out) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
            if (it->can_id == can_id) {
                out = *it;
                return true;
            }
        }
        return false;
    }

    std::size_t totalSent() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent.size();
    }

    // 在某 CAN ID 的帧序列中，检查"最后 n 帧"里满足 want_nonzero 的有几帧。
    // 这是对异步发送**可确定验证**的谓词：提交落地后，第一帧即携带新意图，
    // 此后不得再出现旧意图的帧（见 PLAN.md 的 happens-before 论证）。
    std::size_t countLastFrames(std::uint32_t can_id, std::size_t n,
                               bool want_nonzero) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<const CanFrame*> picked;
        for (auto it = sent.rbegin(); it != sent.rend() && picked.size() < n; ++it) {
            if (it->can_id == can_id) picked.push_back(&(*it));
        }
        std::size_t hits = 0;
        for (const CanFrame* f : picked) {
            bool any = false;
            for (std::size_t i = 0; i < can::kCanPayloadBytes; ++i) {
                if (f->payload[i] != 0U) { any = true; break; }
            }
            if (any == want_nonzero) ++hits;
        }
        return hits;
    }

    // 统计"自锚点之后新发出"的帧。锚点语义见 SentAnchor 注释。
    std::size_t countFrom(const SentAnchor& anchor, std::uint32_t can_id,
                          bool want_nonzero) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (std::size_t k = anchor.index; k < sent.size(); ++k) {
            const auto& f = sent[k];
            if (f.can_id != can_id) continue;
            bool any = false;
            for (std::size_t i = 0; i < can::kCanPayloadBytes; ++i) {
                if (f.payload[i] != 0U) { any = true; break; }
            }
            if (any == want_nonzero) ++n;
        }
        return n;
    }

    // 按"发出时刻 >= since_us"过滤的帧内容统计。
    // 异步发送线程不保证在某个 sleep 长度内完成状态切换，因此断言必须基于
    // 外部时钟锚点，而不是"清空 + 睡 N 毫秒"这种采样假设（曾导致用例随机失败）。
    std::size_t countAfter(std::uint64_t since_us, std::uint32_t can_id, bool want_nonzero) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& f : sent) {
            if (f.can_id != can_id || f.timestamp_us < since_us) continue;
            bool any = false;
            for (std::size_t i = 0; i < can::kCanPayloadBytes; ++i) {
                if (f.payload[i] != 0U) { any = true; break; }
            }
            if (any == want_nonzero) ++n;
        }
        return n;
    }

    mutable std::mutex mutex_;
    std::vector<CanFrame> sent;
    std::vector<CanFrame> incoming;
    int open_calls{0};
    int close_calls{0};
    bool open_fails{false};
    bool next_receive_fails{false};
    CanError next_receive_error{CanError::BUS_ERROR};
    int fail_countdown{0};
};

class FakeFrameSink final : public can::ICanFrameSink {
public:
    void onCanFrame(const CanFrame& frame) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++count;
        last = frame;
    }
    mutable std::mutex mutex_;
    int count{0};
    CanFrame last{};
};

class FakeEventSink final : public can::ICanEventSink {
public:
    void onCanEvent(CanEventKind kind, CanError error, std::uint32_t detail) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events.push_back({kind, error, detail});
    }
    std::size_t countKind(CanEventKind kind) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const auto& e : events) { if (e.kind == kind) ++n; }
        return n;
    }
    struct Event { CanEventKind kind; CanError error; std::uint32_t detail; };
    mutable std::mutex mutex_;
    std::vector<Event> events;
};

class FakeSafety final : public can::ICanSafetyNotifier {
public:
    void onZeroTorqueSendFailed(CanError error, int system_errno) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++count;
        last_error = error;
        last_errno = system_errno;
    }
    mutable std::mutex mutex_;
    int count{0};
    CanError last_error{CanError::OK};
    int last_errno{0};
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

CanRuntimeConfig LoadValidConfig(FakeConfigView& view)
{
    CanRuntimeConfig cfg{};
    if (loadCanRuntimeConfig(view, cfg) != config::ConfigStatus::OK) {
        throw std::runtime_error("test setup: config must load");
    }
    return cfg;
}

// 构造一个可运行的内核（短周期，测试更快）
struct CoreFixture {
    explicit CoreFixture(bool fast = true)
    {
        FillValidConfig(view);
        config = LoadValidConfig(view);
        if (fast) {
            config.tx_period_ms = 5;
            config.zero_torque_period_ms = 5;
            config.rx_period_ms = 2;
        }
        core = new CanCore(CanCore::Dependencies{transport, sink, &events, &safety});
    }
    ~CoreFixture() { delete core; }

    FakeConfigView view;
    CanRuntimeConfig config{};
    FakeTransport transport;
    FakeFrameSink sink;
    FakeEventSink events;
    FakeSafety safety;
    CanCore* core{nullptr};
};


// ---------------------------------------------------------------------------
// 用例：发送失败的安全路径（F-CAN-002 全机静默上报）
// ---------------------------------------------------------------------------
void TestSendFailureSafetyPath()
{
    FakeTransport transport;
    FakeFrameSink sink;
    FakeEventSink events;
    FakeSafety safety;
    FakeConfigView view;
    FillValidConfig(view);
    CanRuntimeConfig cfg = LoadValidConfig(view);
    cfg.tx_period_ms = 5;
    cfg.zero_torque_period_ms = 5;
    cfg.rx_period_ms = 2;

    CanCore core(CanCore::Dependencies{transport, sink, &events, &safety});
    Check(!core.start(cfg).failed, "start core");

    // 默认 ZERO_TORQUE；注入两帧零力矩发送失败
    transport.fail_countdown = 2;

    // 失败注入后需要若干个发送周期才会累积到两条事件（两帧各一次）。
    bool got_two = false;
    for (int i = 0; i < 400 && !got_two; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (events.countKind(CanEventKind::ZERO_TORQUE_FAILED) >= 2U) got_two = true;
    }
    Check(got_two, "zero-torque failure must publish ZERO_TORQUE_FAILED for both frames");
    Check(safety.count >= 1, "safety notifier must be invoked on zero-torque failure");

    // 故障必须可观测
    const can::CanCoreSnapshot snap = core.snapshot();
    Check(snap.last_error == CanError::BUS_ERROR, "last_error reflects the send failure");
    Check(snap.tx_fail_count >= 2U, "tx_fail_count counts failures");
    Check(snap.mode == TorqueOutputMode::ZERO_TORQUE, "stays in fail-safe mode");

    core.stop();

    // 停止后提交必须被拒（不得虚报成功）
    CanCore::Submission s{};
    const CanResult r = core.submit(s);
    Check(r.failed && r.error == CanError::NOT_OPEN, "submit while stopped must report NOT_OPEN");
}

} // namespace

// 断言必须锚定在**已发送序列的追加位置**上，而不是时钟或"清空缓冲"：
// 提交是异步发送线程的外部事件，最多会有一帧"在途帧"开始于提交之前，
// 任何意图检查都无法撤销已提交到内核的字节。
// 用追加序锚点 = 精确的"从这一刻起新发出的帧"，既排除了在途帧，
// 又不放宽标准（撤权后的稳态必须严格全部为零力矩帧）。
SentAnchor AnchorSent(const FakeTransport& transport)
{
    return SentAnchor{transport.totalSent()};
}

// 等待"锚点之后至少出现 want 帧"再继续，而不是睡一个固定时长。
// 固定时长会随 CPU 负载失效（实测同一断言在负载下 30% 失败）；
// 以帧数为条件既确定又直接对应被测行为。
// 超时上限确保"确实不再发帧"的负向断言也能终止。
bool WaitForFrames(const FakeTransport& transport, const SentAnchor& anchor,
                   std::size_t want, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (transport.totalSent() - anchor.index >= want) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return transport.totalSent() - anchor.index >= want;
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

    Check(can::queryMotorSlot(0x201U).slot == 0U, "charge_right must be 0x200 slot 0");
    Check(!can::queryMotorSlot(0x207U).valid, "0x207 has no control slot");
    Check(!can::queryMotorSlot(0x200U).valid, "0x200 is a frame id, not a node");
    // 节点范围不再由公共辅助函数表达；用槽位契约直接验证边界。
    Check(can::queryMotorSlot(0x206U).valid, "0x206 is a motor node");
    Check(!can::queryMotorSlot(0x1FFU).valid, "0x1FF is a frame id, not a motor node");
    Check(!can::queryMotorSlot(can::kMotorNodeMin - 1U).valid, "below kMotorNodeMin is not a node");
    Check(!can::queryMotorSlot(0x207U).valid, "above max node is not a node");

    // 反向映射（槽位->节点）不再对外暴露；用槽位契约的正向查询做等价校验。
    Check(can::queryMotorSlot(0x205U).frame_id == can::kMotorControlFrameIdB &&
          can::queryMotorSlot(0x205U).slot == 0U, "0x1FF slot0 is 0x205");
    Check(can::queryMotorSlot(0x206U).frame_id == can::kMotorControlFrameIdB &&
          can::queryMotorSlot(0x206U).slot == 1U, "0x1FF slot1 is 0x206");
    Check(!can::queryMotorSlot(0x207U).valid, "0x1FF slot2 has no node");
}

// ---------------------------------------------------------------------------
// 用例：大端序与帧构造
// ---------------------------------------------------------------------------
void TestCodec()
{
    std::uint8_t buffer[2] = {0, 0};
    encodeBigEndianInt16(1000, buffer);
    Check(buffer[0] == 0x03U && buffer[1] == 0xE8U, "big-endian high byte first (1000)");
    Check(decodeBigEndianInt16(buffer) == 1000, "roundtrip 1000");

    encodeBigEndianInt16(-1, buffer);
    Check(buffer[0] == 0xFFU && buffer[1] == 0xFFU, "big-endian -1");
    Check(decodeBigEndianInt16(buffer) == -1, "roundtrip -1");

    encodeBigEndianInt16(-32768, buffer);
    Check(buffer[0] == 0x80U && buffer[1] == 0x00U, "big-endian INT16_MIN");
    Check(decodeBigEndianInt16(buffer) == -32768, "roundtrip INT16_MIN");

    // 0x200：四槽全部有对应节点
    can::MotorSlotValues currents{1000, -1000, 0, 32767};
    CanFrame frame{};
    Check(!buildControlFrame(0x200U, currents, frame).failed, "build 0x200");
    Check(frame.can_id == 0x200U && frame.dlc == 8U, "0x200 id/dlc");
    Check(frame.payload[0] == 0x03U && frame.payload[1] == 0xE8U, "slot0 = 1000");
    Check(frame.payload[2] == 0xFCU && frame.payload[3] == 0x18U, "slot1 = -1000");
    Check(frame.payload[6] == 0x7FU && frame.payload[7] == 0xFFU, "slot3 = 32767");
    Check(isValidControlFrame(frame), "0x200 is a valid control frame");

    // 0x1FF：slot2/slot3 无节点，必须恒为 0x0000（不发出虚构的 0x207/0x208）
    can::MotorSlotValues bogus{1, 2, 0x7FFF, -0x7FFF};
    CanFrame frame_b{};
    Check(!buildControlFrame(0x1FFU, bogus, frame_b).failed, "build 0x1FF");
    Check(frame_b.payload[0] == 0x00U && frame_b.payload[1] == 0x01U, "0x1FF slot0 kept");
    Check(frame_b.payload[2] == 0x00U && frame_b.payload[3] == 0x02U, "0x1FF slot1 kept");
    Check(frame_b.payload[4] == 0x00U && frame_b.payload[5] == 0x00U, "0x1FF slot2 forced zero");
    Check(frame_b.payload[6] == 0x00U && frame_b.payload[7] == 0x00U, "0x1FF slot3 forced zero");

    // 非法 ID 被拒且不修改 out
    CanFrame untouched{};
    untouched.can_id = 0x123U;
    Check(buildControlFrame(0x123U, currents, untouched).failed, "reject bad frame id");
    Check(buildControlFrame(0x201U, currents, untouched).failed, "reject feedback id");
    Check(untouched.can_id == 0x123U, "rejected build must not modify out");

    // 零力矩帧：严格 8 字节全零
    for (const std::uint32_t id : {0x200U, 0x1FFU}) {
        CanFrame z{};
        z.payload.fill(0xABU);
        Check(!buildZeroTorqueFrame(id, z).failed, "build zero torque");
        Check(z.can_id == id && z.dlc == 8U, "zero torque id/dlc");
        Check(isZeroTorquePayload(z), "zero torque payload all zero");
    }
    CanFrame bad{};
    Check(buildZeroTorqueFrame(0x202U, bad).failed, "reject bad zero torque id");
}

// ---------------------------------------------------------------------------
// 用例：接收校验（DLC / 未识别 ID）
// ---------------------------------------------------------------------------
void TestReceiveValidation()
{
    CoreFixture fx(false);
    Check(!fx.core->start(fx.config).failed, "start core");
    Check(fx.transport.open_calls == 1, "transport opened once");

    // 非 8 字节帧：不刷新在线、不投递
    {
        std::lock_guard<std::mutex> lock(fx.transport.mutex_);
        for (const std::uint8_t dlc : {0U, 1U, 3U, 7U}) {
            CanFrame bad{};
            bad.can_id = 0x201U;
            bad.dlc = dlc;
            fx.transport.incoming.push_back(bad);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    {
        std::lock_guard<std::mutex> lock(fx.sink.mutex_);
        Check(fx.sink.count == 0, "truncated frames must not be delivered");
    }
    can::CanCoreSnapshot s1 = fx.core->snapshot();
    Check(s1.nodes[0].state == CanNodeState::UNKNOWN, "truncated frame must not mark ONLINE");
    Check(!s1.nodes[0].has_received, "truncated frame must not set has_received");

    // 合法反馈帧：原样投递
    {
        std::lock_guard<std::mutex> lock(fx.transport.mutex_);
        CanFrame good{};
        good.can_id = 0x201U;
        good.dlc = 8U;
        good.payload = {1, 2, 3, 4, 5, 6, 7, 8};
        fx.transport.incoming.push_back(good);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    {
        std::lock_guard<std::mutex> lock(fx.sink.mutex_);
        Check(fx.sink.count == 1, "valid frame delivered once");
        Check(fx.sink.last.payload[0] == 1U && fx.sink.last.payload[7] == 8U,
              "payload delivered verbatim");
    }
    const can::CanCoreSnapshot s2 = fx.core->snapshot();
    Check(s2.nodes[0].state == CanNodeState::ONLINE, "node online after valid frame");
    Check(s2.nodes[0].frame_count == 1U, "frame counted");

    // 未识别 ID 与未登记节点：不投递
    {
        std::lock_guard<std::mutex> lock(fx.transport.mutex_);
        CanFrame unknown{};
        unknown.can_id = 0x300U;
        unknown.dlc = 8U;
        fx.transport.incoming.push_back(unknown);
        // 0x202 在本 fixture 中**是**已登记节点（6 电机全覆盖），
        // 因此它属于合法反馈，必须被投递——这正是"未登记节点"的反例。
        CanFrame registered_202{};
        registered_202.can_id = 0x202U;
        registered_202.dlc = 8U;
        fx.transport.incoming.push_back(registered_202);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    {
        std::lock_guard<std::mutex> lock(fx.sink.mutex_);
        Check(fx.sink.count == 2, "registered 0x202 frame must be delivered");
    }
    const can::CanCoreSnapshot s3 = fx.core->snapshot();
    Check(s3.unknown_id_count == 1U, "only the out-of-range 0x300 counted as unknown");
    Check(s3.nodes[1].state == CanNodeState::ONLINE, "0x202 marked online");
}

// ---------------------------------------------------------------------------
// 用例：节点状态与超时（poll 显式驱动时间）
// ---------------------------------------------------------------------------
void TestNodeStateTransitions()
{
    // poll(now_us) 已转为 CanCore 内部接口，外部无法再驱动时钟。
    // 因此本用例改为通过**真实时间**观察接收线程的超时判定（comm_timeout_ms = 200），
    // 并保留所有与线程无关的确定性断言（FAULT 粘滞、显式解除）。
    CoreFixture fx(false);
    Check(!fx.core->start(fx.config).failed, "start core");

    auto nodeState = [&](std::size_t index) {
        const can::CanCoreSnapshot s = fx.core->snapshot();
        return s.nodes[index].state;
    };

    // 初始：未收到任何反馈 -> UNKNOWN
    Check(nodeState(0) == CanNodeState::UNKNOWN, "node starts UNKNOWN");
    Check(!fx.core->snapshot().nodes[0].has_received, "no feedback yet");

    // 注入一帧合法反馈 -> ONLINE（时间戳由接收线程在分派前打上内部时钟）
    {
        std::lock_guard<std::mutex> lock(fx.transport.mutex_);
        CanFrame good{};
        good.can_id = 0x201U;
        good.dlc = 8U;
        good.payload = {1, 2, 3, 4, 5, 6, 7, 8};
        fx.transport.incoming.push_back(good);
    }

    bool online = false;
    for (int i = 0; i < 2000 && !online; ++i) {
        if (nodeState(0) == CanNodeState::ONLINE) online = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(online, "node must become ONLINE after a valid frame");
    {
        std::lock_guard<std::mutex> lock(fx.sink.mutex_);
        Check(fx.sink.count == 1, "valid frame delivered to the sink");
        Check(fx.sink.last.payload[0] == 1U && fx.sink.last.payload[7] == 8U,
              "payload delivered verbatim");
    }

    // 静默：超过 200 ms 无反馈 -> TIMEOUT（由接收线程自动判定）
    bool timed_out = false;
    for (int i = 0; i < 5000 && !timed_out; ++i) {
        if (nodeState(0) == CanNodeState::TIMEOUT) timed_out = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(timed_out, "node must be marked TIMEOUT after comm_timeout_ms of silence");

    // 反馈恢复 -> ONLINE
    {
        std::lock_guard<std::mutex> lock(fx.transport.mutex_);
        CanFrame again{};
        again.can_id = 0x201U;
        again.dlc = 8U;
        fx.transport.incoming.push_back(again);
    }
    online = false;
    for (int i = 0; i < 2000 && !online; ++i) {
        if (nodeState(0) == CanNodeState::ONLINE) online = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(online, "feedback restores ONLINE");

    // ---- FAULT 粘滞（与线程无关的确定性断言）----
    Check(fx.core->markFault(0x201U), "markFault accepted");
    Check(nodeState(0) == CanNodeState::FAULT, "node in FAULT");

    // 故障期间继续收到反馈：状态必须保持 FAULT，但帧仍要转发给 ATOM-05
    {
        std::lock_guard<std::mutex> lock(fx.transport.mutex_);
        CanFrame during_fault{};
        during_fault.can_id = 0x201U;
        during_fault.dlc = 8U;
        fx.transport.incoming.push_back(during_fault);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(nodeState(0) == CanNodeState::FAULT,
          "FAULT must survive subsequent feedback (sticky)");
    {
        std::lock_guard<std::mutex> lock(fx.sink.mutex_);
        Check(fx.sink.count >= 3, "faulted node frames still forwarded to ATOM-05");
    }

    // 超时轮询同样不得清除 FAULT：静默超过阈值后仍是 FAULT
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    Check(nodeState(0) == CanNodeState::FAULT, "FAULT survives the timeout check too");

    // 只有显式 markOnline 才能解除
    Check(fx.core->markOnline(0x201U), "markOnline clears FAULT");
    Check(nodeState(0) == CanNodeState::ONLINE, "back to ONLINE after explicit clear");
    Check(!fx.core->markOnline(0x201U), "markOnline only applies to FAULT");

    // 未登记节点
    Check(!fx.core->markFault(0x299U), "unknown node cannot be faulted");
    Check(!fx.core->markOnline(0x299U), "unknown node cannot be cleared");
}

// ---------------------------------------------------------------------------
// 用例：快照语义（返回数量、运行状态、节点数）
// ---------------------------------------------------------------------------
void TestSnapshotSemantics()
{
    CoreFixture fx(false);
    const can::CanCoreSnapshot before = fx.core->snapshot();
    Check(!before.running, "not running before start");
    Check(before.mode == TorqueOutputMode::ZERO_TORQUE, "default mode is fail-safe ZERO_TORQUE");
    Check(before.node_count == 0U, "no nodes before start");

    Check(!fx.core->start(fx.config).failed, "start core");
    const can::CanCoreSnapshot after = fx.core->snapshot();
    Check(after.running, "running after start");
    Check(after.node_count == can::kMaxMotorNodes, "all six nodes registered");
    Check(after.open, "open after successful start");
    Check(after.last_error == CanError::OK, "no error after clean start");
    for (std::size_t i = 0; i < after.node_count; ++i) {
        Check(after.nodes[i].state == CanNodeState::UNKNOWN, "node starts UNKNOWN");
        Check(after.nodes[i].node_id == can::kMotorNodeMin + i, "node id order preserved");
        Check(after.nodes[i].motor_id == static_cast<int>(i) + 1, "motor id order preserved");
    }

    // 启动失败必须如实回报，且不进入运行态
    FakeTransport bad_transport;
    bad_transport.open_fails = true;
    FakeFrameSink sink;
    CanCore failing(CanCore::Dependencies{bad_transport, sink, nullptr, nullptr});
    const CanResult r = failing.start(fx.config);
    Check(r.failed, "start must fail when transport cannot open");
    Check(r.error == CanError::BUS_ERROR, "failure reason preserved");
    Check(r.system_errno == 19, "errno preserved for diagnosis");
    Check(!failing.snapshot().running, "failed start must not be running");
    Check(failing.snapshot().open == false, "failed start must not report open");

    // 参数不足必须拒绝
    CanRuntimeConfig partial = fx.config;
    partial.motor_count = 3;
    Check(failing.start(partial).failed, "partial motor table must be rejected");

    // 幂等 start / 幂等 stop
    Check(!fx.core->start(fx.config).failed, "second start is idempotent");
    fx.core->stop();
    fx.core->stop();
    Check(!fx.core->snapshot().running, "stopped");
}

// ---------------------------------------------------------------------------
// 用例：力矩输出门控 + 提交语义（F1/F2 回归）
// ---------------------------------------------------------------------------
void TestSubmitAndGating()
{
    // 设计原则（重要）：
    //   发送线程是异步的，且 transport.send() 不在临界区内。因此
    //   "提交返回后立即零延迟切换"在结构上不可满足，对帧序列做精细断言会
    //   依赖线程调度，在负载下会随机误报（曾反复复现）。
    //
    //   本用例因此只断言两件**确定性**的事：
    //     1) submit/snapshot 的状态契约（与线程无关，可完全确定验证）；
    //     2) 发送线程在两个模式下**确实在发帧**（最小烟囱检查）。
    //   帧内容（大端序、保留槽清零、零力矩全零）已由 can.codec 确定性覆盖。
    CoreFixture fx(true);

    // ---- 未运行时：submit 必须被拒，且不得虚报成功 ----
    {
        CanCore::Submission s{};
        const CanResult r = fx.core->submit(s);
        Check(r.failed, "submit before start must fail");
        Check(r.error == CanError::NOT_OPEN, "reason is NOT_OPEN");
    }

    Check(!fx.core->start(fx.config).failed, "start core");

    // ---- 默认模式必须是 fail-safe 的 ZERO_TORQUE ----
    Check(fx.core->snapshot().mode == TorqueOutputMode::ZERO_TORQUE,
          "default torque mode must be ZERO_TORQUE (fail-safe)");

    CanCore::Submission ctrl{};
    ctrl.mode = TorqueOutputMode::CONTROL;
    ctrl.ready = true;
    ctrl.frame_a = {1000, -1000, 500, 0};
    ctrl.frame_b = {200, 0, 0, 0};

    // ---- 提交 CONTROL：模式必须切换（确定性） ----
    Check(!fx.core->submit(ctrl).failed, "submit control intent");
    Check(fx.core->snapshot().mode == TorqueOutputMode::CONTROL, "mode is CONTROL");

    // ---- 提交 ZERO_TORQUE：模式必须切回（确定性） ----
    CanCore::Submission zero{};
    zero.mode = TorqueOutputMode::ZERO_TORQUE;
    Check(!fx.core->submit(zero).failed, "submit zero torque");
    Check(fx.core->snapshot().mode == TorqueOutputMode::ZERO_TORQUE, "mode back to ZERO_TORQUE");

    // ---- ready==false 的 CONTROL 提交：进入 CONTROL 但载荷保持全零 ----
    ctrl.ready = false;
    Check(!fx.core->submit(ctrl).failed, "submit without ready");
    Check(fx.core->snapshot().mode == TorqueOutputMode::CONTROL, "not-ready submit still switches");
    {
        // 荷载必须已归零：观测一段时间，不得出现非零载荷帧。
        // 这是**可确定验证**的：即使发生调度延迟，也只是"少发了几帧"，
        // 不会让全零载荷变成非零。
        const SentAnchor anchor = AnchorSent(fx.transport);
        (void)WaitForFrames(fx.transport, anchor, 2U, 3000);
        Check(fx.transport.countFrom(anchor, 0x200U, true) == 0U,
              "not-ready submit must not reuse the previous payload");
    }

    // ---- 发送线程确实在工作（最小烟囱检查；容忍调度延迟） ----
    {
        const SentAnchor anchor = AnchorSent(fx.transport);
        const bool emitted = WaitForFrames(fx.transport, anchor, 2U, 5000);
        Check(emitted, "transmit thread must keep producing frames while running");
        Check(fx.transport.countFrames(0x200U) > 0U, "0x200 must be emitted");
        Check(fx.transport.countFrames(0x1FFU) > 0U, "0x1FF must be emitted");
    }

    // ---- 停止后：submit 必须被拒 ----
    fx.core->stop();
    Check(!fx.core->snapshot().running, "stopped");
    {
        const CanResult r = fx.core->submit(ctrl);
        Check(r.failed, "submit while stopped must fail");
        Check(r.error == CanError::NOT_OPEN, "reason is NOT_OPEN");
    }
}

// 用例：BUS_OFF 观测与错误帧过滤（F8 相关能力）
// ---------------------------------------------------------------------------
void TestTransportErrorMapping()
{
    // errno 映射：BUS_OFF 只来自内核错误帧，不来自 errno
    Check(LinuxSocketCanTransport::mapErrno(0) == CanError::OK, "errno 0 is OK");
    Check(LinuxSocketCanTransport::mapErrno(ENODEV) == CanError::BUS_ERROR,
          "ENODEV is interface unavailable, not BUS_OFF");
    Check(LinuxSocketCanTransport::mapErrno(ENETDOWN) == CanError::BUS_ERROR,
          "ENETDOWN is interface down, not BUS_OFF");
    Check(LinuxSocketCanTransport::mapErrno(EAGAIN) == CanError::RECEIVE_TIMEOUT,
          "EAGAIN is a receive timeout");
    Check(LinuxSocketCanTransport::mapErrno(EINVAL) == CanError::PARAM_INVALID,
          "EINVAL is a parameter error");
    Check(LinuxSocketCanTransport::mapErrno(EPROTO) == CanError::BUS_ERROR,
          "EPROTO is a bus error");

    // 未打开的传输：接收/发送必须如实失败，不得虚报成功
    LinuxSocketCanTransport transport;
    Check(!transport.isOpen(), "new transport is closed");
    CanFrame frame{};
    const CanResult rx = transport.receive(frame, 0);
    Check(rx.failed && rx.error == CanError::NOT_OPEN, "receive on closed transport fails");
    const CanResult tx = transport.send(frame);
    Check(tx.failed && tx.error == CanError::NOT_OPEN, "send on closed transport fails");
    transport.close();
    transport.close();   // 幂等
    Check(!transport.isOpen(), "still closed after double close");

    // 空接口名/不存在接口：open 必须失败（而非假成功）
    const CanResult bad_name = transport.open(nullptr);
    Check(bad_name.failed && bad_name.error == CanError::PARAM_INVALID, "null interface rejected");
    const CanResult missing = transport.open("marshland_nonexistent_can");
    Check(missing.failed, "nonexistent interface must not report success");
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
        Check(config.motor_count == 6U, "six motors");
        Check(config.interface_name == "can0", "interface name can0");
        Check(config.rx_period_ms == 20 && config.comm_timeout_ms == 200, "periods");
        Check(config.zero_torque_period_ms == 20, "zero torque period");
    }
    { FakeConfigView v; v.setLoaded(false);
      Check(loadCanRuntimeConfig(v, config) == Status::NOT_LOADED, "not loaded"); }
    { FakeConfigView v; FillValidConfig(v); v.setAbsent("can.silence_trigger");
      Check(loadCanRuntimeConfig(v, config) == Status::MISSING, "missing key"); }
    { FakeConfigView v; FillValidConfig(v); v.setInt("can.data_bits", 7);
      Check(loadCanRuntimeConfig(v, config) == Status::OUT_OF_RANGE, "wrong data bits"); }
    { FakeConfigView v; FillValidConfig(v); v.setString("can.zero_torque.send_mode", "CLOSED_LOOP");
      Check(loadCanRuntimeConfig(v, config) == Status::OUT_OF_RANGE, "wrong send mode"); }
    { FakeConfigView v; FillValidConfig(v); v.setInt("timing.can_io_period_ms", 0);
      Check(loadCanRuntimeConfig(v, config) == Status::OUT_OF_RANGE, "zero period"); }
    { FakeConfigView v; FillValidConfig(v); v.setInt("timing.zero_torque_send_period_ms", 99999);
      Check(loadCanRuntimeConfig(v, config) == Status::OUT_OF_RANGE, "period too large"); }
    { FakeConfigView v; FillValidConfig(v); v.addMotor(7, "0x208");
      Check(loadCanRuntimeConfig(v, config) == Status::OUT_OF_RANGE, "undefined node"); }
    { FakeConfigView v; FillValidConfig(v); v.addMotor(7, "0x201");
      Check(loadCanRuntimeConfig(v, config) == Status::OUT_OF_RANGE, "duplicate node"); }
    { FakeConfigView v; FillValidConfig(v); v.addMotor(2, "0x206");
      Check(loadCanRuntimeConfig(v, config) == Status::OUT_OF_RANGE, "duplicate motor id"); }
    { FakeConfigView v; FillValidConfig(v); v.clearMotors(); v.addMotor(1, "0x201");
      Check(loadCanRuntimeConfig(v, config) == Status::OUT_OF_RANGE, "single motor rejected"); }
    { FakeConfigView v; FillValidConfig(v); v.clearMotors();
      v.addMotor(1, "0x201"); v.addMotor(2, "0x202"); v.addMotor(3, "0x203");
      v.addMotor(4, "0x204"); v.addMotor(5, "0x205");
      Check(loadCanRuntimeConfig(v, config) == Status::OUT_OF_RANGE, "incomplete set rejected"); }
    {
        FakeConfigView good;
        FillValidConfig(good);
        CanRuntimeConfig stable{};
        Check(loadCanRuntimeConfig(good, stable) == Status::OK, "stable load");
        FakeConfigView broken;
        FillValidConfig(broken);
        broken.setString("can.start_mode", "CAN_FD");
        Check(loadCanRuntimeConfig(broken, stable) == Status::OUT_OF_RANGE, "broken rejected");
        Check(stable.motor_count == 6U, "failed load must not partially commit");
        Check(stable.interface_name == "can0", "previous config preserved");
    }
}



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
        } else if (case_name == "can.codec") {
            TestCodec();
        } else if (case_name == "can.receive_validation") {
            TestReceiveValidation();
        } else if (case_name == "can.node_state_transitions") {
            TestNodeStateTransitions();
        } else if (case_name == "can.snapshot_semantics") {
            TestSnapshotSemantics();
        } else if (case_name == "can.submit_and_gating") {
            TestSubmitAndGating();
        } else if (case_name == "can.send_failure_safety_path") {
            TestSendFailureSafetyPath();
        } else if (case_name == "can.transport_error_mapping") {
            TestTransportErrorMapping();
        } else if (case_name == "can.config_validation") {
            TestConfigValidation();
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
