# ATOM-02_State_Arbitration 缺陷清单（BugLists）

## 0. 文档定位

| 项目 | 内容 |
|---|---|
| 对象模块 | `Atoms/ATOM-02_State_Arbitration` |
| 权威来源 | `Design/Atomic/ATOM-02_State_Arbitration.md`；`Design/Interface.md` §3.2、§6、§7、§8、§9、§17.3、§18.2；`Design/Config.md` §7.2、§11.4、§14、§15；`Design/PEF.md` §5、§6、§7、§8.3 |
| 维护约定 | **只登记未修复缺陷。** 已修复或已裁决关闭的缺陷描述一律删除，仅在 §3 留一行标题供追溯与防止重复报告 |
| 更新日期 | 2026-09-27 |

---

## 1. 未修复缺陷

| 编号 | 标题 | 严重度 | 位置 |
|---|---|---|---|
| ATOM02-BUG-004 | 急停「人工确认 → 解除生效」两步在本模块已就绪，但**全工程无代码调用点**，锁存目前无法解除 | 中（需 ATOM-14 实施） | `Src/StateArbitration.cpp` |
| ATOM02-BUG-007 | 安全动作豁免的调用方契约：发起安全动作者必须按类别使用 `IntentKind::SAFETY` / `SAFETY_RECOVERY` | 中（需 ATOM-14 遵守） | `Src/StateArbitration.cpp` |

---

## 2. 缺陷详述

### ATOM02-BUG-004 急停两步语义已就绪，但全工程无调用点

**严重度**：中（本模块无代码待改，等待调用方实施）

**现状**

本模块已实现正确的两步语义：

```cpp
ModeTransitionResult ConfirmSoftwareEstop() noexcept;        // SOFTWARE_ESTOP -> MANUAL，保持锁存
bool                ReleaseSoftwareEstopLatch() noexcept;    // 解除生效，返回是否成功
```

且 `RequestModeTransition` 的 `ESTOP_CONFIRM` 分支**不再**清零 `software_estop` 与锁存。

**仍存在的问题**：经代码检索，`ConfirmSoftwareEstop()` 与 `ReleaseSoftwareEstopLatch()`
**没有任何调用点**（仅在 `Atoms/ATOM-14_Safety_Recovery/Guide.md` 的文档序列中出现，
该模块当前只有 AGENTS/CMakeLists/Guide/PLAN，尚无 Inc/Src）。因此急停一旦触发，
锁存**永久无法解除**。

**影响**：`Atoms/Public/AppCoordinator::RunGate()` 会持续返回 `ESTOP_LATCHED`，
所有普通执行器命令被拒绝；设备只能重启进程才能恢复。

**归属**：调用时机与编排（换弹抬起 + 蓄力双电机回零 + 双限位均低电平）属
`ATOM-14_Safety_Recovery`。已编写 `Atoms/ATOM-14_Safety_Recovery/Guide.md` §3.3 给出
完整调用序列与 E1–E8 防错清单。

**依据**：`Design/Config.md` §15（急停变量须"人工确认并完成解除动作后"才置 `False`）；
`Design/PEF.md` §8.3；`Design/Atomic/ATOM-14_Safety_Recovery.md` 第 7 条。

---

### ATOM02-BUG-007 安全动作豁免的调用方契约

**严重度**：中（契约性；本模块侧的粒度问题已在 ATOM02-BUG-017 修复）

**现状**：安全动作豁免已按**阶段**实现：`IntentKind::SAFETY`（不出力）与
`IntentKind::SAFETY_RECOVERY`（出力，仅人工确认后放行）。
`IsOperationalMode` 刻意**未**放宽（不含 `RECOVERY` / `SOFTWARE_ESTOP`），这是正确设计。

**仍存在的问题**：该豁免是**纯契约性**的——调用方若用 `IntentKind::CONTROL` 提交安全/恢复动作，
会被 `NOT_READY` / `SOFTWARE_ESTOP` 拒绝，而状态机侧无任何机制能发现这一误用。

**需调用方遵守的契约**（`ATOM-14` 及一切发起安全动作者）：

| 动作 | 必须使用的意图类别 | 允许阶段 |
|---|---|---|
| 零力矩报文发送 | `IntentKind::SAFETY` | 急停锁定阶段即可 |
| 换弹机构抬起、蓄力双电机回零（人工确认后的解除动作） | `IntentKind::SAFETY_RECOVERY` | 仅人工确认之后 |
| 一般故障恢复中的回零、三次发射、换弹抬起 | `IntentKind::SAFETY_RECOVERY` | `RECOVERY` / `FAULT` 期间 |

**依据**：`Design/Config.md` §15「安全动作例外」；`Design/Interface.md` §17.3 第 7 条。
详见 `Atoms/ATOM-14_Safety_Recovery/Guide.md` §5 与 F1 防错条目。

---

## 3. 已修复 / 已裁决项索引（描述已按维护约定删除）

| 编号 | 标题 | 状态 |
|---|---|---|
| ATOM02-BUG-001 | 自动流程缺 `FORCE → SHOT_N_YAW` 边，`SHOT_N_YAW` 为孤儿节点 | 已修复（`ShotStage` 消歧 + `current_shot`） |
| ATOM02-BUG-002 | 用控制源 rank 比较代替实际发射授权优先级 | 已修复（授权函数新增显式 `control_holder` 入参；无人持有 → `NO_CONTROL_AUTHORITY`；`priority_conflict` 由判定回填） |
| ATOM02-BUG-003 | 手动/调试模式的 `fire_once` 无合法提交路径 | 已修复（`AuthorizeManualFire()`） |
| ATOM02-BUG-005 | `UpdateInputs` 整体替换导致模块间输入互相覆盖 | 已修复（7 个字段级 setter；`stop_fire` 初值对齐 `Config.md` §2） |
| ATOM02-BUG-006 | `fault_severity` 从未参与判定 | 已修复（`IsFaultBlocking()`，仅 `SEVERE`/`ESTOP` 阻断） |
| ATOM02-BUG-008 | `run_id_` 存在两条递增路径 | 已修复（收敛为 `BeginNewAutoFireRun()`） |
| ATOM02-BUG-009 | `STOP_FIRE_SET` 缺少模式校验 | 已修复 |
| ATOM02-BUG-010 | 未启动时允许进入 `AUTO_TERMINATED` | 已修复（幂等空操作 + `AUTO_FLOW_NOT_STARTED`） |
| ATOM02-BUG-011 | `InitFailed → FAULT` 通路未关闭 | 已修复 |
| ATOM02-BUG-012 | 事件缓冲 64 条且静默覆盖 | 已修复（`dropped_event_count()`） |
| ATOM02-BUG-013 | 仲裁类型未落入 `Atoms/Public` | 已修复（上提 `StateArbitrationTypes.hpp`） |
| ATOM02-BUG-014 | `-Wunused-parameter` 疑似告警 | 已关闭（编译零警告） |
| ATOM02-BUG-015 | 「不支持运行中重启」的裁决需在设计中固化 | 已裁决关闭（**不补 `Design/` 文档**，维持默认：不支持运行中重启；ATOM-01 侧 `Stopped → Created` 由 `ATOM01-BUG-003` 继续跟踪） |
| ATOM02-BUG-016 | `IsOperationalMode` 未导出，上层复制同一判据 | 已修复（导出 `IsOperationalMode()`；`AppCoordinator` 删除副本并改为 `state_arbitration::IsOperationalMode()`） |
| ATOM02-BUG-017 | `IntentKind::SAFETY` 粒度过粗：锁定阶段放行出力动作 | 已修复（按方案 1 拆分：`SAFETY` 不出力 / `SAFETY_RECOVERY` 出力，后者在 `SystemMode == SOFTWARE_ESTOP` 时以 `SOFTWARE_ESTOP` 拒绝） |

## 4. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.6 | 2026-09-27 | 按维护约定精简：删除全部已修复缺陷的详述、过时的"§2.1 修复方案"与历史修复表；仅保留 5 条未修项（002/004/007/015/016）；§3 仅留已修复项标题索引。 |
| 0.7 | 2026-09-27 | 复核轮：逐条核实 0.6 的 5 条未修项。002 已修复（授权函数改为显式 `control_holder` 入参）；016 已修复（导出 `IsOperationalMode`，公共层删副本）；015 按裁决关闭；新发现并登记 017；007 调整为纯调用方契约。 |
| 0.8 | 2026-09-27 | 017 按方案 1 修复：`IntentKind` 拆分为 `SAFETY`（不出力，锁定阶段即可）与 `SAFETY_RECOVERY`（出力，仅人工确认后；`SystemMode == SOFTWARE_ESTOP` 时拒绝）。§1 仅余 004、007 两条 ATOM-14 侧事项。 |
