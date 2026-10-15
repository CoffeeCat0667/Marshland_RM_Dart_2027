// ============================================================================
// SafetyRecoveryFaultCatalog.hpp — ATOM-14 故障码常量目录
//
// 内容逐字取自 Design/Config.md §14.2 故障表（2026-10-15 修复 F-YAW-001
// 残缺行后的版本），共 23 项。规则：故障码与等级全部来自该表，禁止自造；
// dart_control.json 无故障段，运行时不从配置文件读取故障码。
//
// pef_recovery 仅对恢复动作明确为「执行 PEF 一般故障恢复流程」的故障码置
// true；F-VISION-001 / W-SBUS-001 走自身恢复路径，W-VISION-002 属正常回退
// 路径，均不触发一般故障恢复。
//
// 注意：故障等级归约（是否全局阻断）由状态仲裁的 IsFaultBlocking 判定，
// 本目录不复制该判定逻辑；本表只提供登记/展示用的等级目录。
// ============================================================================
#pragma once

#include <cstddef>

#include "SafetyRecoveryTypes.hpp"

namespace marshland::safety {

inline constexpr FaultCatalogEntry kFaultCatalog[] = {
    // 故障码                等级                 自动恢复  人工确认  PEF 一般恢复
    // CAN（Design/Config.md §14.2）
    {"F-CAN-001",     FaultSeverity::SEVERE,  true,  false, true},   // CAN 总线异常
    {"F-CAN-002",     FaultSeverity::SEVERE,  true,  false, true},   // 零力矩帧发送失败
    // 电机 / 编码器
    {"F-MOTOR-001",   FaultSeverity::SEVERE,  true,  false, true},   // 单电机离线
    {"F-MOTOR-002",   FaultSeverity::SEVERE,  true,  false, true},   // 电机反馈超时
    {"F-MOTOR-003",   FaultSeverity::SEVERE,  true,  false, true},   // 电机闭环异常
    {"F-ENC-001",     FaultSeverity::SEVERE,  true,  false, true},   // 编码器异常
    // 命令 / GPIO / 限位
    {"W-CMD-001",     FaultSeverity::WARNING, false, false, false},  // 控制目标异常
    {"F-GPIO-001",    FaultSeverity::SEVERE,  true,  false, true},   // 限位输入异常
    {"F-LIMIT-001",   FaultSeverity::SEVERE,  true,  false, true},   // 限位触发
    // IIC / 舵机 / YAW
    {"F-IIC-001",     FaultSeverity::SEVERE,  true,  false, true},   // PCA9685A/IIC 异常
    {"F-SERVO-001",   FaultSeverity::SEVERE,  true,  false, true},   // 舵机动作超时/失败
    {"F-YAW-001",     FaultSeverity::WARNING, false, false, false},  // YAW 调整动作超时
    // 视觉（自身恢复路径，不进 PEF 一般恢复）
    {"F-VISION-001",  FaultSeverity::WARNING, true,  false, false},  // 视觉系统失联
    {"W-VISION-002",  FaultSeverity::INFO,    false, false, false},  // 视觉无响应回退
    // 通信
    {"W-SBUS-001",    FaultSeverity::WARNING, true,  false, false},  // SBUS 失联（降级 WEB）
    {"W-WEB-001",     FaultSeverity::WARNING, false, false, false},  // WEB 连接异常
    {"I-WS-001",      FaultSeverity::INFO,    false, false, false},  // WebSocket 连接异常
    // 配置
    {"F-CONFIG-001",  FaultSeverity::SEVERE,  false, true,  false},  // 参数加载失败
    {"F-CONFIG-002",  FaultSeverity::SEVERE,  false, true,  false},  // 配置文件损坏
    {"W-CONFIG-003",  FaultSeverity::WARNING, false, true,  false},  // 参数缺失
    {"W-PID-001",     FaultSeverity::WARNING, false, false, false},  // PID 参数越界
    // 安全 / 换弹
    {"E-ESTOP-001",   FaultSeverity::ESTOP,   false, true,  false},  // 软件急停
    {"F-RELOAD-001",  FaultSeverity::SEVERE,  true,  false, true},   // 换弹动作失败
    {"F-RELOAD-002",  FaultSeverity::SEVERE,  false, true,  false},  // 恢复抬起失败锁定（不重试）
};

inline constexpr std::size_t kFaultCatalogSize =
    sizeof(kFaultCatalog) / sizeof(kFaultCatalog[0]);

// 按码查询目录；未命中返回 nullptr（调用方不得据此自造条目）。
constexpr const FaultCatalogEntry* findFaultCatalogEntry(const char* code) noexcept
{
    if (code == nullptr) {
        return nullptr;
    }
    for (const FaultCatalogEntry& entry : kFaultCatalog) {
        const char* a = entry.code;
        const char* b = code;
        while (*a != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*a == '\0' && *b == '\0') {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace marshland::safety
