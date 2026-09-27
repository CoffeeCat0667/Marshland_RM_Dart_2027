# ATOM-14_Safety_Recovery 编码指南（Guide）

## 0. 本文件定位

| 项目 | 内容 |
|---|---|
| 适用模块 | `Atoms/ATOM-14_Safety_Recovery`（安全、故障与恢复管理） |
| 依据（权威，只读） | `Design/Atomic/ATOM-14_Safety_Recovery.md`、`Design/PEF.md` §7–§9、`Design/Config.md` §6.2/§6.3/§14/§15、`Design/Interface.md` §17.3/§18 |
| 上游依赖（已实现） | `Atoms/ATOM-02_State_Arbitration`（状态机与控制权仲裁）、`Atoms/ATOM-01_Runtime`（硬件生命周期） |
| 装配层 | `Atoms/Public/AppCoordinator`（顶层桥接，**本模块不得依赖它**） |
| 编制日期 | 2026-09-27 |
| 性质 | 编码前置指导。目的：在编写 ATOM-14 时**直接避免已知缺陷**，而不是事后在 `BugLists.md` 里补记 |

本文件不是设计文档，不替代 `Design/Atomic/ATOM-14_Safety_Recovery.md`。当两者冲突时以 `Design/` 为准，并应同步修订本文件。

---

## 1. 单一职责与边界

**做什么**：故障登记与查询、普通命令安全闸门、一般故障自动恢复编排、软件急停锁定与人工解除编排、零力矩帧的持续发送。

**不做什么**（越界项，违反即为职责混淆）：

| 禁止事项 | 归属 |
|---|---|
| 修改 `SystemMode` 之外的权威判定逻辑、发射授权判定 | ATOM-02 |
| 直接构造/解析 CAN 报文、访问 SocketCAN | ATOM-04（`CANAdapter`） |
| 直接写 PCA9685A / 舵机通道、直接驱动电机 | ATOM-07 / ATOM-11 / ATOM-05 |
| 推进四发自动流程、判定发次 | ATOM-13（PEFExecutor） |
| 定义硬件就绪门控、启动结果映射 | `AppCoordinator` |
| 创建 `main()` | 禁止（上层工程职责） |

**调用方向**：本模块**持有** `StateArbitrator` 的引用（由装配层注入），**不持有** `RuntimeCoordinator`，**不得**包含 `AppCoordinator.hpp`。

---

## 2. 上游接口速查（ATOM-02 当前已实现）

以下签名取自 `Atoms/ATOM-02_State_Arbitration/Inc/StateArbitration.hpp` 与
`Atoms/Public/StateArbitrationTypes.hpp`，编码时以实际文件为准。

### 2.1 软件急停（两步，关键）

```cpp
// 第一步：人工确认。只做 SOFTWARE_ESTOP -> MANUAL，**保持急停变量与锁存为 true**
ModeTransitionResult ConfirmSoftwareEstop() noexcept;

// 第二步：解除生效。由 ATOM-14 在解除动作全部完成后调用
bool ReleaseSoftwareEstopLatch() noexcept;
```

### 2.2 急停输入（置位即锁存）

```cpp
ControlSourceChange SetSoftwareEstopInput(bool active) noexcept;
// active == true  → estop_latched_ = true（不可逆）
// active == false → 不会解除锁存，只会把已锁存的值写回 true
```

### 2.3 查询

```cpp
bool is_estop_latched() const noexcept;
bool is_software_estop() const noexcept;
const ExternalInputs& inputs() const noexcept;      // fault_active / fault_severity
SystemMode system_mode() const noexcept;
AutoFireState auto_fire_state() const noexcept;
```

### 2.4 安全动作必须走 SAFETY 意图

```cpp
IntentDecision SubmitIntent(const ControlIntent& intent) noexcept;
```

`IntentKind::SAFETY` 在 `EvaluateIntent` 中位于**急停 / 故障 / 模式闸门之前**，无条件放行：

```cpp
// StateArbitration.cpp（ATOM02-BUG-007）
if (intent.kind == IntentKind::SAFETY) {
    decision.accepted = true;
    return decision;
}
```

因此：**零力矩、换弹抬起、人工确认后的蓄力双电机回零、一般故障恢复动作，全部必须以
`IntentKind::SAFETY` 提交**。用普通 `IntentKind::CONTROL` 提交会被
`RejectReason::SOFTWARE_ESTOP` / `FAULT_ACTIVE` / `NOT_READY` 拒绝。

### 2.5 故障等级归约（不要自己复制一份）

```cpp
bool IsFaultBlocking(const ExternalInputs& inputs) noexcept;   // 已导出，公开纯函数
```

只有 `FaultSeverity::SEVERE` / `ESTOP` 全局阻断；`INFO` / `WARNING` 属可运行的降级状态。
`ExternalInputs::fault_severity` 缺省即为 `SEVERE`（fail-safe）。

---

## 3. 软件急停：完整交互契约

### 3.1 触发（可由 WEB 或 SBUS 发起）

```
1. 收到急停请求（WEB /api/control/estop，或 SBUS CH5）
2. 经 AppCoordinator::ReportSoftwareEstop(true) 上报 → SetSoftwareEstopInput(true)
   → estop_latched_ = true，SystemMode → SOFTWARE_ESTOP
3. 本模块立即执行锁定动作（见 3.2），并**持续保持**到解除
```

### 3.2 锁定期必须持续保持的行为（`PEF.md` §8.2）

1. **对所有电机持续发送零力矩 CAN 帧**，20 ms 周期，直到解除（见 §6）。
2. 换弹机构执行**抬起**动作组。
3. **拒绝一切操作发射机构舵机的指令**。
4. 禁止自动发射、连续发射和任何新的发射流程步骤。
5. 保持锁定，直至人工确认解除。

### 3.3 人工确认与解除（**本指南最关键的防错段**）

`Config.md` §15 规定：急停变量「**人工确认并完成解除动作后**才设为 `False`」。
`PEF.md` §8.3 与 `ATOM-14` 第 7 条规定解除动作内容与判据。

正确的调用序列：

```text
外部确认请求（WEB /api/control/estop/confirm，或 SBUS 人工确认通道）
        │
        ▼
[1] authority.ConfirmSoftwareEstop()
        → SystemMode: SOFTWARE_ESTOP -> MANUAL
        → estop_latched_ 仍为 true，software_estop 仍为 true
        → 此刻**不得**恢复任何普通执行器命令
        │
        ▼
[2] 以 IntentKind::SAFETY 提交并执行解除动作：
        (a) 换弹机构抬起动作组（动作组角度命令发送后 500 ms 判定到位）
        (b) 蓄力机构两个电机（电机 1、2）回零
        │
        ▼
[3] 判定回零完成：**两个蓄力限位开关均为低电平**（Config §8.1）
        │  未满足则继续等待/重试，不得提前进入 [4]
        ▼
[4] authority.ReleaseSoftwareEstopLatch()
        → 返回 true 表示锁存与急停变量已清除
        │
        ▼
[5] 解除完成。**不重新初始化**、**不自动恢复原自动发射流程**
```

### 3.4 必须避免的错误（每一项都对应一个已发生的真实缺陷）

| # | 错误写法 | 后果 | 正确做法 |
|---|---|---|---|
| E1 | 把 `ConfirmSoftwareEstop()` 当作"急停已解除" | `SystemMode` 已是 `MANUAL`，但锁存仍为 `true`；若此时放行执行器，违反 `Config.md` §15 / `PEF.md` §8.3 | 确认与解除分离；只有 `ReleaseSoftwareEstopLatch()` 返回 `true` 才算解除 |
| E2 | 依赖 `AppCoordinator::RunGate()` 来"挡住"确认后、解除前的执行器命令 | 本层无法保证调用方一定查闸门；且把安全边界寄托在外部 | 锁定期内由本模块拒绝普通命令，闸门只是第二道防线 |
| E3 | 确认后立刻停止零力矩帧 | 解除动作本身（换弹抬起、蓄力回零）应在锁定保护下进行，此时电机仍需零力矩 | 零力矩持续到 `ReleaseSoftwareEstopLatch()` 成功之后 |
| E4 | 用 `IntentKind::CONTROL` 提交解除动作 | 被 `RejectReason::SOFTWARE_ESTOP` 拒绝，解除流程卡死 | 一律用 `IntentKind::SAFETY` |
| E5 | 用 `SetSoftwareEstopInput(false)` 试图解除 | 该方法**不可能**解除锁存，只会写回 `true` | 只能经 `ConfirmSoftwareEstop()` + `ReleaseSoftwareEstopLatch()` |
| E6 | 把"人工确认"实现为模式切换后就绪 | 见 E1 | 见 3.3 序列 |
| E7 | 解除后自动恢复原自动流程 | 违反 `PEF.md` §8.3 第 6 条 | 解除后停在 `MANUAL`，由上层重新发起 |
| E8 | 解除后执行重新初始化 | 违反 `Config.md` §2「不重新初始化」 | 不调用任何初始化路径。**注意：产品已裁决"不支持运行中重启"**，因此本工程不存在"重新初始化"这条路径；急停解除只能走 `ConfirmSoftwareEstop()` + 解除动作 + `ReleaseSoftwareEstopLatch()` |

---

## 4. 一般故障自动恢复：完整交互契约

### 4.1 触发条件（`Config.md` §14.2）

故障根因清除**且**关联接口恢复有效后才触发。`W-VISION-002`（视觉无响应超时）属**正常回退路径**，
**不触发**一般故障恢复。

### 4.2 固定顺序（`PEF.md` §7，不得调换）

```text
[1] 蓄力机构电机 1、2 回零
        → 以 IntentKind::SAFETY 提交
[2] 判定回零完成：两个限位开关均为低电平
[3] 仅发射机构执行三次发射动作
        每次 = x 号舵机 待机角度 → 发射角度 → 保持 100 ms → 待机角度
        舵机回到待机角度后判定本次完成
[4] 三次动作期间：其他机构电机保持零力矩；其他舵机角度保持不变
[5] 三次完成后：换弹机构执行抬起动作组
        动作组角度命令发送后 500 ms 判定到位
[6] 恢复完成 → 进入 MANUAL
        **不恢复原自动发射上下文**
```

### 4.3 必须避免的错误

| # | 错误写法 | 后果 | 正确做法 |
|---|---|---|---|
| F1 | 用 `IntentKind::CONTROL` 提交恢复动作 | `RECOVERY` 不在 `IsOperationalMode` 集合内 → `NOT_READY`，恢复卡死 | 一律 `IntentKind::SAFETY` |
| F2 | 恢复期间让其他机构电机继续出力 | 违反 `PEF.md` §7 第 5 条 | 恢复期间持续零力矩 |
| F3 | 恢复期间改动其他舵机角度 | 违反同上 | 保持当前角度 |
| F4 | 回零判据用"单限位低"或"超时即算完成" | 违反 `Config.md` §8.1 | 两个限位**均为**低电平 |
| F5 | 恢复完成后自动续跑原自动流程 | 违反 `PEF.md` §7 第 9 条 | 进入 `MANUAL`，不续跑 |
| F6 | 恢复与急停锁定并行推进 | 急停锁定的优先级更高 | 急停锁定期间不启动一般故障恢复 |
| F7 | 把 `INFO`/`WARNING` 级故障也升级为全局阻断 | 例如 `W-SBUS-001`、`F-VISION-001` 本应可运行 | 用 `IsFaultBlocking(inputs)` 判定，不要自建等级表 |

---

## 5. 安全动作集合（必须用 `IntentKind::SAFETY`）

`Config.md` §15「安全动作例外」+ `Interface.md` §17.3 第 7 条明确列出：

- 零力矩报文发送
- 换弹机构抬起
- 人工确认解除后的蓄力机构两个电机回零
- 一般故障恢复中的回零、三次发射、换弹抬起

这些动作**不受普通命令拒绝规则影响**。在代码中应集中为一个显式的构建函数，避免逐处手写：

```cpp
// 建议（示意，最终命名以本模块实现为准）
ControlIntent MakeSafetyIntent(IntentKind kind_or_control, std::uint32_t request_id) noexcept
{
    ControlIntent intent{};
    intent.valid      = true;
    intent.kind       = IntentKind::SAFETY;   // 关键
    intent.source     = ControlSource::AUTO;  // SAFETY 不参与优先级排序
    intent.request_id = request_id;
    return intent;
}
```

**反例**：把 `intent.kind` 留作默认值 `IntentKind::CONTROL`，是 E4/F1 类缺陷的共同根因。

---

## 6. 零力矩 CAN 帧（`Config.md` §6.2、§6.3）

软件急停期间必须**持续发送**，直到 `ReleaseSoftwareEstopLatch()` 成功。

| 项 | 值 |
|---|---|
| 帧 1 | CAN ID `0x200`，DLC 8，数据全零：`1.200#0000000000000000` |
| 帧 2 | CAN ID `0x1FF`，DLC 8，数据全零：`2.1FF#0000000000000000` |
| 周期 | 20 ms（与 CAN 发送周期一致） |
| 字节序 | 大端序（本项目全部多字节量统一大端） |
| 发送方式 | 开环发送，不闭环确认 |
| 失败处理 | 进入错误状态并执行**全机静默**；故障码见 `Config.md` §14.2 |

**编码约束**：

1. 帧的**构造与发送**委托 ATOM-04（`CANAdapter`），本模块只负责"何时发、发多久"。
2. 覆盖**全部受控电机**：电机 1～4（`0x201`～`0x204`）由 `0x200` 覆盖；`0x1FF` 覆盖 `0x205`～`0x208`。两帧都发，不要只发一帧。
3. 用单调时钟计时（`Interface.md` §3.3），不要用墙上时钟。
4. 不要在急停解除后立刻停发——应等解除动作完成。

**必须避免的错误**：

| # | 错误 | 后果 |
|---|---|---|
| Z1 | 只发 `0x200` 不发 `0x1FF` | 高编号电机未进入零力矩 |
| Z2 | 用小端序写字节 | 虽然全零帧看不出差别，但一旦改为非零力矩帧即出错；统一大端 |
| Z3 | 发送失败静默忽略 | 违反 `Config.md` §6.3，必须进入错误状态并全机静默 |
| Z4 | 用"发一次就够"代替持续发送 | 违反 §6.3「持续发送直到解除」 |

---

## 7. 故障管理接口（`Interface.md` §18.2）

必须提供：当前故障、历史故障、故障发生时间、故障来源、故障严重级别、自动恢复状态、
人工确认状态、故障清除结果、向 WebSocket 推送故障事件。

**约束**：

1. 故障码族（`F-CAN-001`、`E-ESTOP-001` 等）与等级全部取自 `Config.md` §14.2，**不得自造**。
2. 历史故障保留 1000 条、环形保留、最新优先（`Config.md` §16.1）。
3. 故障事件必须带时间戳；时间取自单调时钟，展示层再格式化为 UTC ISO 8601。
4. 不得把故障管理做成"修改控制命令"的通道——`Interface.md` §5.1 明确 `Diagnostics` 不修改控制命令。
5. 等级归约复用 `state_arbitration::IsFaultBlocking()`，不要在本模块另建一份等级→阻断的映射（避免双份语义）。

---

## 8. 与装配层的边界

| 事项 | 归属 |
|---|---|
| 启动结果 → `SystemMode` 映射、硬件就绪门控、运行闸门 `RunGate()` | `AppCoordinator` |
| 外部输入的唯一写入路径（`ReportXxx`） | `AppCoordinator` |
| 急停**触发**的事实上报 | 外部来源 → `AppCoordinator::ReportSoftwareEstop(true)` |
| 急停**确认/解除的动作编排** | **本模块**（ATOM-14） |
| 一般故障恢复的动作编排 | **本模块** |
| `RunGateReason::ESTOP_LATCHED` / `FAULT_ACTIVE` | `AppCoordinator`（读取 `is_estop_latched()` 与 `IsFaultBlocking()`） |

**禁止**：本模块包含 `AppCoordinator.hpp`。急停确认与解除的**调用时机**由本模块掌握，
但 `StateArbitrator` 的引用由装配层注入，本模块不需要也不应该经过桥接层。

### 8.1 与"不支持运行中重启"裁决的关系（重要）

产品裁决（2026-09-27，见 `ATOM02-BUG-015`）：**不支持运行中重启**，`SystemMode`
一旦离开 `UNINITIALIZED` 就不可回到可初始化状态；`AppCoordinator::Start()` 在
`SystemMode != UNINITIALIZED` 时返回 `kRestartNotSupported`。

该裁决与 `PEF.md` §8.3 第 3 条「软件急停人工解除后**不需要重新初始化**」在语义上一致，
并且对本模块有两条直接约束：

1. **急停解除路径中不存在"重新初始化"分支**。解除只能走
   `ConfirmSoftwareEstop()` → 解除动作 → `ReleaseSoftwareEstopLatch()`；
   任何"先 shutdown 再 start"的写法都会失败并返回 `kRestartNotSupported`。
2. **不要用 `StateArbitrator::Reset()` 来"清干净"**。`Reset()` 会清空事件与
   丢弃计数，而 `Interface.md` §7.2 第 6 条要求状态迁移事件**可查询**；
   且本工程没有重启路径，`Reset()` 只应用于进程启动时的首次初始化。

---

## 9. 状态与并发约束（`Config.md` §15）

1. 软件急停为**统一 `bool` 变量**；`True` 表示锁定，解除动作完成后才置 `False`。
2. 执行器执行普通动作前**必须**检查该变量；为 `True` 时拒绝普通命令。
3. 安全动作（零力矩、换弹抬起、确认后的蓄力回零）不受该拒绝规则影响。
4. 急停响应上限 **100 ms**（`Config.md` §4）。
5. 线程调度为 Linux `SCHED_OTHER`，固定周期任务用单调时钟**绝对期限**并监测超期；
   这是尽力调度，**不构成硬实时保证**——不要假设回调一定准时到达。
6. 本模块不应阻塞控制与安全任务；网络与日志写入不得放在急停路径上同步执行。

---

## 10. 编码完成前的自检清单

- [ ] 急停确认与解除是**两个独立步骤**，解除只在 `ReleaseSoftwareEstopLatch()` 返回 `true` 后完成（E1/E6）
- [ ] `ConfirmSoftwareEstop()` 之后、解除完成之前，不存在任何放行普通执行器命令的路径（E2）
- [ ] 零力矩帧持续发送至解除动作完成之后，两帧（`0x200` + `0x1FF`）都发（E3/Z1/Z4）
- [ ] 所有安全动作以 `IntentKind::SAFETY` 提交（E4/F1）
- [ ] 未使用 `SetSoftwareEstopInput(false)` 作为解除手段（E5）
- [ ] 解除后不重新初始化、不自动恢复原自动流程（E7/E8）
- [ ] 一般故障恢复顺序为"双电机回零 → 双限位均低 → 仅发射机构三次 → 其他电机零力矩/其他舵机不动 → 换弹抬起 → MANUAL"（F2/F3/F4/F5）
- [ ] 急停锁定期间不推进一般故障恢复（F6）
- [ ] 故障等级归约复用 `IsFaultBlocking()`，未自建等级表（F7）
- [ ] 故障码与等级全部取自 `Config.md` §14.2，无自造码
- [ ] 未包含 `AppCoordinator.hpp`，未直接访问 SocketCAN / I2C / 电机寄存器
- [ ] 所有时间使用单调时钟
- [ ] 头文件在 `Inc/`、源码在 `Src/`、无 `main()`、CMake 目标不设全局编译选项
- [ ] 在 `-Wall -Wextra -Wpedantic` 下零警告（`ATOM-02_PLAN.md` §6.3 的工程约定）

---

## 11. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.1 | 2026-09-27 | 首次编制。依据 ATOM-02 修复轮后的实际接口（急停确认/解除分离、`IntentKind::SAFETY`、`IsFaultBlocking`）与 `AppCoordinator` 桥接层边界，形成软件急停、一般故障恢复、零力矩帧三大契约与 E1–E8 / F1–F7 / Z1–Z4 防错清单。目的：在编码前消除已知缺陷，而非事后补记 `BugLists.md`。 |
