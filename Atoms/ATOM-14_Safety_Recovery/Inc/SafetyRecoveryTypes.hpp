// ============================================================================
// SafetyRecoveryTypes.hpp — ATOM-14 安全、故障与恢复管理的公共类型
//
// 权威来源：
//   Design/Atomic/ATOM-14_Safety_Recovery.md（只读）
//   本目录 Guide.md（急停两步契约、分阶段输出、一般恢复固定顺序）
//
// 依赖方向：仅依赖 Atoms/Public 的 MarshlandTypes.hpp 与 C++ 标准库。
// 禁止包含任何 ATOM 私有头、AppCoordinator 相关头或平台头。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include "MarshlandTypes.hpp"

namespace marshland::safety {

// ---------------------------------------------------------------------------
// 安全输出阶段：决定 CAN 力矩输出门控与恢复出力策略（Guide §6）
// ---------------------------------------------------------------------------
enum class SafetyPhase : std::uint8_t {
    NORMAL,            // 非安全阶段：本模块不持有输出门控
    ESTOP_LOCKED,      // 急停锁定等待人工确认：全部电机持续零力矩，换弹不抬起
    ESTOP_RECOVERY,    // 人工确认后的安全恢复：仅获准机构出力，其余零力矩
    GENERAL_RECOVERY,  // 一般故障自动恢复编排中
    FAULT_LOCKED,      // 恢复失败锁定：全部零力矩、舵机保持、禁止发射、不自动重试
};

// ---------------------------------------------------------------------------
// 恢复编排内部阶段（顺序不可调换，Guide §4.2 / 设计文档「一般故障恢复」）
// 急停确认后的解除动作与一般故障恢复共用同一组阶段语义。
// ---------------------------------------------------------------------------
enum class RecoveryStage : std::uint8_t {
    NONE,
    CHARGE_HOME,     // 蓄力电机 1、2 回零；两个限位均稳定低电平才完成
    RECOVERY_FIRES,  // 仅发射机构三次单次发射；期间其他电机零力矩、舵机不动
    RELOAD_LIFT,     // 双 M3508 换弹抬起：按 Config 定时运行后两台 0 rpm 到位
    COMPLETED,
    FAILED,          // 恢复抬起再次失败等：进入 FAULT 锁定，不自动重试
};

// 机构动作请求结果（窄端口返回；具体失败原因经故障登记上报）
enum class ActionResult : std::uint8_t {
    ACCEPTED,
    REJECTED,
    OFFLINE,   // 机构离线或反馈异常
    FAULT,
};

// ---------------------------------------------------------------------------
// 恢复动作标识与运行状态（IRecoveryActions 的状态查询）
// ---------------------------------------------------------------------------
enum class RecoveryAction : std::uint8_t {
    CHARGE_HOME,     // 蓄力双电机回零
    RECOVERY_FIRE,   // 发射机构单次发射
    RELOAD_LIFT,     // 双 M3508 换弹抬起
};

enum class ActionStatus : std::uint8_t {
    IDLE,       // 未请求或已撤销
    RUNNING,    // 已接受，动作进行中
    SUCCEEDED,  // 到位判据达成（回零双限位除外，见 ILimitSwitchReader）
    FAILED,     // 动作失败；换弹抬起失败对应 F-RELOAD-002 锁定
};

// ---------------------------------------------------------------------------
// 故障码目录条目（常量表见 SafetyRecoveryFaultCatalog.hpp）
//
// 目录内容逐字取自 Design/Config.md §14.2，禁止增删故障码或修改等级；
// 运行时不从 dart_control.json 读取故障码（该文件无故障段）。
// ---------------------------------------------------------------------------
struct FaultCatalogEntry final {
    const char* code;         // 如 "F-RELOAD-002"
    FaultSeverity severity;
    bool auto_recovery;       // Config §14.2「自动恢复」列（含机构自身恢复路径）
    bool manual_confirm;      // 是否需要人工确认
    // 根因清除后是否触发 PEF 一般故障恢复流程：仅恢复动作明确为「执行 PEF
    // 一般故障恢复流程」的故障码为 true；视觉/SBUS 等自身恢复路径不在此列，
    // W-VISION-002 属正常回退路径，不触发一般故障恢复。
    bool pef_recovery;
};

// ---------------------------------------------------------------------------
// 故障记录（Interface §18.2、Config §14.2/§16.1）
//
// 故障码与等级全部来自 Config §14.2，本模块不自造；历史 1000 条环形保留、
// 最新优先。utc_text 为展示用 UTC 时间戳，monotonic_us 为内部计时基准，
// 两种时间基准同时保留，不得用单调时钟纪元推算 UTC。
// ---------------------------------------------------------------------------
inline constexpr std::size_t kFaultCodeCapacity = 24;
inline constexpr std::size_t kFaultSourceCapacity = 16;
inline constexpr std::size_t kFaultUtcTextCapacity = 32;
inline constexpr std::size_t kFaultHistoryCapacity = 1000;

struct FaultRecord final {
    char code[kFaultCodeCapacity]{};            // 如 "F-RELOAD-002"
    char source[kFaultSourceCapacity]{};        // 故障来源（如 "ATOM-04"）
    FaultSeverity severity{FaultSeverity::INFO};
    char utc_text[kFaultUtcTextCapacity]{};     // UTC 展示时间戳（ISO 8601 文本）
    std::uint64_t monotonic_us{0};              // 单调时钟基准（等待/超时用）
    bool active{false};                         // 当前是否仍未清除
    bool recovery_in_progress{false};           // 自动恢复状态
    bool acknowledged{false};                   // 人工确认状态
};

} // namespace marshland::safety
