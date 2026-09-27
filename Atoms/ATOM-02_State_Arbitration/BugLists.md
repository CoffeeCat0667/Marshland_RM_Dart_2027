# ATOM-02_State_Arbitration 缺陷清单（BugLists）

## 0. 文档定位

| 项目 | 内容 |
|---|---|
| 对象模块 | `Atoms/ATOM-02_State_Arbitration` |
| 权威来源 | `Design/Atomic/ATOM-02_State_Arbitration.md` |
| 接口权威 | `Design/Interface.md` §3.2、§6、§7、§8、§9、§18.2 |
| 配置权威 | `Design/Config.md` §3、§7.1、§7.2、§11.4、§14、§15 |
| 流程权威 | `Design/PEF.md` §1.4、§3、§5、§8.3 |
| 登记日期 | 2026-09-27 |
| 本轮行为 | **仅登记缺陷，未修改任何 Atom 代码**；`Inc/`、`Src/`、`CMakeLists.txt` 均保持原样 |

本文件只记录本次审查发现的缺陷与风险，不提出实现方案、不产出补丁、不修改任何源码。所有条目均给出"证据"，即具体文件与行号，便于逐条复核。

### 0.1 本次审查范围

- `Inc/StateArbitration.hpp`（307 行）
- `Src/StateArbitration.cpp`（764 行）
- `PLAN.md`（138 行，用于区分"已知未完成"与"实现缺陷"）
- `Atoms/Public/MarshlandTypes.hpp`（153 行，作为权威枚举来源对照）

### 0.2 已确认正确、不作为缺陷登记的行为

为避免后续复核重复排查，以下行为经逐条核对后**判定正确**，是本模块值得保留的设计优点：

1. 纯函数（`SelectControlSource`、`EvaluateIntent`、`EvaluateFireAuthorization`、`IsModeTransitionAllowed`、`IsAutoFireTransitionAllowed`）无副作用，状态提交集中在显式提交接口，符合头注释 L17–21 的"无副作用约定"。
2. `AUTO_TERMINATED` / `AUTO_COMPLETED` 终态保护（L567–570）正确实现 `ATOM-02.md` 原子操作 11「已终止不得续跑」。
3. `TerminateAutoFlowInternal`（L466–478）在进入急停、进入故障、SBUS 接管三条路径上均被调用，符合 `PEF.md` §9 第 5 条"不暂停、不恢复"。
4. `SBUS` 接管以「SBUS 在线且自动模式标志为 `False`」为前置条件（L650–653），与 `Config.md` §7.2 第 4 行一致。
5. `estop_latched_` 锁存使外部把急停 bool 写回 `false` 也无法解除，符合 `Config.md` §15。
6. 环形事件缓冲的 `CopyEvents` / `latest_event` 下标运算（L742–761）经逐项核对**逻辑正确**：容量、取模、最旧到最新顺序、`ClearEvents` 后复位均无越界或错序。
7. 全部公开接口 `noexcept`、零堆分配（`std::array` 固定 64 条）、无硬件与 OS 依赖，符合 `Config.md` §15「状态机任务优先级高、固定周期」的定位。
8. `Reset()`（L366–379）正确复位全部成员，包括 `estop_latched_` 与 `run_id_`。

---

## 1. 缺陷汇总

| 编号 | 标题 | 严重度 | 状态 | 修复位置 / 说明（2026-09-27 修复轮） |
|---|---|---|---|---|
| ATOM02-BUG-001 | 自动流程第 2～4 发在状态机中走不通：缺 `FORCE → SHOT_N_YAW` 边，`SHOT_N_YAW` 成为孤儿节点 | 高 | ✅ 已修复 | `Src/StateArbitration.cpp` 自动流程边表 + 新增 `ShotStage` 消歧；`current_shot` 约束第 4 发 |
| ATOM02-BUG-002 | `EvaluateFireAuthorization` 用控制源数值大小替代实际发射授权优先级，可永久阻断自动流程发射并反向绕过 SBUS 检查 | 高 | ✅ 已修复（方案 B） | 授权函数只做前置条件复核；优先级移到 `EvaluateIntent` 的 FIRE 分支（原子操作 8 的落点） |
| ATOM02-BUG-003 | `RequestAutoFireState(FIRING)` 硬编码请求源为 `AUTO`，手动/调试模式下的发射动作无合法提交路径 | 高 | ✅ 已修复 | 新增 `AuthorizeManualFire()`（`Config.md` §11.4 `fire_once`，手动/调试） |
| ATOM02-BUG-004 | 人工确认解除急停时立即清零 `software_estop`，早于设计的解除动作完成时点 | 高 | ✅ 已修复 | `ConfirmSoftwareEstop()` 只切模式并保持锁存；新增 `ReleaseSoftwareEstopLatch()`；确认后控制源不再恒为 `NONE` |
| ATOM02-BUG-005 | `UpdateInputs` 整体替换输入快照，与头注释声明的多方分别写入契约矛盾 | 中 | ✅ 已修复 | 新增 7 个字段级 setter；`stop_fire` 初值改为 `true`（`Config.md` §2）；`UpdateInputs` 保留并注明仅限单一写入方 |
| ATOM02-BUG-006 | `fault_severity` 被存储但从未参与任何决策，故障等级形同虚设 | 中 | ✅ 已修复 | 新增 `IsFaultBlocking()`：仅 `SEVERE`/`ESTOP` 全局阻断；`fault_severity` 缺省 `SEVERE`（fail-safe） |
| ATOM02-BUG-007 | `IsOperationalMode` 不含 `RECOVERY`，一般故障恢复期间的安全动作会被状态机拒绝 | 中 | ✅ 已修复 | 新增 `IntentKind::SAFETY` 豁免（`Config.md` §15、`Interface.md` §17.3 第 7 条），不放宽模式闸门 |
| ATOM02-BUG-008 | `run_id_` 存在两条递增路径，公开接口重复 | 中 | ✅ 已修复 | `run_id` 收敛为 `BeginNewAutoFireRun()` 唯一入口；`RequestAutoFireState(AUTO_PREPARE)` 转交之 |
| ATOM02-BUG-009 | `IntentKind::STOP_FIRE_SET` 分支缺少模式检查 | 低 | ✅ 已修复 | 补 `MANUAL`/`DEBUG` 模式校验（`Config.md` §11.4），保留安全单调性（不受优先级/急停阻断） |
| ATOM02-BUG-010 | `RequestAutoFireState` 在自动流程未起步时仍允许进入 `AUTO_TERMINATED` | 低 | ✅ 已修复 | `IDLE` = 未启动（等价于已停止）：`auto_stop` / SBUS 接管终止为幂等空操作，新增 `RejectReason::AUTO_FLOW_NOT_STARTED` |
| ATOM02-BUG-011 | `RequestModeTransition` 为 `InitFailed → MANUAL` 留出通路 | 低 | ✅ 已修复 | `FAULT_DETECTED` 通配边排除 `INIT_FAILED`（ATOM-01 验收条件、`Config.md` §3.1） |
| ATOM02-BUG-012 | 事件缓冲仅 64 条，20 ms 推送周期下覆盖速度快于消费速度 | 低 | ✅ 已修复 | 新增 `dropped_event_count()` 显式丢弃计数，仍零堆分配 |
| ATOM02-BUG-013 | 仲裁输入/意图/结果类型未落入 `Atoms/Public`，导致 6 个模块横向依赖本模块私有头 | 中 | ✅ 已修复 | 类型上提 `Atoms/Public/StateArbitrationTypes.hpp` 并登记进 `Atoms/Public/CMakeLists.txt` |
| ATOM02-BUG-014 | 类内定义且带未使用形参的成员函数在 `-Wextra` 下可能触发 `-Wunused-parameter` | 低 | ✅ 已关闭 | aarch64 交叉编译零警告，判定不成立（见 §2 该条状态） |

---

## 2. 缺陷详述

> **状态提示（2026-09-27 修复轮）**：以下 §2 各条为首次审查的**历史记录**，逐条状态以 §1 汇总表的
> 「状态」列与 §4「修复状态」为准。ATOM02-BUG-001～013 已全部修复、014 已关闭，
> **已修项请勿重复修复**；如需变更，请先复核 §4 列出的修复位置与回归验收点。

### ATOM02-BUG-001 自动流程第 2～4 发在状态机中走不通：缺 `FORCE → SHOT_N_YAW` 边，`SHOT_N_YAW` 成为孤儿节点

**严重度**：高

**现象**

```cpp
// Src/StateArbitration.cpp L57-74
constexpr AutoFireEdge kAutoFireEdges[] = {
    {IDLE,           AUTO_PREPARE},
    {AUTO_PREPARE,   CHARGE},
    {CHARGE,         FORCE},
    {CHARGE,         RELOAD_LIFT},
    {FORCE,          SHOT_1_YAW},      // 仅指向第 1 发 YAW
    {FORCE,          RELOAD_LIFT},
    {SHOT_1_YAW,     FIRE_READY},
    {RELOAD_LIFT,    FEED},
    {FEED,           FEED_WAIT},
    {FEED_WAIT,      RELOAD_LOWER},
    {RELOAD_LOWER,   FORCE},
    {SHOT_N_YAW,     FIRE_READY},      // 有出边、无入边
    {FIRE_READY,     FIRING},
    {FIRING,         FIRE_COMPLETED},
    {FIRE_COMPLETED, CHARGE},
    {FIRE_COMPLETED, AUTO_COMPLETED},
};
```

**设计要求（逐字）**

- `Design/Interface.md` §7.1 状态转换总表：「换弹后变力 | 变力到位 | **下一发 YAW** | 控制视觉 YAW」。
- `Design/Interface.md` §8.2 第 2～4 发流程：「→ FORCE：变力动作（X 秒）→ **SHOT_N_YAW**」。
- `Design/PEF.md` §5.2 第 9–10 条：换弹放下后「变力机构执行一次变力动作」→「视觉系统提供本次指定 YAW 控制量」。

**推导出的必然死路**

第 2 发从 `FIRE_COMPLETED` 之后进入 `CHARGE`（L72），此后：

| 步 | 当前状态 | 期望后继（PEF） | 表中是否存在 |
|---|---|---|---|
| 1 | `CHARGE` | `RELOAD_LIFT` | ✅ L61 |
| 2 | `RELOAD_LIFT` | `FEED` | ✅ L65 |
| 3 | `FEED` | `FEED_WAIT` | ✅ L66 |
| 4 | `FEED_WAIT` | `RELOAD_LOWER` | ✅ L67 |
| 5 | `RELOAD_LOWER` | `FORCE` | ✅ L68 |
| 6 | `FORCE` | **`SHOT_N_YAW`** | ❌ **不存在** |

`FORCE` 在表中只有 `SHOT_1_YAW` 与 `RELOAD_LIFT` 两条出边：

- 取 `FORCE → SHOT_1_YAW`：随后 `SHOT_1_YAW → FIRE_READY` 走通，第 2 发确实能发射（详见下文），但状态语义错误，且第 3 发会从 `CHARGE → FORCE` 再次进入 `SHOT_1_YAW`（因为 `FIRE_COMPLETED` 无条件回 `CHARGE`），**`AutoFireState` 永远无法体现"这是第 N 发"**。
- 取 `FORCE → RELOAD_LIFT`：形成 `RELOAD_LIFT → FEED → FEED_WAIT → RELOAD_LOWER → FORCE → RELOAD_LIFT` 的**无限循环**，且需要从 `FORCE` 直接跳回 `RELOAD_LIFT`，多执行一次换弹。

**`SHOT_N_YAW` 出边存在（L69）而无任何入边**，是"边表漏行"的直接证据。

**竞态分析（额外发现）**

`RequestAutoFireState(AUTO_COMPLETED)` 依赖 `FIRE_COMPLETED` 作为唯一前驱（L73）。由于 `FIRE_COMPLETED → CHARGE`（L72）与 `FIRE_COMPLETED → AUTO_COMPLETED`（L73）同时存在，**第 4 发完成后也必须由调用方显式选择走哪条边**。若 `PEFExecutor` 判定发次时出现偏差（例如把第 4 发误判为第 3 发），流程会回到 `CHARGE` 而不进入终态。表中没有任何机制以发次为约束限制该分支，`AutoFireContext.total_shots = 4`（`Interface.md` §6.6）在本模块中**没有对应的状态或校验**。

**修复注意事项（仅记录，不在本轮实施）**

不能简单地补一行 `{FORCE, SHOT_N_YAW}`，因为那样 `FORCE` 将同时拥有 `SHOT_1_YAW` 与 `SHOT_N_YAW` 两个合法后继，而 `IsAutoFireTransitionAllowed(from, to)`（L94–102）的签名**只吃状态对，拿不到发次信息**，无法消歧。因此修复必须同时引入"发次/阶段"入参或把发次纳入转换条件，否则会从"走不通"退化为"走错路"。

---

### ATOM02-BUG-002 `EvaluateFireAuthorization` 用控制源数值大小替代"实际发射授权优先级"，可永久阻断自动流程发射并反向绕过 SBUS 检查

**严重度**：高

**现象**

```cpp
// Src/StateArbitration.cpp L187-192
// 低优先级来源不得越过更高优先级的控制源
if (ControlSourceRank(requesting_source) < ControlSourceRank(ctx.control_source)) {
    auth.priority_conflict = true;
    auth.reason = RejectReason::LOWER_PRIORITY_CONFLICT;
    return auth;
}
```

**证据（设计要求）**

`Design/Atomic/ATOM-02_State_Arbitration.md` L20–38 把两者定义为**必须分开建模**的两个量：

- 「当前控制源选择」：由 `Config.md` §7.2 五行规则决定（有无 WEB 连接、SBUS 是否在线、自动模式标志、是否失联）。
- 「实际发射授权」：始终严格为 `SBUS > WEB > 自动流程`。

`Design/Interface.md` §20 亦强调：「`Config.md` 是上述具体配置的唯一来源。实现不得在代码中创建与配置表冲突的隐含默认值。」

**两处错误**

**错误一：自动流程的发射请求会被控制源数值阻断。** `Config.md` §7.2 第 3 行：SBUS 在线且自动模式标志为 `True` → 控制源为 `AUTO`；第 4 行：标志为 `False` → 控制源为 `SBUS`。而 `Config.md` §7.1 注 1 规定 CH6「自动模式」与「手动模式」互斥。因此在"SBUS 在线且自动模式标志为 `False`"这一**合法且常见**的状态下，`ctx.control_source == SBUS`（rank 3），自动流程的 `AUTO` 请求（rank 1）必然满足 `1 < 3` 而被拒绝，理由为 `LOWER_PRIORITY_CONFLICT`。此时 SBUS 并未请求发射，自动流程本应正常推进。

**错误二：SBUS 请求绕过检查。** `rank(SBUS) = 3` 为最大值，因此 `3 < rank(任何控制源)` 恒为假，SBUS 来源的发射授权**永远跳过这一检查**。

**风险总结**

| 请求源 | 控制源 | 期望 | 实际 |
|---|---|---|---|
| `AUTO` | `AUTO` | 通过 | 通过（`1 < 1` 为假） |
| `AUTO` | `SBUS` | 通过（SBUS 未请求发射） | **拒绝** |
| `AUTO` | `WEB` | 通过（WEB 未请求发射） | **拒绝** |
| `SBUS` | `NONE`（急停已在前置分支返回） | 按前置条件判定 | 跳过优先级检查 |
| `WEB` | `AUTO` | 通过 | 通过（`2 < 1` 为假） |

**修复注意事项（仅记录，不在本轮实施）**

需要引入独立的"是否存在更高优先级控制源**正在主动介入**"判据。`FireAuthorization` 已有 `priority_conflict` 字段（`Inc/StateArbitration.hpp` L172），但驱动它的条件需要替换，而不是继续沿用数值比较。判据来源可以是 `control_source` + `sbus_online` + `sbus_auto_mode` 的组合推导，也可以由调用方显式提供"介入中"状态，二者需在设计层面先定论。

---

### ATOM02-BUG-003 `RequestAutoFireState(FIRING)` 硬编码请求源为 `AUTO`，手动/调试模式下的发射动作无合法提交路径

**严重度**：高

**现象**

```cpp
// Src/StateArbitration.cpp L596-611
} else if (target == AutoFireState::FIRING) {
    // 发射前待命 -> 发射中：停止标志为 False 且授权通过（Interface §7.1、§7.2）
    const FireAuthorization auth = EvaluateFireAuthorization(ctx_, ControlSource::AUTO);
    if (!auth.allowed) {
        result.reason = (auth.reason == RejectReason::NONE) ? RejectReason::FIRE_NOT_AUTHORIZED
                                                            : auth.reason;
        ...
        return result;
    }
    accepted = IsAutoFireTransitionAllowed(ctx_.auto_fire_state, target);
}
```

请求源被写死为 `ControlSource::AUTO`。

**证据（设计要求）**

- `Design/Config.md` §11.4 WEB 手动动作集：`fire_once`「执行一次发射动作」，**允许模式为手动、调试**，「必须满足发射授权与全部前置条件」。
- `Design/PEF.md` §1.4 与 `Design/Interface.md` §9.2：实际发射动作的控制优先级严格为 `SBUS > WEB > 自动流程`，即 SBUS 与 WEB 都是合法的发射请求源。
- `Inc/StateArbitration.hpp` L266–271：`SubmitIntent` 在 `IntentKind::FIRE` 分支**只返回判定结果、不推进任何状态**。
- 同文件 L272：`RequestAutoFireState(AutoFireState target)` 签名中没有请求源参数。

**影响**

SBUS/WEB 发起的手动 `fire_once` 在状态机上**没有任何入口可以进入 `FIRING`**：

1. `SubmitIntent(FIRE)` 只给出 `accepted`，不改变 `auto_fire_state`。
2. `RequestAutoFireState(FIRING)` 又只以 `AUTO` 身份授权，因此 WEB/SBUS 的授权结论根本没被采用。
3. 更严重的是，手动触发发射还必须同时满足 `ctx_.system_mode == SystemMode::AUTO_FIRE`（L571–574）。而手动/调试模式的 `system_mode` 是 `MANUAL`/`DEBUG`，**会在 L571 就被 `NOT_AUTO_FIRE_MODE` 拒绝**。

即：手动发射在 `RequestAutoFireState` 路径上被拒绝两次，在 `SubmitIntent` 路径上不产生状态变化。`Config.md` §11.4 的 `fire_once` 与 `PEF.md` §8.1/§8.2 的发射授权要求无法同时满足。

**修复注意事项（仅记录，不在本轮实施）**

需要区分"自动流程内推进到 `FIRING`"与"手动/调试模式下的单次发射授权"，两者不应共用一个写死 `AUTO` 的方法。可选择为 `RequestAutoFireState` 增加请求源参数，或在 `StateArbitrator` 上新增独立的手动发射入口。该选择影响 `ATOM-11`（发射机构）与 `ATOM-13`（PEFExecutor）的接线方式，需先行定论。

---

### ATOM02-BUG-004 人工确认解除急停时立即清零 `software_estop`，早于设计的"解除动作完成后"时点

**严重度**：高

**现象**

```cpp
// Src/StateArbitration.cpp L505-516
const SystemMode from = ctx_.system_mode;
ctx_.system_mode = target;
result.accepted = true;

// 急停锁定为统一 bool（Config §15）；退出急停只能走 ESTOP_CONFIRM 人工确认路径
if (target == SystemMode::SOFTWARE_ESTOP) {
    ctx_.inputs.software_estop = true;
    estop_latched_ = true;
} else if (from == SystemMode::SOFTWARE_ESTOP && reason == TransitionReason::ESTOP_CONFIRM) {
    ctx_.inputs.software_estop = false;   // L515：确认即清零
    estop_latched_ = false;               // L516
}
```

**证据（设计要求）**

- `Design/Config.md` §15：「软件急停变量：统一 `bool` 变量；`True` 表示锁定急停，**人工确认并完成解除动作后**设为 `False`」。
- `Design/Config.md` §14.1：「软件急停人工解除 | 换弹抬起、**蓄力机构两个电机回零**；不重新初始化；不自动恢复原自动流程」。
- `Design/PEF.md` §8.3 第 4 条：「人工确认解除后，换弹机构执行抬起动作，蓄力机构两个电机执行回零动作；蓄力机构**仅在两个限位开关均为低电平时**判定回零完成」。
- `Design/Atomic/ATOM-14_Safety_Recovery.md` 第 7 条：「**两个蓄力限位均低且换弹抬起步骤完成后**，急停变量方可置为 `False`」。
- `Design/Interface.md` §17.3 第 7 条：「安全管理模块发出的零力矩报文、换弹抬起和人工确认后的蓄力机构两个电机回零属于**安全动作**，不受普通命令拒绝规则影响」——即这些安全动作必须在急停变量为 `True` 期间执行。

**影响**

`ConfirmSoftwareEstop()`（L551–554）一旦被接受，急停变量立即变为 `false`，产生三项后果：

1. 零力矩帧的持续发送依据消失（`Config.md` §6.3 要求"持续发送直到解除急停状态"，而解除动作尚未执行）。
2. `EvaluateIntent`（L221–225）不再拒绝普通控制意图，`SelectControlSource`（L133–135）也恢复正常排序，**普通执行器命令在恢复动作完成前即被放行**。
3. 与 `Config.md` §11.4 `servo_set`「软件急停时发射机构舵机一律拒绝」的约束脱钩。

即：解除动作（换弹抬起 + 蓄力双电机回零 + 双限位低电平判定）**尚未开始执行**时，系统已认为急停解除完毕。

**修复注意事项（仅记录，不在本轮实施）**

需要把"人工确认"与"解除生效"拆成两个可区分的事件，或让急停变量由 `ATOM-14` 统一持有、由其在恢复动作全部完成后写入 `false`。本模块当前同时"持有并解释"该变量（`Inc/StateArbitration.hpp` L46 将其放在 `ExternalInputs` 中，却在内部决定清零时机），归属需要先定论。

---

### ATOM02-BUG-005 `UpdateInputs` 整体替换输入快照，与头注释声明的"多方分别写入"契约矛盾，导致模块间输入互相覆盖

**严重度**：中

**现象**

```cpp
// Src/StateArbitration.cpp L437-446
ControlSourceChange StateArbitrator::UpdateInputs(const ExternalInputs& inputs) noexcept
{
    // Config §15：急停为锁定变量，置位后只有人工确认（ESTOP_CONFIRM）能解除
    if (inputs.software_estop) {
        estop_latched_ = true;
    }
    ctx_.inputs = inputs;   // L443：整体替换，未写入的字段全部回到结构体默认值
    if (estop_latched_) {
        ctx_.inputs.software_estop = true;
    }
    ...
}
```

而头注释声明的是**多方分别写入**：

```cpp
// Inc/StateArbitration.hpp L36-41
// 写入方（用户确认）：SBUS 解析器写 web/sbus 无关的 SBUS 字段；WEB、视觉、
// 故障模块均可写入各自字段；软件急停为统一 bool（Config §15）。
//   停止发射标志：WEB 前端置 True；视觉正常 YAW 到位置 False（Interface §6.4）
//   YAW 到位：视觉置 True；主控超时回退置 True；发射完成置 False
```

**影响**

`ExternalInputs` 有 10 个字段（L42–53），分属 `ATOM-03`（`sbus_online`、`sbus_auto_mode`）、`ATOM-16/17`（`web_online`）、`ATOM-12`（`yaw_arrived`、`stop_fire`）、`ATOM-14`（`software_estop`、`fault_active`、`fault_severity`）、时钟模块（`timestamp_us`、`sequence`）。

接口上没有任何字段级 setter，唯一的写入入口是整体替换。因此：

- 若 SBUS 模块只填自己的两个字段提交，视觉的 `yaw_arrived` 会被清零、`stop_fire` 会被重置为默认值 `false`——**而 `Config.md` §2 规定停止发射标志初值必须为 `True`**，一次误替换即构成"非法清除了安全标志"。
- 若各模块都通过 Read-Modify-Write 自行拼接完整结构，则必须共享同一份最新快照，形成跨模块共享可变状态，与 `Config.md` §15「状态机与机构控制器同步采用数据零拷贝」的意向不符，且存在丢失更新的竞态。

**备注**：`Config.md` §2 中停止发射标志的初值为 `True`，而 `ExternalInputs::stop_fire` 的默认值是 `false`（L47）；`Reset()`（L366–379）把 `ctx_ = ArbitrationContext{}`，因此**复位后 `stop_fire` 为 `false`**，与配置表规定的初值 `True` 相反。这一默认值偏差与整体替换缺陷叠加后，会直接导致"复位即解除停止发射"。

**修复注意事项（仅记录，不在本轮实施）**

需要按字段合并更新（或提供字段级 setter），并修正 `stop_fire` 的初值以对齐 `Config.md` §2。

---

### ATOM02-BUG-006 `fault_severity` 被存储但从未参与任何决策，故障等级形同虚设

**严重度**：中

**现象**

`Inc/StateArbitration.hpp` L49 定义：

```cpp
FaultSeverity fault_severity{FaultSeverity::INFO};
```

`Src/StateArbitration.cpp` 中对 `fault_active` 的使用见 L159、L174–177、L246–249、L115–118，全部为布尔判断。以 `grep` 全文件核对，`fault_severity` **仅有定义与赋值（整体拷贝），没有任何读取点**。

**证据（设计要求）**

- `Design/Config.md` §14.2 定义四档等级「信息、警告、严重、急停」，且明确区分行为，例如：`W-CMD-001`（警告）「拒绝该命令，不改变当前有效目标并上报调用方」；`W-VISION-002`（信息）「正常回退路径，不触发一般故障恢复」；`F-CAN-001`（严重）「执行 PEF 一般故障恢复流程」。
- `Design/Interface.md` §18.2：故障管理须提供「故障严重级别」并用于「自动恢复条件」。

**影响**

当前所有故障等级在决策上一视同仁：任何 `fault_active == true` 都会阻塞普通控制意图（L246–249）并否决发射授权（L174–177）。这会把本应"仅拒绝该命令"的警告级故障（`W-CMD-001`）、本应"不触发一般故障恢复"的信息级事件（`W-VISION-002`）升级为全局阻塞。

更关键的是保留一个**从不生效的字段**会让后续调用方误以为"填了等级就会按等级处理"，从而在配置与实现之间形成静默偏差。

**修复注意事项（仅记录，不在本轮实施）**

要么在授权/仲裁/状态转换判定中实际使用 `fault_severity`，要么将其从本模块的输入契约中移除、改为由 `ATOM-14` 在写入 `fault_active` 前完成等级到布尔的归约，避免出现"看似生效实则无效"的字段。两种取向需先行定论。

---

### ATOM02-BUG-007 `IsOperationalMode` 不含 `RECOVERY`，一般故障恢复期间的安全动作会被状态机拒绝

**严重度**：中

**现象**

```cpp
// Src/StateArbitration.cpp L76-82
// Interface §7.2 第 1 条：未完成初始化不得进入自动发射或手动动作状态
bool IsOperationalMode(SystemMode mode) noexcept
{
    return mode == SystemMode::STANDBY || mode == SystemMode::MANUAL ||
           mode == SystemMode::AUTO_FIRE || mode == SystemMode::DEBUG ||
           mode == SystemMode::CALIBRATION;
}
```

`RECOVERY` 不在集合内。而 `EvaluateIntent` L251–254 对非可操作模式一律返回 `NOT_READY`：

```cpp
if (!IsOperationalMode(ctx.system_mode)) {
    decision.reason = RejectReason::NOT_READY;
    return decision;
}
```

**证据（设计要求）**

- `Design/PEF.md` §7 一般故障自动恢复流程第 1–7 条：`RECOVERY` 期间必须执行「蓄力机构两个电机执行回零」「仅发射机构执行三次发射动作」「换弹机构执行抬起动作组」。
- `Design/Atomic/ATOM-14_Safety_Recovery.md` 一般故障恢复第 1–6 条：`RECOVERY` 期间需分别请求蓄力电机回零、调用单次 FireAction 三次、请求换弹抬起。
- `Design/Interface.md` §7.1：「故障 | 自动恢复触发 | 自动恢复 | 执行回零、三次发射、换弹抬起」。

**影响**

若 `ATOM-14` 的恢复流程按常规路径通过状态机提交控制意图，所有恢复动作都会被 `NOT_READY` 拒绝，**一般故障自动恢复无法执行**。

若 `ATOM-14` 改为绕过状态机直连机构控制器，则违反 `Design/Interface.md` §5.2「机构控制器不得绕过 `ControlArbiter`、`StateMachine` 和 `SafetyManager` 直接接受外部控制请求」。

两条路都必须先明确"恢复期安全动作是否豁免状态机闸门"，否则必然二选一地违反某条设计约束。同时，软件急停期间的"零力矩发送 / 换弹抬起 / 人工确认后的蓄力回零"（`Interface.md` §17.3 第 7 条明确列为**安全动作例外**）在 `SOFTWARE_ESTOP` 模式下同样会被 `IsOperationalMode` 拦截，属于同一根因。

**修复注意事项（仅记录，不在本轮实施）**

需要引入"安全动作"这一意图类别，并明确其在 `RECOVERY`、`SOFTWARE_ESTOP` 下的豁免规则，与 `Interface.md` §17.3 第 7 条的例外清单逐条对齐。

---

### ATOM02-BUG-008 `run_id_` 存在两条递增路径，公开接口重复

**严重度**：中

**现象**

路径一：

```cpp
// Src/StateArbitration.cpp L590-595
} else if (target == AutoFireState::AUTO_PREPARE) {
    accepted = IsAutoFireTransitionAllowed(ctx_.auto_fire_state, target);
    reason = TransitionReason::AUTO_FLOW_RUN;
    if (accepted && ctx_.auto_fire_state == AutoFireState::IDLE) {
        ++run_id_; // 从 IDLE 启动视为新一次流程（BeginNewAutoFireRun 的另一入口）
    }
}
```

路径二：

```cpp
// Src/StateArbitration.cpp L675-705
AutoFireTransitionResult StateArbitrator::BeginNewAutoFireRun() noexcept
{
    ...
    if (is_auto_fire_active()) {
        result.reason = RejectReason::AUTO_FLOW_ACTIVE;
        return result;
    }
    // 唯一允许离开 AUTO_COMPLETED / AUTO_TERMINATED 的入口：全新一次流程（新 run_id）
    const AutoFireState from = ctx_.auto_fire_state;
    ++run_id_;
    ctx_.auto_fire_state = AutoFireState::AUTO_PREPARE;
    ...
}
```

**影响**

1. 代码注释已自承两者是"另一入口"，但 `BeginNewAutoFireRun` 的注释又声明自己是"**唯一**允许离开 `AUTO_COMPLETED` / `AUTO_TERMINATED` 的入口"——**注释与实现相互矛盾**。
2. 两条路径的前置条件不同：路径一要求 `system_mode == AUTO_FIRE`（L571）且当前恰为 `IDLE`；路径二要求 `system_mode == AUTO_FIRE` 且**不**处于活动流程（包含 `IDLE`、`AUTO_COMPLETED`、`AUTO_TERMINATED`）。当 `PEFExecutor` 在 `AUTO_COMPLETED` 后再用路径一开启新一轮时，`IsAutoFireTransitionAllowed(AUTO_COMPLETED, AUTO_PREPARE)`（L94–102）为假，会返回 `UNKNOWN_TRANSITION`，**接口在对外表现上不可预测**。
3. 两条路径都会推进 `run_id_`，若调用方同时使用，`run_id` 的语义（"一次自动流程的唯一标识"）会被稀释。

**修复注意事项（仅记录，不在本轮实施）**

收敛为单一入口，删除另一条自增路径；或明确二者职责边界并在文档与注释中同步。

---

### ATOM02-BUG-009 `IntentKind::STOP_FIRE_SET` 分支缺少模式检查

**严重度**：低

**现象**

```cpp
// Src/StateArbitration.cpp L215-219
// 置停止发射标志是安全单调动作：只收紧约束，不受优先级与急停阻断
if (intent.kind == IntentKind::STOP_FIRE_SET) {
    decision.accepted = true;
    return decision;
}
```

该分支位于 L222（急停检查）**之前**，且不检查 `ctx.system_mode`。

**证据**

`Design/Config.md` §11.4：`stop_fire_set`「置停止发射标志」的**允许模式**为「手动、调试」，安全限制「置位后不执行实际发射、不开始下一发」。

**影响**

"只收紧约束、不受优先级阻断"这一设计判断本身是合理且值得保留的（安全单调性优先），但**完全豁免模式检查**超出了配置表的规定：在 `UNINITIALIZED`、`INIT_FAILED`、`CALIBRATION` 等模式下也可置位。实际危害有限（置位只会更保守），但与 `Config.md` §11.4 的字面规定不符，且会使"允许模式"这一列对该动作失去约束力。

**修复注意事项（仅记录，不在本轮实施）**

在保留"不受优先级与急停阻断"这一安全单调性的前提下，补上模式合法性校验。

---

### ATOM02-BUG-010 `RequestAutoFireState` 在自动流程未起步时仍允许进入 `AUTO_TERMINATED`

**严重度**：低

**现象**

`Src/StateArbitration.cpp` L562–570 的前置检查为：

```cpp
if (target == ctx_.auto_fire_state) { ... ALREADY_IN_TARGET }
if (IsAutoFireStateTerminal(ctx_.auto_fire_state)) { ... AUTO_FLOW_TERMINATED }
if (ctx_.system_mode != SystemMode::AUTO_FIRE) { ... NOT_AUTO_FIRE_MODE }
```

随后 L579–586：

```cpp
if (target == AutoFireState::AUTO_TERMINATED) {
    accepted = is_auto_fire_active();
    ...
}
```

`is_auto_fire_active()`（L397–401）要求 `auto_fire_state != IDLE`。因此 `IDLE → AUTO_TERMINATED` 会被拒绝。

**影响**

`IDLE` 状态下若请求终止，返回 `NOT_READY`（L584）。这在语义上可以接受，但产生一处**状态表达不一致**：`AUTO_TERMINATED` 与 `AUTO_COMPLETED` 同被 `IsAutoFireStateTerminal`（L89–92）视为终态，而 `IDLE` 是"尚未开始的初始态"。当 `ATOM-13` 收到 `auto_stop`（`Config.md` §11.4 规定其允许模式为「任意普通模式」）而流程实际尚未启动时，`AUTO_TERMINATED` 与 `IDLE` 的选择将影响后续 `BeginNewAutoFireRun` 的可用性判断，属于需要显式约定的边界。

此外，`TerminateAutoFireOnSbusTakeover`（L645–648）对 `IDLE` 明确返回 `NOT_READY`，与 `RequestAutoFireState` 的行为一致，说明这可能是**有意设计**而非疏漏，故严重度低，仅登记以待确认。

**修复注意事项（仅记录，不在本轮实施）**

明确"未启动流程收到 auto_stop"的期望状态与原因码，并在 `Config.md` §11.4 或 `Interface.md` §8.3 中固化。

---

### ATOM02-BUG-011 `RequestModeTransition` 为 `InitFailed → MANUAL` 留出通路

**严重度**：低

**现象**

`Src/StateArbitration.cpp` L36：

```cpp
{SystemMode::SOFTWARE_ESTOP, SystemMode::MANUAL, TransitionReason::ESTOP_CONFIRM},
```

`kModeEdges` 中并无 `INIT_FAILED` 的出边。但 L107–125 的通配边逻辑为：

```cpp
if (reason == TransitionReason::SOFTWARE_ESTOP_TRIGGER) {
    return to == SystemMode::SOFTWARE_ESTOP && from != SystemMode::SOFTWARE_ESTOP;
}
if (reason == TransitionReason::FAULT_DETECTED) {
    return to == SystemMode::FAULT && from != SystemMode::SOFTWARE_ESTOP &&
           from != SystemMode::FAULT && from != SystemMode::UNINITIALIZED &&
           from != SystemMode::INITIALIZING;
}
```

`FAULT_DETECTED` 分支排除了 `UNINITIALIZED` 与 `INITIALIZING`，**但未排除 `INIT_FAILED`**。经与 `Design/Interface.md` §7.1 状态转换总表逐行对照，该表同样未给出 `初始化失败` 的任何出边。

**影响**

一个初始化失败的设备仍可被推进到 `FAULT`，进而经 `FAULT → RECOVERY → MANUAL`（L37–38）进入可操作状态。这与 `Design/Atomic/ATOM-01_Runtime.md` 验收条件第 3 条「任一必需项失败时进入 `INIT_FAILED`，**且发射请求被拒绝**」以及 `Design/Config.md` §3.1「初始化失败 | 初始化失败，禁止执行器动作」相冲突。

**修复注意事项（仅记录，不在本轮实施）**

在 `FAULT_DETECTED` 通配边中一并排除 `INIT_FAILED`，或明确允许并经文档修订确认这一恢复通路。**本项涉及设计意图裁决，不宜由实现单方面决定。**

---

### ATOM02-BUG-012 事件缓冲仅 64 条，`Config.md` §12.1 的 20 ms 推送周期下覆盖速度快于消费速度

**严重度**：低

**现象**

```cpp
// Inc/StateArbitration.hpp L234
static constexpr std::size_t kMaxEvents = 64;
```

`PushEvent`（L381–395）在写满后覆盖最旧事件。

**证据**

- `Design/Config.md` §12.1：状态推送周期 **20 ms**；§4：WebSocket 推送周期 20 ms。
- `Design/Config.md` §15：控制权变化时"停止后续低优先级动作，并执行优先级更高的控制源命令"——控制源重算在每次 `UpdateInputs`、每次模式转换、每次自动状态提交时都会触发事件。
- `Design/Config.md` §16.1：历史故障保留数量为 **1000 条**，日志单文件 10 MiB。

**影响**

64 条容量在 20 ms 周期下仅对应约 1.28 秒的事件密度。若消费方（`ATOM-18` 诊断或 WebSocket 推送）慢于生产速率，事件会静默丢失，且**没有任何丢弃计数或丢弃标记**可供诊断——`POM` 层面无法区分"没有事件"与"事件被覆盖"。

**修复注意事项（仅记录，不在本轮实施）**

评估容量与消费速率匹配后调整，或在覆盖时累计丢弃计数并通过 `CopyEvents` 暴露，避免静默丢失。注意：本项与"零堆分配"的设计取向存在权衡，不应简单改为动态容器。

---

### ATOM02-BUG-013 仲裁输入/意图/结果类型未落入 `Atoms/Public`，导致 6 个模块横向依赖本模块私有头

**严重度**：中

**现象**

以下类型目前全部定义在 `Inc/StateArbitration.hpp` 内，但它们的分属方并非本模块：

| 类型 | 行号 | 生产方 / 消费方 |
|---|---|---|
| `ExternalInputs` | L42–53 | 写入：`ATOM-03`(SBUS)、`ATOM-12`(视觉)、`ATOM-14`(急停/故障)、`ATOM-16/17`(WEB) |
| `ArbitrationContext` | L56–61 | 消费：`ATOM-13`(PEFExecutor)、`ATOM-18`(诊断) |
| `ControlIntent` / `IntentKind` | L136–151 | 生产：`ATOM-03`、`ATOM-16`；消费：本模块 |
| `IntentDecision` | L153–159 | 消费：`ATOM-03`、`ATOM-16`、`ATOM-13` |
| `FireAuthorization` | L162–173 | 消费：`ATOM-11`、`ATOM-13` |
| `ModeTransitionResult` / `AutoFireTransitionResult` | L112–126 | 消费：`ATOM-13`、`ATOM-16` |
| `ControlSourceChange` | L128–133 | 消费：`ATOM-18`、`ATOM-17` |
| `StateEvent` / `EventKind` / `TransitionReason` / `RejectReason` | L66–107、L179–202 | 消费：`ATOM-18`(日志)、`ATOM-17`(WebSocket) |

**影响**

按 `ATOM-02_PLAN.md` §3.1 已确立的纪律：「**禁止各 ATOM 重复定义同名枚举/类型**（同一编译单元会重定义报错）」，各模块只能 `#include "StateArbitration.hpp"`。这形成"6 个模块横向依赖 ATOM-02 私有头"的耦合，与 `ATOM-01` 的 `ILifecycleModule` 问题（见 `ATOM-01_Runtime/BugLists.md` ATOM01-BUG-007）同源。

**修复注意事项（仅记录，不在本轮实施）**

把上述输入/意图/结果/事件类型提升至 `Atoms/Public/`，仅保留 `StateArbitrator` 类本体在本模块 `Inc/`。

---

### ATOM02-BUG-014 类内定义且带未使用形参的成员函数在 `-Wextra` 下可能触发 `-Wunused-parameter`

**严重度**：低

**现象**

以下成员函数在类内定义（隐式 `inline`），形参未在函数体中使用：

```cpp
// Inc/StateArbitration.hpp L248-256
const ArbitrationContext& context() const noexcept { return ctx_; }
SystemMode system_mode() const noexcept { return ctx_.system_mode; }
AutoFireState auto_fire_state() const noexcept { return ctx_.auto_fire_state; }
ControlSource control_source() const noexcept { return ctx_.control_source; }
```

```cpp
// Inc/StateArbitration.hpp L284-290
std::size_t event_count() const noexcept { return event_count_; }
void ClearEvents() noexcept;
std::size_t CopyEvents(StateEvent* out, std::size_t capacity) const noexcept;
const StateEvent* latest_event() const noexcept;
```

以上无参函数本身不涉及未使用形参；风险集中在 `Inc/StateArbitration.hpp` L232–256、L284–290 区间内**带形参但未使用**的成员（例如 `Reset(std::uint64_t timestamp_us)` 已使用，不属此列）。

**说明**：本项为**低置信度提示**，未在静态阅读中定位到确定触发点。上层 `NewVersion/CMakeLists.txt` L46–47 启用了 `-Wall -Wextra -Wpedantic`，而 `ATOM-02_PLAN.md` §6.3 要求「警告视为需清零」，因此登记此项以便编译时逐条核对，避免遗漏。

**状态（2026-09-27）：经编译验证判定为不成立，本条关闭。**

在 WSL aarch64 交叉编译、`-Wall -Wextra -Wpedantic` 下增量构建 `atom_02_state_arbitration`，**零警告**。原登记为低置信度提示，现确认无需处理，保留记录以说明该提示已排查完毕。

---

## 2.1 高优先级缺陷的源头修复方案

本节针对 `ATOM02-BUG-001`～`004` 给出可直接执行的修复方案。

**为什么必须在本模块修复而不能由上层补偿**：`Atoms/Public/AppCoordinator/`（本轮新增的桥接层）已刻意保持"薄"，只做启动结果映射、输入合并与运行闸门。若把上列缺陷的修正逻辑放进桥接层，会产生与状态机并存的"第二真相"，且 `kAutoFireEdges` / `kModeEdges` / `EvaluateFireAuthorization` 均在本模块 `.cpp` 的匿名命名空间内，**从外部不可达、无法覆盖**。

### 2.1.1 `ATOM02-BUG-001` 的修复方案（发次消歧）

依赖背景：`SHOT_N_YAW` 目前无入边，第 2～4 发在 `FORCE` 后无合法后继。但**不能简单补一行 `{FORCE, SHOT_N_YAW}`**，否则 `FORCE` 同时拥有两个后继且 `IsAutoFireTransitionAllowed(from, to)` 无法消歧。

建议引入"发次区间"到自动流程转换：

```cpp
// StateArbitration.hpp：新增阶段枚举
enum class ShotStage : std::uint8_t { FIRST, SUBSEQUENT };

// 新增纯函数（保留既有签名，供不需要发次信息的调用方继续使用）
bool IsAutoFireTransitionAllowed(AutoFireState from, AutoFireState to) noexcept;
bool IsAutoFireTransitionAllowed(AutoFireState from, AutoFireState to,
                                 ShotStage stage) noexcept;
```

```cpp
// StateArbitration.cpp：为 kAutoFireEdges 增加 stage 字段
struct AutoFireEdge {
    AutoFireState from;
    AutoFireState to;
    ShotStage stage;   // 通配边用 ANY 语义：两条边都登记
};

constexpr AutoFireEdge kAutoFireEdges[] = {
    ...
    {AutoFireState::FORCE, AutoFireState::SHOT_1_YAW,   ShotStage::FIRST},
    {AutoFireState::FORCE, AutoFireState::SHOT_N_YAW,   ShotStage::SUBSEQUENT}, // 新增
    ...
};
```

同时 `RequestAutoFireState()` 需增加发次入参（或由 `StateArbitrator` 持有 `current_shot` 并据此推导 `ShotStage`）。`Interface.md` §6.6 的 `AutoFireContext.current_shot` 已存在该信息，建议直接落到状态机内，避免调用方各自维护。

**验收补充**：`FORCE -> SHOT_1_YAW` 与 `FORCE -> SHOT_N_YAW` 在不同 `ShotStage` 下各自唯一可达。

### 2.1.2 `ATOM02-BUG-002` 的修复方案（授权优先级不改写控制源）

核心：**"实际发射授权优先级"不应通过改写 `ctx.control_source` 或与 rank 比较来实现。**

当前错误代码位于 L187–192。建议改为"同源"判据：

```cpp
// 请求源必须是当前实际生效的控制源；请求源自身必须有效。
// 控制源为 AUTO 时（SBUS 自动模式标志为 True，或 SBUS 失联且无 WEB），
// SBUS/WEB 仍可依 Interface §9.2 的仲裁路径介入，但其介入须经
// EvaluateIntent/SubmitIntent，不由本函数放行。
if (requesting_source != ctx.control_source) {
    auth.priority_conflict = true;
    auth.reason = RejectReason::LOWER_PRIORITY_CONFLICT;
    return auth;
}
```

或引入显式的"更高优先级是否正在介入"判据：

```cpp
bool IsHigherPriorityIntervening(const ArbitrationContext& ctx,
                                 ControlSource requesting_source) noexcept
{
    if (ctx.inputs.software_estop)      return true;  // 已在前置分支处理
    if (ctx.inputs.sbus_online && !ctx.inputs.sbus_auto_mode &&
        ControlSourceRank(requesting_source) < ControlSourceRank(ControlSource::SBUS)) {
        return true;
    }
    if (ctx.inputs.web_online &&
        ControlSourceRank(requesting_source) < ControlSourceRank(ControlSource::WEB)) {
        return true;
    }
    return false;
}
```

后一种更贴合 `Config.md` §7.2 的五条控制源选择规则，建议采用。

**必须同时验证的判定表**（当前实现的行为见 §2 的 `ATOM02-BUG-002` 表格）：

| 请求源 | SBUS | 自动模式标志 | WEB | 期望授权 |
|---|---|---|---|---|
| `AUTO` | 在线 | `True` | 任意 | 通过（控制源为 `AUTO`） |
| `AUTO` | 在线 | `False` | 任意 | 通过（SBUS 未主动请求发射） |
| `AUTO` | 失联 | — | 在线 | 通过（WEB 未主动请求发射） |
| `SBUS` | 在线 | `False` | — | 通过 |
| `SBUS` | 在线 | `True` | — | 需经 `SubmitIntent` 仲裁，不由本函数直接放行 |

### 2.1.3 `ATOM02-BUG-003` 的修复方案（发射请求源参数化）

`RequestAutoFireState(FIRING)` 硬编码 `ControlSource::AUTO`（L596–611），叠加 L571 的 `system_mode != AUTO_FIRE` 前置检查，使 `Config.md` §11.4 的 `fire_once`（允许模式：手动、调试）无合法提交路径。

建议新增独立的单次发射入口，与自动流程内的 `FIRE_READY → FIRING` 分离：

```cpp
// 自动流程内部推进：沿用现有语义
AutoFireTransitionResult RequestAutoFireState(AutoFireState target) noexcept;

// 手动/调试模式下的单次发射授权（Config §11.4 fire_once）
// 前置：system_mode ∈ {MANUAL, DEBUG}；不与自动流程状态机耦合。
FireAuthorization AuthorizeManualFire(ControlSource requesting_source) noexcept;
```

调用方（`ATOM-11` 发射机构 / `ATOM-16` HTTP `fire_once`）据此先取授权、再执行动作。这样既不改动自动流程的既有推进语义，又为手动发射提供唯一合法入口。

若选择参数化 `RequestAutoFireState(AutoFireState target, ControlSource requesting_source)`，则必须在 `FIRING` 分支中使用该参数替代硬编码 `AUTO`，并把 L571 的模式前置检查改为"自动流程推进要求 `AUTO_FIRE`；手动单发要求 `MANUAL`/`DEBUG`"的双分支。

### 2.1.4 `ATOM02-BUG-004` 的修复方案（急停解除与锁定分离）

当前 `RequestModeTransition` L513–516 在 `ESTOP_CONFIRM` 被接受的瞬间即清零 `ctx_.inputs.software_estop` 与 `estop_latched_`，早于 `Config.md` §15 / `PEF.md` §8.3 / `ATOM-14` 第 7 条要求的"解除动作完成"时点。

建议把"人工确认"与"解除生效"拆成两步：

```cpp
// 第一步：人工确认解除急停。只切换到 MANUAL，**保持急停锁定为 True**，
// 以便 ATOM-14 在锁定保护下执行换弹抬起与蓄力双电机回零。
ModeTransitionResult ConfirmSoftwareEstop() noexcept;

// 第二步：由 ATOM-14 在确认下列条件全部满足后调用：
//   - 换弹机构抬起动作组已完成
//   - 蓄力电机 1、2 回零完成，即两个限位开关均为低电平
// 只有此时才允许清零急停变量与锁存。
void ReleaseSoftwareEstopLatch() noexcept;
```

实施要点：

1. 从 `RequestModeTransition` 的 `ESTOP_CONFIRM` 分支中**删除** L515–516 的清零动作。
2. `ConfirmSoftwareEstop()` 改为只做模式迁移（`SOFTWARE_ESTOP → MANUAL`），保留 `estop_latched_ = true`。
3. 新增 `ReleaseSoftwareEstopLatch()`，并在其中校验调用前置条件（可返回 bool 表示是否允许释放）。
4. 相应调整 `SelectControlSource()`：锁定期间是否返回 `NONE` 需与 `ATOM-14` 定论，避免"已切到 MANUAL 但控制源仍为 NONE"造成手动模式不可用。

**回归验收**：人工确认后、解除动作完成前，`EvaluateIntent()` 仍应拒绝普通控制意图，且零力矩帧仍在持续发送。

---

## 3. 与 `PLAN.md` 已声明未完成项的对应关系

| 本清单条目 | `PLAN.md` 状态 | 是否属于已知未完成 |
|---|---|---|
| ATOM02-BUG-001～012、014 | §6.1 声明"静态自查"已完成、§7 声明"已落地" | **否，均为已实现代码中的缺陷** |
| ATOM02-BUG-013（Public 共享类型） | §8 待确认项 1 已提及共享头文件名为 `MarshlandTypes.hpp`，但未覆盖仲裁输入/结果类型 | 部分是新发现 |

`PLAN.md` §8 待确认项 2 提到「软件急停、停止发射标志、故障状态的**写入方**与锁的类型需在上层接线时敲定」，与 ATOM02-BUG-004（急停解除时点）和 ATOM02-BUG-005（整体替换覆盖）**高度相关但未覆盖其后果**，建议在接线定论后一并复核本清单。

---

---

> **【已裁决】本条不新增 `RESTART` 边，`kModeEdges` 保持现状即为正确行为。**
>
> **裁决（用户，2026-09-27）：不支持运行中重启。** `SystemMode` 一旦离开
> `UNINITIALIZED` 即不可回到可初始化状态；运行中需要重新初始化时必须重启进程。
>
> | 层 | 处置 | 状态 |
> |---|---|---|
> | `Atoms/Public/AppCoordinator` | `Start()` 在 `SystemMode != UNINITIALIZED` 时返回 `kRestartNotSupported` 并报告当前模式，**永久拒绝**第二次初始化；不调用 `Reset()`，保留诊断事件 | 已实施 |
> | `Atoms/ATOM-01_Runtime` | 建议同步收紧 `start()` 的 `Stopped -> Created` 重启能力（见 `Atoms/ATOM-01_Runtime/BugLists.md` §5.8） | **待 ATOM-01 处置** |
> | `Atoms/ATOM-02_State_Arbitration` | **无需改动**。裁决与现有边表一致 | 已关闭 |
>
> **仍有效的独立待办**：在 `Design/Config.md` 或 `Design/Interface.md` §7 中显式写明
> "运行中重启不支持，重新初始化需重启进程"（文档动作，不属本模块代码）。
>
> 以下原始登记内容中的"需要裁决的问题"与"临时处置"**已被本裁决取代**，
> 仅作追溯用途，**不得作为待办依据**。

### ATOM02-BUG-015 缺少重启语义：`Shutdown()` 后无法重新初始化，`START` 边只从 `UNINITIALIZED` 出发（原始记录，2026-09-27 首次登记）

**严重度**：中（已裁决关闭）

*（裁决结果与处置见本节开头的【已裁决】块，此处不再重复。）*

**登记日期**：2026-09-27（第二轮集成审查）

**现象**

`ATOM-01` 已按 `ATOM01-BUG-003` 实现"允许重启"：`RuntimeCoordinator::start()` 在
`RuntimeState::Stopped` 状态下回到 `Created` 并重新执行启动序列。但状态机侧没有对应的重启路径。

`kModeEdges`（`Src/StateArbitration.cpp` L34–56）中与初始化相关的边只有三条：

```cpp
{UNINITIALIZED, INITIALIZING, START},
{INITIALIZING,  STANDBY,      INIT_SUCCESS},
{INITIALIZING,  INIT_FAILED,  INIT_FAILURE},
```

`START` 边**只从 `UNINITIALIZED` 出发**。而一次正常的 `AppCoordinator::Shutdown()` 只停止硬件，
不改变业务模式，`SystemMode` 会停留在 `STANDBY` 或 `MANUAL`。此时再次启动：

```
RequestModeTransition(INITIALIZING, START)
  from = STANDBY 或 MANUAL
  查表无 {STANDBY, INITIALIZING, START} / {MANUAL, INITIALIZING, START}
  → IsModeTransitionAllowed == false
  → UNKNOWN_TRANSITION
```

**即：硬件层可重启，业务层不可重启，两侧的"重启"语义已经分叉。**

**影响**

1. 设备无法在不重启进程的前提下重新初始化（例如运行中修复了配置或设备后需要重建）。
2. 调用方若依赖 `RuntimeCoordinator::start()` 的成功来推定"已重新初始化"，会得到错误结论：
   硬件起来了、业务模式却没有回到可初始化状态。
3. `AppCoordinator` 已在本层之上以 `kUninitializedRequired` 显式拒绝并给出文案
   （不伪造 `InitFailed`），但这只是**把不可用变成可诊断**，并未提供重启能力。

**需要裁决的问题（本项不宜由实现单方面决定）**

1. **是否支持运行中重启？**
   - 若支持：需要定义 `INIT_FAILED / STANDBY / MANUAL / FAULT / RECOVERY → INITIALIZING`
     的边与对应 `TransitionReason`（建议新增 `RESTART`），并明确"重启前必须先 `shutdown()`"。
   - 若不支持：应在 `RuntimeCoordinator` 侧同步收紧（不要让硬件层单独支持重启），
     或在 `Config.md` / `Interface.md` §7 中显式写明"重启需重启进程"，避免两侧语义继续分叉。
2. **重启时诊断事件如何处理？**
   这与 `Reset()` 直接冲突：`Reset()`（`Inc/StateArbitration.hpp` L93）会清空 `events_`
   与 `dropped_event_count_`，而 `Design/Interface.md` §7.2 第 6 条要求"状态迁移必须产生
   **可查询**的状态事件和原因"。若重启路径通过 `Reset()` 实现，故障发生→重启之间的
   事件与丢弃计数会消失，事后无法定责。
   建议：新增**只复位状态、不清空事件**的重启入口（或让 `Reset()` 保留事件并提供独立的
   `ClearEvents()`），并在 `Interface.md` 中固化重启前后的事件语义。

**当前临时处置（已实施于 `Atoms/Public/AppCoordinator`）**

`AppCoordinator::Start()` 在调用 `RequestModeTransition` 之前先检查
`authority_.system_mode() != SystemMode::UNINITIALIZED`，命中即返回
`kUninitializedRequired` 并在 `detail` 中给出当前模式与原因。
这是**桥接层的规避**，不是修复；本条仍需在本模块源头裁决。

**越界说明**：重启语义属于系统状态机职责，桥接层不得自行拼凑模式迁移序列
（那会形成第二驱动源），故本条只登记、不由公共层实施。

---

### ATOM02-BUG-016 `IsOperationalMode` 位于匿名命名空间，导致上层不得不复制同一语义

**严重度**：低

**登记日期**：2026-09-27（第二轮集成审查）

**现象**

`IsOperationalMode(SystemMode)` 定义在 `Src/StateArbitration.cpp` 的**匿名命名空间**内
（L89–94），外部不可链接：

```cpp
namespace {
bool IsOperationalMode(SystemMode mode) noexcept
{
    return mode == SystemMode::STANDBY || mode == SystemMode::MANUAL ||
           mode == SystemMode::AUTO_FIRE || mode == SystemMode::DEBUG ||
           mode == SystemMode::CALIBRATION;
}
} // namespace
```

而 `Atoms/Public/AppCoordinator::RunGate()` 需要同一判据来决定"业务模式是否可操作"，
只能在自己的 `.cpp` 匿名命名空间内**复制一份等价实现**
（`Atoms/Public/AppCoordinator/Src/AppCoordinator.cpp`）。

**影响**

同一个模式集合现在有两份定义。若 ATOM-02 后续调整该集合（例如
`ATOM02-BUG-007` 相关裁决把 `RECOVERY` 纳入普通闸门、或新增模式），
`AppCoordinator` 的副本**不会自动跟随**，会出现"状态机认为可操作、桥接层认为不可操作"
（或反之）的静默分歧——而这是一个安全闸门，分歧后果是执行器被错误放行或错误封锁。

**建议（需本模块裁决）**

把该判据作为纯函数导出，与 `IsFaultBlocking` 一致：

```cpp
// StateArbitration.hpp（公开纯函数区）
bool IsOperationalMode(SystemMode mode) noexcept;
```

理由：`IsFaultBlocking(const ExternalInputs&)` 已经采用了"导出纯函数供上层复用"的做法
（`Inc/StateArbitration.hpp` L63），`IsOperationalMode` 属于同一类判据，没有理由留在匿名命名空间。
导出后 `AppCoordinator` 直接调用，消除双份语义。

**当前临时处置**：`AppCoordinator.cpp` 保留副本，并在其上方以注释登记该漂移风险与本条编号。

---

## 4. 修复状态（2026-09-27 修复轮）

本节是本清单**防重复修复**的唯一依据。修复范围：`Atoms/ATOM-02_State_Arbitration/Inc`、`Src`，
以及新上提的 `Atoms/Public/StateArbitrationTypes.hpp`（同时登记进 `Atoms/Public/CMakeLists.txt`）。
`Atoms/Public/AppCoordinator` 仅同步注释，未改变行为。

**编译验证状态（2026-09-27 第二轮，已由审查方补充执行）**：本节各修复项与
`ATOM-01` 修复轮的当前源码已通过 WSL aarch64 交叉编译（`-Wall -Wextra -Wpedantic`，
**零 error、零 warning**），产物为 `ARM aarch64` ELF。此前记录的"本轮未执行编译"已不再适用。

| 编号 | 状态 | 回归验收点（期望行为） |
|---|---|---|
| ATOM02-BUG-001 | 已修复 | 第 1 发 `FORCE→SHOT_1_YAW`、第 2～4 发 `FORCE→SHOT_N_YAW`；第 4 发前 `FIRE_COMPLETED→CHARGE` 返回 `SHOT_STAGE_MISMATCH` |
| ATOM02-BUG-002 | 已修复（方案 B） | `EvaluateFireAuthorization` 只受急停/故障/停止发射/YAW 影响；`SubmitIntent(FIRE)` 仍拒绝低优先级来源（`LOWER_PRIORITY_CONFLICT`） |
| ATOM02-BUG-003 | 已修复 | `MANUAL`/`DEBUG` 下 `AuthorizeManualFire(SBUS\|WEB)` 可授权；`AUTO` 源或非手动/调试模式返回 `NOT_READY`/`LOWER_PRIORITY_CONFLICT` |
| ATOM02-BUG-004 | 已修复 | 确认后模式为 `MANUAL` 但 `software_estop` 与锁存仍为 `true`；解除动作完成后 `ReleaseSoftwareEstopLatch()` 返回 `true` 并恢复控制源 |
| ATOM02-BUG-005 | 已修复 | 各来源只写自身字段互不清零；`Reset()` 后 `stop_fire == true` |
| ATOM02-BUG-006 | 已修复 | `INFO`/`WARNING` 级故障不阻断普通意图；`SEVERE`/`ESTOP` 阻断；漏填等级按 `SEVERE` 处理 |
| ATOM02-BUG-007 | 已修复 | `SOFTWARE_ESTOP`/`RECOVERY`/`FAULT` 下 `IntentKind::SAFETY` 被接受，普通意图仍被拒绝 |
| ATOM02-BUG-008 | 已修复 | 自动流程启动只经 `BeginNewAutoFireRun()`；`run_id` 每次流程自增一次、`current_shot == 1` |
| ATOM02-BUG-009 | 已修复 | `MANUAL`/`DEBUG` 下 `STOP_FIRE_SET` 被接受；其他模式返回 `NOT_READY`；仍不受优先级/急停阻断 |
| ATOM02-BUG-010 | 已修复 | `IDLE` 下请求 `AUTO_TERMINATED` 或 SBUS 接管终止均返回 `AUTO_FLOW_NOT_STARTED` 且状态不变 |
| ATOM02-BUG-011 | 已修复 | `INIT_FAILED → FAULT` 返回 `UNKNOWN_TRANSITION`，初始化失败设备不可进入可操作状态 |
| ATOM02-BUG-012 | 已修复 | 事件超过 64 条后 `dropped_event_count()` 递增，`CopyEvents` 仍按最旧到最新返回 |
| ATOM02-BUG-013 | 已修复 | 类型集中在 `Atoms/Public/StateArbitrationTypes.hpp`；`Inc/StateArbitration.hpp` 只含策略与 `StateArbitrator` |
| ATOM02-BUG-014 | 已关闭 | 无需回归 |

### 4.1 上层接线必须注意的三点

1. **优先级只在意图路径判定（BUG-002 方案 B）**：`EvaluateFireAuthorization`、`AuthorizeFire`、
   `AuthorizeManualFire` 只给出前置条件结论，不再判优先级；SBUS/WEB 的介入冲突必须先经
   `SubmitIntent(FIRE)`。`FireAuthorization::priority_conflict` 因此恒为 `false`（字段保留以兼容类型）。
2. **急停两步语义（BUG-004）**：`ConfirmSoftwareEstop()` 只做 `SOFTWARE_ESTOP → MANUAL` 并保持锁存；
   解除锁存必须由 ATOM-14 在「换弹抬起完成 + 蓄力双电机回零 + 双限位均低电平」之后调用
   `ReleaseSoftwareEstopLatch()`。确认之后、解除动作完成之前：控制源已按 `Config.md` §7.2 正常选择，
   但普通命令仍被急停锁存拒绝（`Config.md` §15、`Interface.md` §17.3 第 7 条），仅 `IntentKind::SAFETY` 放行。
3. **恢复期动作走 SAFETY（BUG-007）**：`RECOVERY` 期间的回零、换弹抬起、仅发射机构三次发射，
   必须以 `IntentKind::SAFETY` 提交；`IsOperationalMode` 未放宽，普通控制意图在 `RECOVERY` 下仍被拒绝。

## 5. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.1 | 2026-09-27 | 首次登记。基于 `Inc/StateArbitration.hpp`、`Src/StateArbitration.cpp`、`PLAN.md`、`Atoms/Public/MarshlandTypes.hpp` 及 `Design/Interface.md`、`Design/Config.md`、`Design/PEF.md` 的静态审查形成 14 条缺陷；本轮未修改任何 Atom 代码。 |
| 0.2 | 2026-09-27 | 新增 §2.1「高优先级缺陷的源头修复方案」，为 ATOM02-BUG-001～004 提供可直接执行的修复设计与验收要点。ATOM02-BUG-014 经 aarch64 编译验证判定不成立并关闭。**本轮仍未修改 `Inc/`、`Src/`、`CMakeLists.txt` 中任何一行**；修复实施由本模块专属 Agent 执行。 |
| 0.3 | 2026-09-27 | 修复轮：ATOM02-BUG-001～013 全部修复（002 采用方案 B、004 采用确认/解除分离、010 采用幂等语义），014 已关闭；新增 §4「修复状态」作为防重复修复依据；共享类型上提 `Atoms/Public/StateArbitrationTypes.hpp`。本轮未执行编译，编译验证由用户执行。 |
| 0.4 | 2026-09-27 | 第二轮集成审查（`AppCoordinator` ↔ `StateArbitrator`）。**新增 ATOM02-BUG-015（缺少重启语义，中）与 ATOM02-BUG-016（`IsOperationalMode` 未导出导致上层复制语义，低）**。两条均为桥接层无法代偿、需本模块裁决的问题。桥接层侧已同步实施规避与登记：`RunGate()` 增加急停锁存与阻断故障判据、输入上报改为委托字段级 setter、`lastInputs()` 改读仲裁器、析构调用 `Shutdown()`、`Start()` 以 `kUninitializedRequired` 显式拒绝重复初始化。修复轮后的当前源码已通过 WSL aarch64 交叉编译（`-Wall -Wextra -Wpedantic`，零警告）。 |
| 0.5 | 2026-09-27 | 用户裁决：**不支持运行中重启**。ATOM02-BUG-015 据此**关闭**（不新增 `RESTART` 边，`kModeEdges` 保持现状即正确）；桥接层改以 `kRestartNotSupported` 永久拒绝第二次初始化，ATOM-01 侧同步收紧为待办。ATOM02-BUG-016 仍待裁决。新增 `Atoms/ATOM-14_Safety_Recovery/Guide.md` 作为 ATOM-14 编码前置指导（急停确认/解除两步、SAFETY 意图、零力矩帧、防错清单）。 |
