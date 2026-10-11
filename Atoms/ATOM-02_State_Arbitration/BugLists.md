# ATOM-02_State_Arbitration 待修改事项

- 更新日期：2026-10-10（**ATOM-02 已实施 018/019/020**，见 §5 实施记录）。
- 设计基线：PRD 0.10、PEF 1.5、Interface 1.5、Config 0.8。
- 本文件按本轮用户要求重新创建，只登记未完成事项；不恢复已删除历史内容。
- 以下仲裁/状态机修改由 ATOM-02 负责人实施。本工程 Agent 不在网关复制策略，也不修改本模块代码。

## 1. ATOM02-BUG-018：WEB 清标志绕过 SBUS 控制权

**优先级：P1。状态：✅ 已实施（2026-10-10，见 §5）。**

位置：`Src/StateArbitration.cpp` 的 EvaluateIntent/STOP_FIRE_CLEAR 分支。

该分支在控制源优先级检查前返回 accepted。MANUAL/DEBUG、非活动自动流程、当前控制源 SBUS 时，WEB 清除请求仍被接受，违反 Config 11.4 和 Interface 9.2。

所需行为：`source == WEB`、`kind == STOP_FIRE_CLEAR` 且当前权威 `control_source == SBUS` 时，返回 accepted=false、LOWER_PRIORITY_CONFLICT，不修改标志、模式或自动流程；允许产生拒绝事件。

不能仅以 sbus_online 为条件：SBUS 在线但自动模式为 True、当前源 AUTO 时仍按正常 WEB 条件判断。保留有效性、来源、急停、手动/调试和活动流程约束，不改变 STOP_FIRE_SET 的安全单调行为。

验收覆盖 WEB+SBUS 拒绝、WEB+WEB/AUTO 既有允许路径、SBUS 在线自动模式不误拒、SBUS 来源不受 WEB 专属限制、急停/非法模式拒绝、拒绝后重连/失联不自动执行旧请求。

网关的 SubmitStopFireIntent 只在同一锁内委托 SubmitIntent 并提交获准标志，**不是该缺陷的策略修复**。ATOM-02 修复前不能宣称 WEB/SBUS 限制已经生效。

## 2. ATOM02-BUG-019：清标志分支跳过阻断故障检查

**优先级：P1。状态：✅ 已实施（2026-10-10，见 §5）。**

同一分支还跳过 IsFaultBlocking。模式仍为 MANUAL/DEBUG 而 fault_active=true、等级 SEVERE/ESTOP 时，清除请求可被接受，违背既有故障保护。

需要在接受清除前应用现有阻断故障语义，返回 FAULT_ACTIVE，不放宽急停/模式约束，不把 INFO/WARNING 自行升级为全局阻断。验收需覆盖故障输入更新和模式迁移之间的窗口，并验证拒绝不清标志。

## 3. ATOM02-BUG-020：AUTO_STOP 限制为 AUTO_FIRE

**优先级：P2，接口验收偏差。状态：✅ 已实施（2026-10-10，见 §5）。**

位置：EvaluateIntent 的 AUTO_STOP 模式检查及相应幂等停止提交路径。

Config 11.4 允许任意普通模式 auto_stop，当前仅接受 AUTO_FIRE。需要在无急停、阻断故障或优先级冲突时提供普通模式幂等停止结果，不把 IDLE/已终止流程恢复或启动，不新增 HTTP 路由。

验收覆盖 STANDBY、MANUAL、DEBUG、CALIBRATION 的无活动流程请求，以及 AUTO_FIRE 的活动/完成/终止状态。不得把 /api/control/auto/stop 的 stop_fire 更新语义混同于动作集 auto_stop 的流程终止语义。

## 4. 接入配合与仍需确认的边界

- 状态机仍是唯一业务权威，网关的互斥入口只负责串行转发，不替代本模块策略。
- 所有调用及读取经网关公共端口；不得从模块线程绕过入口使用 setter、Reset、UpdateInputs 或保存内部引用。
- 急停确认/解除的调用时机由 ATOM-14 负责。ReleaseSoftwareEstopLatch 不应由 WEB/SBUS 模块直接作为普通请求调用。
- 现有 SAFETY_RECOVERY 的模式类别放行不证明恢复阶段有效；FAULT 恢复失败锁定及不自动重试仍由 ATOM-14 按设计实施，不在此清单擅自修改安全豁免。
- WEB 清除若涉及先退出自动模式，需在发生业务副作用前完成来源/安全授权；与现有模式转换 API 的原子提交契约应由 ATOM-02/16 明确，不用网关另写优先级副本。
## 5. 实施记录（ATOM-02，2026-10-10）

本节只记录**已实施**内容，用于防止重复修复；未完成事项见 §1–§3、边界见 §4。

| 编号 | 结果 | 代码落点 | 实施要点与验收 |
|---|---|---|---|
| ATOM02-BUG-018 | ✅ 已实施 | `Src/StateArbitration.cpp` `EvaluateIntent` 的 `STOP_FIRE_CLEAR` 分支 | `source == WEB && control_source == SBUS` → `accepted=false` / `LOWER_PRIORITY_CONFLICT`，不修改标志、模式或自动流程，并产生拒绝事件；判别取 `control_source`（`Config.md` §7.2 选择结果）**而非** `sbus_online`——SBUS 在线且自动模式 `True`（控制源 `AUTO`）时不误拒；`source == SBUS` 不受该限制 |
| ATOM02-BUG-019 | ✅ 已实施 | 同一分支，紧随模式检查之后 | `IsFaultBlocking(inputs)`（仅 `SEVERE`/`ESTOP`）→ `FAULT_ACTIVE`；不放宽急停/模式约束，不把 `INFO`/`WARNING` 升级为全局阻断；拒绝时不清标志 |
| ATOM02-BUG-020 | ✅ 已实施 | `EvaluateIntent` 的 `AUTO_STOP` 检查；`RequestAutoFireState` 的 `AUTO_TERMINATED` 路径 | `auto_stop` 放开到任意普通模式（普通模式合法性由 `IsOperationalMode` 统一承担，仍受急停 / 阻断故障 / 优先级检查）；`AUTO_TERMINATED` 分支前移到终态与 `AUTO_FIRE` 前置之前——无活动流程（`IDLE`/`AUTO_COMPLETED`/`AUTO_TERMINATED`）幂等返回 `RejectReason::ALREADY_IN_TARGET`，不恢复、不启动；有活动流程时终止（`TransitionReason::AUTO_FLOW_ABORT`，不暂停、不恢复） |

**未改动**：`STOP_FIRE_SET` 的安全单调性；`SAFETY`/`SAFETY_RECOVERY` 分相豁免；发射授权的显式 `control_holder` 判据；`TerminateAutoFireOnSbusTakeover` 的 SBUS 接管语义（其 `IDLE` 分支仍返回 `AUTO_FLOW_NOT_STARTED`，属接管路径、不是 `auto_stop`，未与 §3 的语义混合）。

**语义分离保持**：`/api/control/auto/stop` 的 `stop_fire` 更新语义（`STOP_FIRE_SET`/`STOP_FIRE_CLEAR`）与动作集 `auto_stop` 的**流程终止**语义（`AUTO_STOP`）仍是独立意图类别。

本轮未执行编译；编译验证按 `PLAN.md` §6.3 由用户执行。
