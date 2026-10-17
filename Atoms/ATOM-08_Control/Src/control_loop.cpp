// ============================================================================
// control_loop.cpp — 控制源 → scaled_channels → 电机转速 / 舵机角度 → 直接写入
//
// 控制源：marshland::state::getGlobalStateMachine()->control_source()
//         （枚举 state::ControlSource{ AUTO, WEB, SBUS }，无 NONE；
//          装配层未注入状态机时返回 nullptr → 本周期不写任何执行器）
//
// 数据源：SBUS → sbus::SbusData / WEB → web::WebData / AUTO → auto_fire::AutoData
//         三者由装配层各自注入，读者必须在持 mutex 期间拷贝字段。
//
// 通道表来源：用户 2026-10-16 给定的 SBUS 通道定义（含供弹补充） + PLAN.md §16/§17。
//   下标 0 = CH1（与 scaled_channels 的约定一致）。
//   电机：CH2 蓄力整体 / CH3 换弹整体 / CH4 YAW / CH8 左 / CH9 右 /
//         CH10 左 / CH11 右 / CH12 变力
//   舵机：CH7 < 0 → 开火（PCA9685 通道 5，180°） / CH13 < 0 → 供弹（通道 6，70°）
//   其余通道（CH1 / CH5 / CH6 / CH14～CH16）不产生输出。
//
// 行为约束（用户裁决）：
//   * D1：scaled_channels 的值已经是 rpm，本文件不做任何换算；
//   * D2：0 值照写，即 0 rpm 速度闭环（不是零力矩）；
//   * D4：不提交仲裁意图；
//   * D6：越出 IMotorControl 允许范围时不写该电机（保持旧目标），也不钳位；
//   * D3：失联语义不归本模块，本文件不读 online 标志；
//   * 开火/供弹为电平判据，不做连续帧确认、不做保持计时。
// ============================================================================
#include "atom08_control/control_loop.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace marshland::atom08 {
namespace {

using marshland::state::ControlSource;

// ---- SBUS 通道下标（0 基；用户给定的通道表） ------------------------------
constexpr std::size_t kChannelChargeBoth = 1U;   // CH2  蓄力机构速度（不区分左右）
constexpr std::size_t kChannelReloadBoth = 2U;   // CH3  换弹机构速度（不区分左右）
constexpr std::size_t kChannelYaw = 3U;          // CH4  YAW 电机速度
constexpr std::size_t kChannelFireRequest = 6U;  // CH7  <0 → 发射
constexpr std::size_t kChannelChargeLeft = 7U;   // CH8  蓄力左电机速度
constexpr std::size_t kChannelChargeRight = 8U;  // CH9  蓄力右电机速度
constexpr std::size_t kChannelReloadLeft = 9U;   // CH10 换弹左电机速度
constexpr std::size_t kChannelReloadRight = 10U; // CH11 换弹右电机速度
constexpr std::size_t kChannelForce = 11U;       // CH12 变力机构速度（专用映射，单位 rpm）
constexpr std::size_t kChannelFeedRequest = 12U; // CH13 <0 → 供弹位

// ---- 电机 CAN 节点（PLAN.md §16 执行器归属表） ----------------------------
constexpr std::uint32_t kNodeChargeRight = 0x201U; // 电机 1 蓄力右
constexpr std::uint32_t kNodeChargeLeft = 0x202U;  // 电机 2 蓄力左
constexpr std::uint32_t kNodeReloadRight = 0x203U; // 电机 3 换弹右
constexpr std::uint32_t kNodeReloadLeft = 0x204U;  // 电机 4 换弹左
constexpr std::uint32_t kNodeYaw = 0x205U;         // 电机 5 YAW
constexpr std::uint32_t kNodeForce = 0x206U;       // 电机 6 变力

// ---- 舵机通道与角度（PLAN.md §16 执行器归属表） ---------------------------
constexpr std::uint8_t kServoChannelFire = 5U; // 开火舵机（PCA9685 通道 5，无 id）
constexpr std::uint8_t kServoChannelFeed = 6U; // 供弹舵机（PCA9685 通道 6，id = 6）
constexpr double kFireStandbyDeg = 90.0;
constexpr double kFireActionDeg = 180.0;
constexpr double kFeedStandbyDeg = 90.0;
constexpr double kFeedActionDeg = 70.0;

using Channels = std::array<std::int32_t, marshland::sbus::kChannelCount>;

// ---- 数据发布源取数（各持其 mutex 拷贝后立即释放） ------------------------
Channels CopyChannels(const marshland::sbus::SbusData& data) noexcept
{
    std::lock_guard<std::mutex> lock(data.mutex);
    return data.scaled_channels;
}

Channels CopyChannels(const marshland::web::WebData& data) noexcept
{
    std::lock_guard<std::mutex> lock(data.mutex);
    return data.scaled_channels;
}

// D6：越界即拒绝（保持旧目标），不钳位、不打印、不上报。
void WriteRpm(marshland::motor::IMotorControl& motors, std::uint32_t node_id,
              std::int32_t rpm) noexcept
{
    if (rpm < marshland::motor::kMinTargetRpm || rpm > marshland::motor::kMaxTargetRpm) {
        return;
    }
    (void)motors.setRpm(node_id, static_cast<std::int16_t>(rpm));
}

// 机构速度：整体键（CH2 / CH3）非零时覆盖左右键，否则左右键各写各的。
void ApplyMechanism(marshland::motor::IMotorControl& motors, const Channels& channels,
                    std::size_t both_channel, std::size_t left_channel,
                    std::size_t right_channel, std::uint32_t left_node,
                    std::uint32_t right_node) noexcept
{
    if (channels[both_channel] != 0) {
        WriteRpm(motors, left_node, channels[both_channel]);
        WriteRpm(motors, right_node, channels[both_channel]);
        return;
    }
    WriteRpm(motors, left_node, channels[left_channel]);
    WriteRpm(motors, right_node, channels[right_channel]);
}

// 舵机：电平判据 —— 请求通道为负即动作位，否则待机位。
void ApplyServos(marshland::servo::IServoDriver& servos, const Channels& channels) noexcept
{
    const double fire_deg =
        channels[kChannelFireRequest] < 0 ? kFireActionDeg : kFireStandbyDeg;
    (void)servos.writeChannelAngle(kServoChannelFire, fire_deg);

    const double feed_deg =
        channels[kChannelFeedRequest] < 0 ? kFeedActionDeg : kFeedStandbyDeg;
    (void)servos.writeChannelAngle(kServoChannelFeed, feed_deg);
}

// 一张通道表同时驱动电机与舵机；SBUS 与 WEB 的通道语义同构，故共用。
void ApplyChannels(marshland::motor::IMotorControl& motors,
                   marshland::servo::IServoDriver& servos, const Channels& channels) noexcept
{
    // 蓄力：CH2 覆盖 / CH8 左（电机 2） / CH9 右（电机 1）
    ApplyMechanism(motors, channels, kChannelChargeBoth, kChannelChargeLeft,
                   kChannelChargeRight, kNodeChargeLeft, kNodeChargeRight);

    // 换弹：CH3 覆盖 / CH10 左（电机 4） / CH11 右（电机 3）
    ApplyMechanism(motors, channels, kChannelReloadBoth, kChannelReloadLeft,
                   kChannelReloadRight, kNodeReloadLeft, kNodeReloadRight);

    // YAW：CH4（电机 5）
    WriteRpm(motors, kNodeYaw, channels[kChannelYaw]);

    // 变力：CH12（专用映射，已是 rpm；中位 0 即 0 rpm 闭环，D2）
    WriteRpm(motors, kNodeForce, channels[kChannelForce]);

    // 开火 CH7 / 供弹 CH13
    ApplyServos(servos, channels);
}

} // namespace

void RunControlLoop(marshland::sbus::SbusData& sbus, marshland::web::WebData& web,
                    marshland::auto_fire::AutoData& auto_data,
                    marshland::motor::IMotorControl& motors,
                    marshland::servo::IServoDriver& servos, std::uint32_t period_ms,
                    const std::atomic<bool>& stop_requested) noexcept
{
    const std::uint32_t effective_period_ms =
        period_ms != 0U ? period_ms : static_cast<std::uint32_t>(marshland::motor::kDrivePeriodMs);
    const std::chrono::milliseconds period(effective_period_ms);

    while (!stop_requested.load(std::memory_order_acquire)) {
        // 状态机由装配层在启动时注入；未注入时本周期不写任何执行器（fail-safe）。
        const marshland::state::IStateMachine* machine =
            marshland::state::getGlobalStateMachine();
        if (machine != nullptr) {
            switch (machine->control_source()) {
            case ControlSource::SBUS:
                ApplyChannels(motors, servos, CopyChannels(sbus));
                break;
            case ControlSource::WEB:
                ApplyChannels(motors, servos, CopyChannels(web));
                break;
            case ControlSource::AUTO:
                // auto_fire::AutoData 目前只有 state/shot_index/active/stop_fire/online，
                // **不含 scaled_channels**（Atoms/Public/Auto.hpp）。字段落地前不得臆造
                // 通道数据，故本分支不产生任何执行器输出；发布源引用已接好，落地后
                // 直接在此按同一通道表取数即可。
                (void)auto_data;
                break;
            }
        }

        std::this_thread::sleep_for(period);
    }
}

} // namespace marshland::atom08
