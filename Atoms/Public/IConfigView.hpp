// ============================================================================
// IConfigView.hpp — 配置查询契约（跨 ATOM 公共契约）
//
// 归属说明：
//   本文件是**跨模块契约**，与 MarshlandTypes.hpp / ILifecycle.hpp /
//   StateArbitrationTypes.hpp / AppInterfaces.hpp 同级，置于 Atoms/Public/。
//   由 ATOM-15_Config_Persistence 实现，由装配层注入给需要的模块。
//
// 为什么需要它（而不是只靠 RuntimeContext）：
//   ILifecycleModule::initialize(const RuntimeContext&) 收到的是 **const 引用**，
//   且 RuntimeContext 被 RuntimeCoordinator **按值私有持有**。因此配置模块
//   无法把解析结果写回该 context 供后续模块读取——配置必须经本接口按需查询。
//   详见 Atoms/ATOM-15_Config_Persistence/PLAN.md §2.1（约束 C1/C2/C3）。
//
// 键名与类型：**必须**遵循 Rule/ConfigSchema.md。数值一律以 Design/Config.md 为准。
//
// 并发约定：
//   * 查询可在任意控制线程调用，实现必须保证并发读安全；
//   * configVersion() 单调递增；消费方通过它判断是否需要重新读取（热加载）；
//   * 本接口的任何方法都**不得**阻塞在 I/O 上（配置文件读取发生在加载阶段）。
//
// 错误处理：
//   * 全部方法为 noexcept，**不得**抛异常；
//   * 失败必须通过 ConfigStatus 显式表达，**禁止**用"返回默认值 + 无提示"掩盖错误。
//
// 不作为：
//   * 不提供写入接口（运行期更新走 ATOM-15 的既定路径，不在此契约内）；
//   * 不表达业务语义（不解释某个值意味着什么）。
// ============================================================================
#pragma once

#include <cstdint>

namespace marshland::config {

// ---------------------------------------------------------------------------
// 查询结果状态
//
// 消费方**必须**区分 OK 与其余状态；把非 OK 当作 OK 使用会导致以未校验值驱动执行器。
// ---------------------------------------------------------------------------
enum class ConfigStatus : std::uint8_t {
    OK,
    NOT_LOADED,          // 配置尚未成功加载（加载失败或尚未到达该阶段）
    MISSING,             // 键不存在
    INVALID_TYPE,        // 键存在但类型不符
    OUT_OF_RANGE         // 键存在、类型正确，但超出 Config.md 规定的范围
};

inline const char* ToString(ConfigStatus status) noexcept
{
    switch (status) {
    case ConfigStatus::OK:           return "OK";
    case ConfigStatus::NOT_LOADED:   return "NOT_LOADED";
    case ConfigStatus::MISSING:      return "MISSING";
    case ConfigStatus::INVALID_TYPE: return "INVALID_TYPE";
    case ConfigStatus::OUT_OF_RANGE: return "OUT_OF_RANGE";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 配置查询视图
//
// 只读。实现方为 ATOM-15；消费方为 ATOM-03 ~ ATOM-19。
// 消费方**禁止**包含 ATOM-15 的私有头，只依赖本契约。
// ---------------------------------------------------------------------------
class IConfigView {
public:
    virtual ~IConfigView() = default;

    // ---- 标量查询 ---------------------------------------------------------
    // 全部按 Rule/ConfigSchema.md 的 ID 查询。
    // out 仅在返回 OK 时被写入；返回非 OK 时 out 保持调用前的值不变。
    virtual ConfigStatus getInt(const char* key, std::int64_t& out) const noexcept = 0;
    virtual ConfigStatus getDouble(const char* key, double& out) const noexcept = 0;
    virtual ConfigStatus getBool(const char* key, bool& out) const noexcept = 0;

    // 字符串以 (指针, 长度) 返回，避免所有权的歧义；指针在下一次配置热加载前保持有效。
    // 实现必须保证该内存在 configVersion() 未变化期间稳定。
    virtual ConfigStatus getString(const char* key, const char*& out_data,
                                   std::uint64_t& out_size) const noexcept = 0;

    // ---- 存在性与数组 -----------------------------------------------------
    // 仅判断键是否存在（不判断类型与范围）；用于区分 MISSING 与其它失败。
    virtual bool has(const char* key) const noexcept = 0;

    // 数组长度查询（Rule/ConfigSchema.md 中的 array<T> 键）。
    virtual ConfigStatus getArraySize(const char* key, std::uint64_t& out_size) const noexcept = 0;

    // 按数组下标查询标量，key 形如 "motor" 或 "servo.channels"。
    virtual ConfigStatus getIntAt(const char* key, std::uint64_t index,
                                  const char* element_key, std::int64_t& out) const noexcept = 0;
    virtual ConfigStatus getDoubleAt(const char* key, std::uint64_t index,
                                     const char* element_key, double& out) const noexcept = 0;
    virtual ConfigStatus getBoolAt(const char* key, std::uint64_t index,
                                   const char* element_key, bool& out) const noexcept = 0;
    virtual ConfigStatus getStringAt(const char* key, std::uint64_t index,
                                     const char* element_key, const char*& out_data,
                                     std::uint64_t& out_size) const noexcept = 0;

    // ---- 版本与可用性 -----------------------------------------------------
    // 单调递增。每次成功的热加载都必须自增，消费方据此决定是否重新读取。
    virtual std::uint64_t configVersion() const noexcept = 0;

    // 配置是否已成功加载。为 false 时所有 get* 必须返回 NOT_LOADED。
    virtual bool isLoaded() const noexcept = 0;
};

} // namespace marshland::config
