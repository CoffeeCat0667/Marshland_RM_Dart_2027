#ifndef MARSHLAND_PUBLIC_FORCE_INTERFACES_HPP
#define MARSHLAND_PUBLIC_FORCE_INTERFACES_HPP

#include "ForceTypes.hpp"
#include <cstdint>

namespace marshland::force {

/// 变力控制器接口（供 ATOM-13/16/17-18 调用）
class IForceController {
public:
    virtual ~IForceController() = default;

    /// 提交变力目标
    /// @param target 目标参数
    /// @param mode 模式（TIMED 或 MANUAL）
    /// @return 提交结果
    virtual ForceSubmitResult submitTarget(const ForceTarget& target, ForceMode mode) noexcept = 0;

    /// 周期推进（由装配层调用）
    /// @param now_us 当前时间（微秒）
    virtual void update(std::int64_t now_us) noexcept = 0;

    /// 查询当前状态
    /// @return 变力状态快照
    virtual ForceStatus status() const noexcept = 0;

    /// 停止当前动作
    virtual void stop() noexcept = 0;

    /// 查询模块是否就绪（构造期配置加载是否成功）
    virtual bool ready() const noexcept = 0;

    /// 查询初始化错误信息
    /// @return 错误描述（nullptr 表示无错误）
    virtual const char* initializationError() const noexcept = 0;
};

} // namespace marshland::force

#endif // MARSHLAND_PUBLIC_FORCE_INTERFACES_HPP
