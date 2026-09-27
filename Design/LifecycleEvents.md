# LifecycleEvents.md — 模块生命周期事件公共契约

## 元信息

| 项目 | 内容 |
|---|---|
| 定义方 | ATOM-01_Runtime（进程生命周期与启动协调） |
| 状态 | 生效 |
| 生产者 | ATOM-01_Runtime（`RuntimeCoordinator`） |
| 消费者 | ATOM-18_Diagnostics_Logging（诊断与日志）及状态查询接口 |
| 上游依据 | `Design/Interface.md` §4.3、§7.2 第 6 条、§19；`Design/Config.md` §16.1、§16.2、§16.3；`Design/Atomic/ATOM-01_Runtime.md` |
| 本文件性质 | 跨 ATOM 的事件契约；不定义任何具体配置数值、日志文件格式、轮转与容量策略 |

## 1. 目的与边界

`ATOM-01_Runtime.md` 要求模块初始化结果可查询，`Interface.md` §7.2 第 6 条要求“状态迁移必须产生可查询的状态事件和原因”。本文件把 ATOM-01 在启动与关闭过程中产生的事件固定为**结构化契约**，使 ATOM-18 能以统一字段消费，而无需 ATOM-01 反向依赖 ATOM-18，也无需各模块各自解释原子步骤。

**本文件定义**：事件字段模型（§2）、事件类型（§3）、等级下限（§4）、生产与消费边界（§5）。

**本文件不定义**：

- 日志输出位置、文件名、格式、轮转、容量与刷新策略（属 `Config.md` §16.1 与 ATOM-18）。
- 故障码表与故障等级划分（属 `Config.md` §14.2 与 ATOM-14）。
- 业务 `SystemMode` 状态机事件（属 ATOM-02）。
- 任何具体硬件/服务参数与默认值（属 `Config.md`；其 §17 第 8 条禁止实现创建隐含默认值）。

## 2. 事件模型

每个事件是一份不可变记录，字段与 `Interface.md` §19 的 `LogEvent` 一一对应，ATOM-18 直接映射即可落盘：

| 字段 | 类型 | 语义 | 对应 `LogEvent` 字段 |
|---|---|---|---|
| `event` | 枚举（§3） | 事件类型 | `event` |
| `run_id` | 无符号整数 | 一次启动序列的标识，随 `start()` 递增，同一进程内唯一 | `request_id` |
| `sequence` | 无符号整数 | 同一 `run_id` 内的事件序号，从 0 起严格递增 | `payload.sequence` |
| `timestamp_utc_ms` | 有符号整数 | UTC 墙钟毫秒（`Config.md` §16.1），用于日志时间戳 | `timestamp` |
| `monotonic_us` | 有符号整数 | 单调时钟微秒（`Interface.md` §3.3），用于排序与超时，不受校时影响 | `payload.monotonic_us` |
| `level` | 枚举（§4） | 事件等级 | `level` |
| `module` | 字符串 | 模块名，取 `ILifecycleModule::name()`；无模块的事件为空 | `module` |
| `step` | 枚举 | `RuntimeStep`（ATOM-01 私有生命周期步骤）；无步骤的事件为空 | `payload.step` |
| `state` | 枚举 | 事件发生后的 `RuntimeState` | `state` |
| `required` | 布尔 | 该模块是否为必需项；无模块的事件为假 | `payload.required` |
| `error_code` | 字符串 | 注册/初始化失败原因（如 `ModuleRegistrationError`）；成功时为空 | `error_code` |
| `detail` | 字符串 | 模块返回的错误说明；成功时为空 | `payload.detail` |

**`state` 的语义边界**：本字段是 ATOM-01 的 `RuntimeState`（`Created` / `Starting` / `Standby` / `InitFailed` / `ShuttingDown` / `Stopped`），**不是**业务 `SystemMode`。启动结果到业务模式的映射由装配层 `Atoms/Public/AppCoordinator` 唯一承担（见 `Atoms/ATOM-01_Runtime/PLAN.md` §5.1），本契约不重复表达业务状态。

**来源字段**：`Interface.md` §19 的 `source` 在 ATOM-01 事件中恒为 `runtime-coordinator`，因此不单列；如需区分控制源，由消费方按 `Config.md` §7.2 的规则另行补充。

## 3. 事件类型

| `event` | 触发点 | `module` / `step` | 说明 |
|---|---|---|---|
| `START_BEGIN` | `RuntimeCoordinator::start()` 进入启动序列 | 均为空 | 开始一次 `run_id`；`state = Starting` |
| `STEP_BEGIN` | 每个步骤的首个模块 `initialize()` 之前 | `step` | 步骤进入 |
| `MODULE_INIT_OK` | 模块 `initialize()` 成功返回 | `module` + `step` | 单模块结果 |
| `MODULE_INIT_FAILED` | 模块 `initialize()` 返回失败或抛异常 | `module` + `step` | 必须携带 `error_code` 与 `detail` |
| `STEP_END` | 该步骤全部模块执行完毕 | `step` | 步骤结果汇总 |
| `START_END` | 启动序列结束 | 均为空 | `state` 为 `Standby` 或 `InitFailed` |
| `ROLLBACK_BEGIN` | 必需模块失败，开始逆序释放 | 均为空 | 仅失败路径；`state = InitFailed` |
| `MODULE_SHUTDOWN` | `shutdown()` 或回滚对某模块调用完成 | `module` | 每个已启动模块恰好一次 |
| `SHUTDOWN_BEGIN` | `RuntimeCoordinator::shutdown()` 进入释放 | 均为空 | `state = ShuttingDown` |
| `SHUTDOWN_END` | `shutdown()` 释放完成 | 均为空 | `state = Stopped` |

**事件不变量**（供 ATOM-18 与测试校验）：

1. 每个实际执行的步骤：`STEP_BEGIN` 与 `STEP_END` 成对出现。
2. 每个被调用过 `initialize()` 的模块：恰好一个 `MODULE_INIT_OK` 或 `MODULE_INIT_FAILED`。
3. 每个 `MODULE_INIT_OK` 的模块：最终恰好一个 `MODULE_SHUTDOWN`（无论经由正常关闭还是失败回滚）。
4. 同一 `run_id` 内 `sequence` 严格递增；重启产生新的 `run_id` 并从 0 重新计数（对应 `ATOM01-BUG-003` 的允许重启语义）。

## 4. 事件等级

等级语义取自 `Config.md` §16.1（默认 INFO；故障与安全事件至少 WARN；严重故障使用 ERROR）。本契约只固定等级**下限**：

| 事件 | 等级下限 |
|---|---|
| `START_BEGIN` / `STEP_BEGIN` / `STEP_END` / `MODULE_INIT_OK` / `START_END`（成功） | `INFO` |
| 可选模块（`required == false`）的 `MODULE_INIT_FAILED`、`SHUTDOWN_BEGIN` / `MODULE_SHUTDOWN` / `SHUTDOWN_END` | `WARN` |
| 必需模块的 `MODULE_INIT_FAILED`、`ROLLBACK_BEGIN`、`START_END`（`InitFailed`） | `ERROR` |

具体等级上限、输出与刷新策略由 ATOM-18 按 `Config.md` §16.1 决定，本契约不覆盖。

## 5. 生产与消费边界

依赖方向必须保持 `ATOM -> Public`，不得出现 `ATOM-01 -> ATOM-18`：

1. 事件类型与抽象接收端 `ILifecycleEventSink` 属于跨 ATOM 契约，应置于 `Atoms/Public/`（纯类型与接口，无实现、无 ATOM 依赖）。
2. ATOM-01 在 §3 的生产点产出事件并调用被注入的 `ILifecycleEventSink`；未注入时（`nullptr`）静默跳过，且**不得**因此改变启动或关闭的语义、顺序与返回结果。
3. ATOM-18 实现该接收端；装配层 `Atoms/Public/AppCoordinator` 负责把 ATOM-18 的实例注入 ATOM-01，作为唯一组装点。
4. 事件产出位于既有步骤边界，不新增线程切换，不阻塞启动序列。

> **实施状态**：本文件仅确立契约。`ILifecycleEventSink` 与 ATOM-01 内的事件产出**尚未实现**，需另行授权；在此之前 ATOM-01 仅提供 `RuntimeStartResult` / `StepResult` 终态快照。

## 6. 与既有实现的关系

ATOM-01 当前已提供的可查询信息是本契约的结构化子集：

- `RuntimeCoordinator::stepResults()` → `std::vector<StepResult>`，每模块一条，含 `step` / `required` / `attempted` / `ModuleInitResult`。
- `RuntimeStartResult::state` 与 `succeeded()`。
- `RuntimeCoordinator::lastRegistrationError()`（回应 `ATOM01-BUG-009` 的“注册失败无原因”）。

`ATOM01-BUG-008` 指出的缺口正是“无时间戳、无事件序号、无等级、无法与 ATOM-18 事件流对齐”；本契约通过 `run_id` / `sequence` / `timestamp_utc_ms` / `monotonic_us` / `level` 补齐。

## 7. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.1 | 2026-09-27 | 首次定义。由 ATOM-01 依 `Interface.md` §4.3/§7.2/§19、`Config.md` §16.1/§16.2/§16.3 与 `Design/Atomic/ATOM-01_Runtime.md` 形成；回应 `ATOM01-BUG-008`。代码侧实施待授权。 |
