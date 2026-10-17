#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "Motor.hpp"

namespace marshland::web {

inline constexpr std::size_t kChannelCount = 16;

// WEB 虚拟通道语义（下标 0 为 CH1；与 SBUS 通道表同构，差异见注）：
//   CH1  保留，不产生输出
//   CH2  蓄力机构速度（不区分左右电机），(raw - 1024) * x，范围 -660*x ~ +660*x
//   CH3  换弹机构速度（不区分左右电机），同上
//   CH4  YAW 电机速度，同上
//   CH5  AUTO / WEB 控制权分配：原始值 364=AUTO；1024=WEB。
//        与 SBUS 不同：WEB 侧没有 SBUS 档，只能分配 AUTO 或 WEB。
//        scaled_channels[4] 为中心化值：负数=AUTO，非负数=WEB。
//   CH6  软件急停（中心化离散量）：raw - 1024 > 0 正常；< 0 急停；0 为中点。
//        当前 StateMachine 不读取 CH6，也不提供急停处理。
//   CH7  发射输入（通用缩放量）：(raw - center) * x，推荐标定 -660*x ~ +660*x；
//        无持续帧数确认，不直接驱动发射舵机。
//   CH8  蓄力机构左电机速度，(raw - 1024) * x
//   CH9  蓄力机构右电机速度，同上
//   CH10 换弹机构左电机速度，同上
//   CH11 换弹机构右电机速度，同上
//   CH12 变力机构速度（专用映射）：(raw - 1024) * 2000 / 660，单位 rpm，
//        范围 -2000 ~ +2000；不使用通用 x。
//   CH13 供弹舵机输入（通用缩放量）：(raw - center) * x；不直接驱动舵机。
//   CH14~CH16 保留，仅保留解析数值，不得臆造动作。
//
// One writer (ATOM-16 HTTP 线程). All readers must hold mutex while copying
// the fields. online 由 SSE 连接状态驱动：有活跃推送连接即 true。
struct WebData final {
    mutable std::mutex mutex;
    std::array<std::int32_t, kChannelCount> scaled_channels{};
    bool online{false};
};

// HTTP 服务配置（数值来源 Design/Config.md §11.1）
struct HttpServerConfig {
    std::string  listen_address{"0.0.0.0"};
    std::int32_t port{10000};
    std::size_t  request_body_limit_bytes{64 * 1024};    // 超限返回 413
    std::size_t  response_body_limit_bytes{1024 * 1024}; // 超限返回 500
    const char*  static_root{nullptr};                   // 前端目录；nullptr 不提供静态服务
};

// HTTP 线程函数（ATOM-16 实现）。装配层以此拉起 HTTP 线程：
//     std::thread t(marshland::web::RunHttpServer, cfg, std::ref(data),
//                   &motors, std::cref(stop_flag));
// 阻塞运行直至 stop_requested 置位。返回 0 = 正常停止；1 = 监听绑定失败。
// motors 为 ATOM-04 的电机状态读取口（可为 nullptr，则电机数据不可用）；
// 控制源经 state::getGlobalStateMachine() 全局读取。
// 调用方拥有线程与所有引用参数的生命周期；本函数返回前不得销毁它们。
int RunHttpServer(HttpServerConfig config, WebData& data,
                  const motor::IMotorControl* motors,
                  const std::atomic<bool>& stop_requested) noexcept;

} // namespace marshland::web
