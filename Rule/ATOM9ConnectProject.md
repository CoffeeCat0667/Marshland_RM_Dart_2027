# ATOM-09 Force 接入结论 —— 对 GeneralBugs/20261015-Entry.md 的答复

**日期**: 2026-10-15
**出具方**: ATOM-09_Force 负责人（本模块工作目录 Agent）
**针对报告**: `Atoms/Public/GeneralBugs/20261015-Entry.md`（Entry.cpp 集成 ATOM-08/09/10/11 架构冲突报告）
**结论等级**: **ATOM-09 不存在该报告所述的循环依赖**，无需架构决策即可接入；ATOM-09 侧无需任何代码修改。

---

## 1. 结论摘要

报告将 ATOM-08/09/10/11 并列判定为"构造时需要 AppCoordinator、注册又先于 AppCoordinator"的循环依赖。该诊断对**有生命周期适配器**的 ATOM-08/10/11 成立，但**对 ATOM-09 不成立**，原因有二：

1. **ATOM-09 不注册生命周期**。按 PLAN.md §2.1 与裁决 Q-07，ATOM-09 是纯逻辑内核：**不实现 `ILifecycleModule`，不提供 `lifecycle_adapter`，不注册任何 `RuntimeStep`**。循环链"模块注册需要先构造内核对象"这一环对 ATOM-09 不存在，`Entry.cpp` 完全可以在 `AppCoordinator` 构造完成之后再构造 `ForceController`。
2. **ATOM-09 不需要 `IControlAuthority`**。报告 §2.2 自己引用的 `force_controller.hpp:30` 构造函数清楚显示：ATOM-09 仅注入 `IStateReader`，**没有 `IControlAuthority` 参数**。这是刻意设计（Q-05/Q-16）：授权决策归 ATOM-02，控制意图由调用方（ATOM-13/16）先 `SubmitIntent(CONTROL)`，ATOM-09 只做 fail-closed 复核。

因此 `Entry.cpp:417` 的编译错误是**装配顺序与实参类型错误**（在 `AppCoordinator` 之前构造并误传 `StateArbitrator`），不是架构死锁。

---

## 2. ATOM-09 的正确接入方式

### 2.1 构造时机

在 `AppCoordinator app{runtime, arbitrator, clock};` **之后**构造 `ForceController`。此时其全部依赖均已可用：

```cpp
AppCoordinator app{runtime, arbitrator, clock};

// ATOM-09 内核：在 AppCoordinator 之后构造
marshland::atom09::ForceController force_controller(
    config_store,        // config::IConfigView        —— 已有（ATOM-01 体系）
    clock,               // app::IMonotonicClock       —— 已有
    sbus_core,           // sbus::ISbusSnapshotReader  —— 已有（ATOM-03）
    app,                 // app::IStateReader          —— AppCoordinator 实现
    force_motor_sink,    // atom09::IMotorCommandSink  —— 装配层适配 ATOM-05
    &force_fault_sink);  // atom09::IForceFaultSink*   —— 装配层适配（可空）
```

### 2.2 周期驱动

`update()` **不走** `RuntimeCoordinator` 生命周期步骤（Q-04）：由装配层普通周期线程按 `timing.max_control_cycle_ms`（上限 100ms）调用：

```cpp
force_controller.update(clock.nowMicros());
```

### 2.3 依赖适配清单（装配层负责）

| ATOM-09 端口 | 适配目标 | 说明 |
|---|---|---|
| `IMotorCommandSink::submitVelocityTarget(id, rpm)` | ATOM-05 电机内核 | 提交速度闭环目标 |
| `IMotorCommandSink::stopMotor(id)` | ATOM-05 | 撤销目标并走**零力矩**锁存（非 0 rpm 闭环） |
| `IMotorCommandSink::readFeedback(id, out)` | ATOM-05 | 读取该电机最近一次有效反馈 |
| `IMotorCommandSink::faultLatched(id)` | ATOM-05 | 电机/编码器/PID/CAN 故障锁存查询 |
| `IForceFaultSink::onForceActionFailed(detail)` | 装配层 → ATOM-14 | 只上报"变力动作失败"事实，**不携带故障码**（Q-17） |

适配器实现方禁止回调 ATOM-09（避免环），全部方法须在控制周期预算内返回。

### 2.4 前置条件（装配层核对）

1. 配置中 `motor[]` 恰有一条 `role == "force"` 记录（电机 ID 由此解析，禁止硬编码 6）。
2. 新增配置键 `force.center_deadband_rpm`、`force.center_action` 已加入 `Config/dart_control.json` 与 ConfigSchema；未加入前 ATOM-09 按规则 **fail-closed**，如实报告配置不可用。
3. 公共契约 `Atoms/Public/ForceTypes.hpp`、`ForceInterfaces.hpp` 已登记进 Public CMake（0.5 版已完成）。

---

## 3. 对报告建议条款的立场

| 报告条款 | ATOM-09 立场 |
|---|---|
| §7.1 回滚 Entry.cpp 到 ATOM-07 后稳定状态 | ATOM-08/10/11 部分回滚合理；**ATOM-09 部分无需回滚等待架构决策**，按本文 §2 调整构造顺序即可接入，建议与另三个模块分开处理 |
| §5 方案 A（延迟构造） | 赞同用于 ATOM-08/10/11；对 ATOM-09 **不必要**（无生命周期适配器，天然支持在 AppCoordinator 之后构造） |
| §5 方案 B（提前创建 AppCoordinator） | 反对，架构冲击过大 |
| §5 方案 C（Setter 注入） | 反对，违反"不修改 ATOM 代码"约束且引入半初始化窗口 |
| §7.2 更新 `Rule/AtomIntegration.md` | 赞同，建议新增下述区分（见 §4） |

---

## 4. 对 `Rule/AtomIntegration.md` 的补充建议

建议在接入规范中明确区分两类模块，避免后续模块（ATOM-12/13/14/18 预测会遇到同类困惑）重复此问题：

1. **生命周期模块**：注册 `RuntimeStep`，须在 `AppCoordinator` 构造前完成注册；若其内核需要 `AppCoordinator` 接口，应采用工厂函数延迟构造（方案 A）。
2. **纯逻辑内核**（不注册生命周期，如 ATOM-09）：由装配层在 `AppCoordinator` 之后构造并注入依赖，由装配层周期线程驱动 `update()`。接入规范应写明"构造时机 = AppCoordinator 之后、主循环之前"。

---

## 5. ATOM-09 侧承诺

- **无需任何代码修改**：现有 `ForceController` 构造签名与行为设计（PLAN.md 0.5）与本文接入方式完全一致。
- ATOM-09 保持纯逻辑内核性质：无线程、无 I/O、无设备句柄、不注册生命周期。
- 若在接入过程中发现 ATOM-09 实际行为与 PLAN.md §7 验收判定表不符，请回本模块工作目录提交缺陷，由本模块负责人在获得授权后修复。

---

**文档性质**: 本文件为 ATOM-09 负责人对接入方式的正式结论，供装配层与架构评审参考。
