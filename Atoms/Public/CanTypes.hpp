// ============================================================================
// CanTypes.hpp — CAN/SocketCAN 跨 ATOM 公共契约类型
//
// 归属说明：
//   本文件是**跨模块契约**，与 MarshlandTypes.hpp / ILifecycle.hpp /
//   StateArbitrationTypes.hpp / SbusTypes.hpp 同级，置于 Atoms/Public/。
//   由 ATOM-04_CAN 产生，由 ATOM-05（电机反馈）与装配层消费。
//
// 权威来源：
//   Design/Config.md       §5.1（电机映射）、§6.1（SocketCAN）、§6.2（报文接口表）、
//                          §6.3（零力矩）、§14.2（F-CAN-001 / F-CAN-002）
//   Design/Atomic/ATOM-04_CAN.md  电机 CAN 帧语义 / 零力矩安全帧 / 验收条件
//   Config/dart_control.json      motor[].id / motor[].node、can.zero_torque
//
// 依赖方向（不可协商）：
//   仅依赖 C++ 标准库。禁止包含 AppCoordinator.hpp、任何 ATOM 私有头。
//
// 不作为：
//   * 不定义反馈帧字段布局、单位或错误码映射（归 ATOM-05，见 PLAN.md 待接入点）；
//   * 不定义状态机、控制权仲裁、PID 或机构流程语义；
//   * 本文件不是 SocketCAN 的平台头，禁止泄漏 <linux/can.h>。
// ============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace marshland::can {

// ---------------------------------------------------------------------------
// 帧常量（Design/Config.md §6.2 报文接口表）
//
// 两组电机控制帧：
//   0x200  -> node 0x201、0x202、0x203、0x204（电机 id 1..4）
//   0x1FF  -> node 0x205、0x206（电机 id 5、6），其余两个槽位未使用
// 多字节量一律大端序，每个 int16 先写高字节。
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kMotorControlFrameIdA = 0x200U;
inline constexpr std::uint32_t kMotorControlFrameIdB = 0x1FFU;
inline constexpr std::uint8_t kMotorControlDlc = 8U;
inline constexpr std::size_t kCanPayloadBytes = 8U;
inline constexpr std::size_t kMotorSlotsPerFrame = 4U;
inline constexpr std::size_t kMotorControlFrameCount = 2U;

// 电机控制帧承载的节点区间（Design/Config.md §6.2 第 256 行明确 YAW/变力位于
// 0x1FF 帧槽位 0、1；反馈节点区间为 0x201~0x206）。
inline constexpr std::uint32_t kFrameANodeBase = 0x201U;
inline constexpr std::uint32_t kFrameBNodeBase = 0x205U;
inline constexpr std::uint32_t kMotorNodeMin = 0x201U;
inline constexpr std::uint32_t kMotorNodeMax = 0x206U;
inline constexpr std::size_t kMaxMotorNodes = 6U;

// ---------------------------------------------------------------------------
// 单帧表示（can_frame 的与平台无关镜像）
//
// can_id 保存去掉 CAN_EFF_FLAG / CAN_RTR_FLAG / CAN_ERR_FLAG 后的标识符，
// 以便公共头不依赖 SocketCAN 常量。
// ---------------------------------------------------------------------------
struct CanFrame final {
    std::uint32_t can_id{0};
    std::uint8_t dlc{0};
    std::array<std::uint8_t, kCanPayloadBytes> payload{};
    std::uint64_t timestamp_us{0};
};

// ---------------------------------------------------------------------------
// 错误模型（Design/Atomic/ATOM-04_CAN.md 验收条件：能区分成功、超时、
// BUS_OFF 和总线错误；发送失败不得虚报成功）
// ---------------------------------------------------------------------------
enum class CanError : std::uint8_t {
    OK = 0,
    NOT_OPEN,          // 传输未打开
    PARAM_INVALID,     // 入参非法（ID/DLC/电流/接口名）
    SEND_FAILED,       // 发送失败
    SEND_INCOMPLETE,   // 内核未确认完整写入
    RECEIVE_TIMEOUT,   // 读超时，无帧
    BUS_OFF,           // 总线关闭
    BUS_ERROR,         // 总线错误（含未知 errno）
    FRAME_INVALID,     // 帧长度/结构非法
    UNKNOWN_ID,        // 未识别 CAN ID（不臆造字段）
    UNSUPPORTED,       // 当前平台不支持
};

inline const char* toString(CanError error) noexcept
{
    switch (error) {
    case CanError::OK:               return "OK";
    case CanError::NOT_OPEN:         return "NOT_OPEN";
    case CanError::PARAM_INVALID:    return "PARAM_INVALID";
    case CanError::SEND_FAILED:      return "SEND_FAILED";
    case CanError::SEND_INCOMPLETE:  return "SEND_INCOMPLETE";
    case CanError::RECEIVE_TIMEOUT:  return "RECEIVE_TIMEOUT";
    case CanError::BUS_OFF:          return "BUS_OFF";
    case CanError::BUS_ERROR:        return "BUS_ERROR";
    case CanError::FRAME_INVALID:    return "FRAME_INVALID";
    case CanError::UNKNOWN_ID:       return "UNKNOWN_ID";
    case CanError::UNSUPPORTED:      return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

// 带 errno 的错误结果：Bus 错误必须可定位（适配器 error_message 要求）。
struct CanIoResult final {
    CanError error{CanError::OK};
    int system_errno{0};

    constexpr explicit operator bool() const noexcept { return error == CanError::OK; }
};

// ---------------------------------------------------------------------------
// 节点状态（Design/Config.md §5.2 离线判定、§6.2 心跳与错误状态）
//
// 注意：状态**不承载反馈字段**。ATOM-04 只维护"是否按 200 ms 收到该节点帧"，
// 反馈字段解析归 ATOM-05。
// ---------------------------------------------------------------------------
// 状态语义（必须与实现一致）：
//   UNKNOWN  初始化后尚未收到任何反馈
//   ONLINE   已收到反馈且在 timing.can_comm_timeout_ms 内持续收到
//   TIMEOUT  曾在线但超过阈值未收到反馈
//   FAULT    **粘滞终态**：设备上报/上层判定的故障。普通反馈或超时轮询都不得
//            把它改回 ONLINE；只有重新初始化（registerNodes）或显式 markOnline
//            才能解除，以保证"故障清除需经授权流程"这一不变量。
enum class CanNodeState : std::uint8_t {
    UNKNOWN = 0,
    ONLINE,
    TIMEOUT,
    FAULT,
};

inline const char* toString(CanNodeState state) noexcept
{
    switch (state) {
    case CanNodeState::UNKNOWN: return "UNKNOWN";
    case CanNodeState::ONLINE:  return "ONLINE";
    case CanNodeState::TIMEOUT: return "TIMEOUT";
    case CanNodeState::FAULT:   return "FAULT";
    }
    return "UNKNOWN";
}

struct CanNodeStatus final {
    int motor_id{0};
    std::uint32_t node_id{0};
    CanNodeState state{CanNodeState::UNKNOWN};
    // 显式"是否曾收到过反馈"标志：超时判定不得依赖 last_frame_time_us == 0，
    // 否则时钟起点附近的真实帧会被误判（见 PLAN.md 缺陷 D5）。
    bool has_received{false};
    std::uint64_t last_frame_time_us{0};
    std::uint64_t frame_count{0};
};

// 总线状态快照（与节点状态分开，便于诊断）
//
// 分工：rx/tx/tx_fail 与 last_error/last_errno 由传输层维护；
// unknown_id_count 由 ATOM-04 内核（投递层）维护——传输层不识别 CAN ID 语义，
// 因此不在此处由传输层置零掩盖（见 PLAN.md 缺陷 D4）。
struct CanBusStatus final {
    bool open{false};
    CanError last_error{CanError::OK};
    int last_errno{0};
    std::uint64_t rx_count{0};
    std::uint64_t tx_count{0};
    std::uint64_t tx_fail_count{0};
    std::uint64_t unknown_id_count{0};
};

enum class CanEventKind : std::uint8_t {
    BUS_ERROR = 0,
    NODE_STATE_CHANGE,
    ZERO_TORQUE_FAILED,
};

inline const char* toString(CanEventKind kind) noexcept
{
    switch (kind) {
    case CanEventKind::BUS_ERROR:          return "BUS_ERROR";
    case CanEventKind::NODE_STATE_CHANGE:  return "NODE_STATE_CHANGE";
    case CanEventKind::ZERO_TORQUE_FAILED: return "ZERO_TORQUE_FAILED";
    }
    return "UNKNOWN";
}

struct CanEvent final {
    CanEventKind kind{CanEventKind::BUS_ERROR};
    CanError error{CanError::OK};
    int motor_id{0};
    std::uint32_t node_id{0};
    int system_errno{0};
    std::uint64_t timestamp_us{0};
};

// ---------------------------------------------------------------------------
// 节点状态仓储：互斥锁保护的整快照替换 / 复制读取
//
// 与 SbusChannelStore 同构：读取方只能读到完整旧快照或完整新快照，
// 不会读到半更新状态。
// ---------------------------------------------------------------------------
class CanNodeStatusStore final {
public:
    CanNodeStatusStore() = default;
    CanNodeStatusStore(const CanNodeStatusStore&) = delete;
    CanNodeStatusStore& operator=(const CanNodeStatusStore&) = delete;
    ~CanNodeStatusStore() = default;

    // 复制至多 capacity 个节点，**返回实际复制的元素个数**。
    // 调用方按返回值遍历是安全的；已登记节点总数用 size() 查询。
    // output == nullptr 或 capacity == 0 时安全返回 0，不解引用空指针。
    std::size_t snapshot(CanNodeStatus* output, std::size_t capacity) const noexcept
    {
        if (output == nullptr || capacity == 0U) {
            return 0U;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t copied = count_ < capacity ? count_ : capacity;
        for (std::size_t i = 0; i < copied; ++i) {
            output[i] = nodes_[i];
        }
        return copied;
    }

    std::size_t size() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    // 按 node_id 原位更新单个节点（仅修改已登记节点，不改变节点集合）。
    bool replaceOne(std::uint32_t node_id, const CanNodeStatus& status) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < count_; ++i) {
            if (nodes_[i].node_id == node_id) {
                nodes_[i] = status;
                return true;
            }
        }
        return false;
    }

    // 在线集合一次性替换（初始化阶段登记全部受控节点）。
    bool replaceAll(const CanNodeStatus* input, std::size_t count) noexcept
    {
        if (count > kMaxMotorNodes || (count != 0U && input == nullptr)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < count; ++i) {
            nodes_[i] = input[i];
        }
        count_ = count;
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::array<CanNodeStatus, kMaxMotorNodes> nodes_{};
    std::size_t count_{0};
};

// ---------------------------------------------------------------------------
// 电机槽位契约（编译期可判定，唯一真相）
//
// 映射依据 Design/Config.md §6.2：
//   * 0x200 帧 4 个 int16 按 0x201、0x202、0x203、0x204 顺序排列；
//   * 0x1FF 帧 4 个 int16 按 0x205、0x206、0x207、0x208 顺序排列；
//   * §6.2 第 256 行：YAW(id:5/node:0x205) 与 变力(id:6/node:0x206) 位于
//     0x1FF 帧槽位 0、1；换弹左侧(node:0x204) 槽位 3、右侧(node:0x203) 槽位 2。
//   因此槽位 == node - 帧基址，未使用的槽位恒填 0x0000。
// ---------------------------------------------------------------------------
constexpr bool isMotorNode(std::uint32_t node_id) noexcept
{
    return node_id >= kMotorNodeMin && node_id <= kMotorNodeMax;
}

constexpr bool isMotorControlFrameId(std::uint32_t can_id) noexcept
{
    return can_id == kMotorControlFrameIdA || can_id == kMotorControlFrameIdB;
}

// 纯查询结果：可在 static_assert 中直接检查，避免输出参数阻止常量求值。
struct MotorSlotQuery final {
    bool valid{false};
    std::uint32_t frame_id{0};
    std::size_t slot{0};
};

constexpr MotorSlotQuery queryMotorSlot(std::uint32_t node_id) noexcept
{
    if (node_id >= kFrameANodeBase && node_id < kFrameANodeBase + kMotorSlotsPerFrame) {
        return MotorSlotQuery{true, kMotorControlFrameIdA,
                              static_cast<std::size_t>(node_id - kFrameANodeBase)};
    }
    if (node_id >= kFrameBNodeBase && node_id <= kMotorNodeMax) {
        return MotorSlotQuery{true, kMotorControlFrameIdB,
                              static_cast<std::size_t>(node_id - kFrameBNodeBase)};
    }
    return MotorSlotQuery{false, 0U, 0U};
}

// ---- 输出参数式查询（运行期使用） ----------------------------------------
constexpr bool motorFrameIdOf(std::uint32_t node_id, std::uint32_t& frame_id) noexcept
{
    const MotorSlotQuery query = queryMotorSlot(node_id);
    if (!query.valid) {
        return false;
    }
    frame_id = query.frame_id;
    return true;
}

constexpr bool motorSlotOf(std::uint32_t node_id, std::uint32_t& frame_id, std::size_t& slot) noexcept
{
    const MotorSlotQuery query = queryMotorSlot(node_id);
    if (!query.valid) {
        return false;
    }
    frame_id = query.frame_id;
    slot = query.slot;
    return true;
}

constexpr bool motorNodeOf(std::uint32_t frame_id, std::size_t slot, std::uint32_t& node_id) noexcept
{
    if (slot >= kMotorSlotsPerFrame) {
        return false;
    }
    if (frame_id == kMotorControlFrameIdA) {
        node_id = kFrameANodeBase + static_cast<std::uint32_t>(slot);
        return true;
    }
    if (frame_id == kMotorControlFrameIdB) {
        node_id = kFrameBNodeBase + static_cast<std::uint32_t>(slot);
        return isMotorNode(node_id);
    }
    return false;
}

// ---- 编译期固化 Design/Config.md §6.2 的槽位映射（唯一真相） ----------------
static_assert(queryMotorSlot(0x201U).frame_id == kMotorControlFrameIdA &&
              queryMotorSlot(0x201U).slot == 0U);  // 电机1 charge_right
static_assert(queryMotorSlot(0x202U).frame_id == kMotorControlFrameIdA &&
              queryMotorSlot(0x202U).slot == 1U);  // 电机2 charge_left
static_assert(queryMotorSlot(0x203U).frame_id == kMotorControlFrameIdA &&
              queryMotorSlot(0x203U).slot == 2U);  // 电机3 reload_right
static_assert(queryMotorSlot(0x204U).frame_id == kMotorControlFrameIdA &&
              queryMotorSlot(0x204U).slot == 3U);  // 电机4 reload_left
static_assert(queryMotorSlot(0x205U).frame_id == kMotorControlFrameIdB &&
              queryMotorSlot(0x205U).slot == 0U);  // 电机5 YAW
static_assert(queryMotorSlot(0x206U).frame_id == kMotorControlFrameIdB &&
              queryMotorSlot(0x206U).slot == 1U);  // 电机6 变力

// 未定义节点与反馈帧 ID 不得被当作控制槽位。
static_assert(!queryMotorSlot(0x207U).valid);
static_assert(!queryMotorSlot(0x208U).valid);
static_assert(!queryMotorSlot(0x200U).valid);
static_assert(!isMotorNode(0x200U));
static_assert(isMotorControlFrameId(0x200U) && isMotorControlFrameId(0x1FFU));
static_assert(!isMotorControlFrameId(0x201U));

} // namespace marshland::can
