// ============================================================================
// fire_facts.hpp — ATOM-11 安全事实与授权结论快照
//
// FireFacts 由生命周期适配器每个控制周期经 Public 契约
// （IStateReader / IControlAuthority）拉取并组装后传入内核；内核只消费快照，
// 不直接触碰任何 Public 查询接口，保持可注入替身的纯逻辑形态。
//
// 授权结论复用 Public 的 state_arbitration::FireAuthorization，不自造控制权
// 语义；授权判定本身由 ATOM-02 的 StateArbitrator 做出，本模块只查询不裁决。
// ============================================================================
#pragma once

#include "StateArbitrationTypes.hpp"

namespace marshland::atom11 {

struct FireFacts {
    bool software_estop{false};  // 软件急停锁存（Config §15）
    bool fault_active{false};    // 故障激活（Config §14.2）
    state_arbitration::FireAuthorization authorization{};
};

} // namespace marshland::atom11
