// ============================================================================
// ws_snapshot.hpp — ATOM-17 状态快照（从 IStateReader 按值拷出的只读数据）
//
// StateReadView 是持锁借用视图：锁内禁止 I/O，引用不得随视图释放后保留。
// 因此推送管线先在读取租约内把所需字段按值拷出到本快照，再在锁外完成
// JSON 序列化与网络发送。
// ============================================================================
#pragma once

#include <cstdint>

#include "AppInterfaces.hpp"
#include "StateArbitrationTypes.hpp"

namespace marshland::atom17 {

struct StateSnapshot {
    state_arbitration::ArbitrationContext context{};
    app::RunGateReason run_gate{app::RunGateReason::NOT_INITIALIZED};
    bool hardware_ready{false};
    bool estop_latched{false};
    std::uint64_t run_id{0};
    std::uint32_t current_shot{0};

    // 最近一次控制源变更事件的原因（control_arbiter.preempt_reason 的数据源）。
    bool has_preempt_reason{false};
    state_arbitration::TransitionReason preempt_reason{
        state_arbitration::TransitionReason::NONE};
};

} // namespace marshland::atom17
