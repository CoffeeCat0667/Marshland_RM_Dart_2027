# ATOM-15 Config Persistence 实现方案

## 1. 目标与边界

ATOM-15（Marshland）是**配置的唯一解析与校验方**：读取唯一配置源 `./Config/dart_control.json`，
完成解析与按 `Design/Config.md` 规则的字段/类型/范围校验，向其余模块提供**已校验的**参数查询；
同时支持运行时配置更新、PID 参数持久化和最多 100 个额外持久化变量的读写。

本 Atom 不提供 `main` 入口，不负责启动编排、执行器生命周期、硬件控制策略或 Config 规则本身的重新定义。
PID 数值、边界和持久化策略均从 Config 读取。

---

## 2. 与已实现接口的兼容性（**最高优先级约束**）

本节是本方案的前提。**已实现的接口是权威，本模块必须适配它们，不得反向要求它们改动。**
权威来源：`Atoms/Public/ILifecycle.hpp`、`Atoms/Public/MarshlandTypes.hpp`、
`Atoms/Public/StateArbitrationTypes.hpp`、`Atoms/Public/AppInterfaces.hpp`、
`Atoms/ATOM-01_Runtime/Inc/RuntimeCoordinator.hpp`。
接入规则另见 `Rule/AtomIntegration.md`。

### 2.1 配置注入的实际机制（已实现，不可协商）

```cpp
// Atoms/Public/ILifecycle.hpp
struct RuntimeContext {
    std::string configuration_path;                        // 配置文件路径
    std::unordered_map<std::string, std::string> values;   // 无类型字符串字典
};

class ILifecycleModule {
    virtual ModuleInitResult initialize(const RuntimeContext& context) = 0;
    virtual void shutdown() noexcept = 0;
    virtual const char* name() const noexcept = 0;
};
```

传递链（已实现行为）：

```text
Entry.cpp                                 构造 RuntimeContext，填写 configuration_path
        │
        ▼
RuntimeCoordinator::RuntimeCoordinator(RuntimeContext context)
        │  按值拷贝（std::move）为私有成员 context_
        ▼
RuntimeCoordinator::start()  循环
        │  registered.module->initialize(context_)   —— 按 const 引用，交付一次
        ▼
各模块 LifecycleAdapter::initialize(const RuntimeContext& ctx)
```

**由此推出三条硬约束：**

| # | 约束 | 后果 |
|---|---|---|
| C1 | `initialize()` 收到的是 **const 引用** | 本模块**不能**把解析结果写回该 `RuntimeContext` 供后续模块读取 |
| C2 | `RuntimeContext` 由 `RuntimeCoordinator` **按值私有持有** | 不存在"共享可变配置"通道；本模块无法从外部修改协调器内部副本 |
| C3 | ATOM-01 **明确不解析配置**（源码注释原文："The coordinator deliberately does not parse configuration"） | 解析与校验责任全部在本模块，不得指望协调器代劳 |

### 2.2 结论：本模块必须**既消费又提供**

- **消费**：从 `initialize(context)` 的 `context.configuration_path` 取配置文件路径；该路径是
  本模块启动的唯一入口信息。
- **提供**：本模块对外提供**配置查询接口**，供其余模块在运行期取得**已校验**的值。

`RuntimeContext::values` 是由装配层预填的可选通道，**不得**作为本模块向他人供值的唯一手段
（原因见 C1/C2）。本模块的查询接口才是运行期的权威读取路径。

### 2.3 契约面：已实现的四个端口（其余模块经此与本模块交互）

`Atoms/Public/AppInterfaces.hpp` 已实现的端口**不包含配置查询**。本模块**必须**以新增的纯虚接口
形式声明自己的契约，并遵循 `Rule/AtomIntegration.md` §6.2 的"接口注入"约定：

- 接口头文件位置：`Atoms/Public/`（跨模块契约统一放此处，供其余模块包含）。
- 其余模块**禁止**包含本模块的 `Inc/` 私有头（`Rule/AtomIntegration.md` §6.1）。
- 发起方只依赖该接口；由**装配层**把本模块的实现注入给它们。

建议形态（名称可在实现阶段定稿，语义不得更改）：

```cpp
// 建议置于 Atoms/Public/，供 ATOM-03..19 依赖
enum class ConfigStatus : std::uint8_t { OK, MISSING, INVALID_TYPE, OUT_OF_RANGE, NOT_LOADED };

class IConfigView {
public:
    virtual ~IConfigView() = default;

    // 已校验取值；失败必须可区分原因，禁止以默认值静默掩盖
    virtual ConfigStatus getInt(const char* key, std::int64_t& out) const noexcept = 0;
    virtual ConfigStatus getDouble(const char* key, double& out) const noexcept = 0;
    virtual ConfigStatus getBool(const char* key, bool& out) const noexcept = 0;
    virtual ConfigStatus getString(const char* key, const char*& out) const noexcept = 0;

    // 参数变更版本号：用于运行期热加载后重新读取（单调递增）
    virtual std::uint64_t configVersion() const noexcept = 0;
};
```

**禁止**在本模块内定义与其他 ATOM 重复的同名枚举/类型（`Rule/AtomIntegration.md`、`ATOM-02_PLAN.md` §3.1 纪律）。

### 2.4 生命周期接入

本模块**必须**实现 `ILifecycleModule`，并按 `Rule/AtomIntegration.md` §2 采用"内核 + 适配器"两层结构：

- 内核：`ConfigStore`（解析、校验、查询、持久化），不依赖任何 ATOM；
- 适配器：`LifecycleAdapter`，独立翻译单元，≤ 50 行，只做 `initialize()` / `shutdown()` 的编排与结果翻译。

注册步骤为 `RuntimeStep::LoadConfiguration`（`Rule/AtomIntegration.md` §3 映射表）。

**适配器行为契约：**

1. `initialize()` 从 `context.configuration_path` 取路径并加载；**禁止**在模块内硬编码路径。
2. 文件缺失、JSON 损坏、必需字段缺失、类型错误、范围错误**必须**返回 `success = false`，
   且 `error_message` 足以定位到具体键与原因。**禁止**把失败转成成功。
3. `error_message` 中不得回显配置里的敏感内容（本工程当前无敏感项，但保持该习惯）。
4. `shutdown()` 必须 `noexcept`、幂等、对"从未成功 initialize"的调用安全。
5. `required` 注册为 `true`：配置是全部模块的前提（`Design/Config.md` §17 第 1 条）。

### 2.5 初始化顺序与跨模块时序

`RuntimeCoordinator` 只保证**步骤顺序**与**同步 FIFO**，**不建立模块间链接**，也**不会**让
先注册的模块自动对后注册的模块可见。

因此：

1. `LoadConfiguration` 步骤**必须**早于所有需要配置的模块（映射表中它位列第 2 步，满足）。
2. 在 `LoadConfiguration` 步骤**之后**初始化的模块，可以在自己的 `initialize()` 内**直接**
   查询 `IConfigView` 取得已校验值。
3. 查询前提是发起方**已持有** `IConfigView` 引用 —— 该引用由装配层在**构造期**注入，
   而非在本模块 `initialize()` 之后才建立。装配层构造对象图时即可建立该引用
   （`ConfigStore` 是普通对象，不依赖"初始化完成"才存在）。
4. `RuntimeContext::values` 若由装配层预填，本模块**必须**优先信任由 `load()` 校验后的结果；
   两者冲突时以本模块校验结果为准，并在诊断中记录冲突。

### 2.6 运行期更新与热加载

`Design/Config.md` §13.1 要求"参数更新成功后立即生效"（热加载）。已实现的 `RuntimeContext`
是**按值快照**，无法承担该职责（约束 C1/C2）。因此热加载按以下方式实现：

```text
WEB（ATOM-16）提交参数更新
        │  经已实现端口 IControlAuthority / 既定路由
        ▼
本模块校验来源与模式
   * 仅接受 source == WEB
   * 仅手动（MANUAL）或调试（DEBUG）模式
   * 拒绝越界值，不改变当前有效值
        │  校验通过
        ▼
更新内存中的有效配置，configVersion() 自增
        │
        ▼
发布"参数已变更"事件（载体见 §7 待确认）
        │
        ▼
消费方按 configVersion() 或事件重新读取
```

**约束：**

1. **禁止**本模块直接向其他模块推送具体数值；它只发布"某键已变更 + 新版本号"这一事实。
2. 其余模块**禁止**反向依赖本模块的具体类型，只经 `IConfigView` 与事件端口交互。
3. `effective` 与 `persisted` 必须**分别**报告，**禁止**在文件写入失败时谎报全部成功。

---

## 3. 拟定模块结构

```text
ATOM-15_Config_Persistence/
├─ CMakeLists.txt
├─ PLAN.md
├─ Inc/atom15/
│  ├─ config_store.hpp          # 内核对外接口、数据结构、错误/状态定义
│  └─ lifecycle_adapter.hpp     # 生命周期适配器（唯一实现 ILifecycleModule）
└─ Src/
   ├─ config_store.cpp          # 配置加载、校验、查询和更新
   ├─ json_config_store.cpp     # JSON 文件读写与损坏/缺失处理
   ├─ pid_persistence.cpp       # 电机 PID 与额外持久化变量
   └─ lifecycle_adapter.cpp     # 适配器实现（≤ 50 行）
```

`IConfigView` 声明于 `Atoms/Public/`，不属于本目录 `Inc/`。

实现阶段若确认依赖库已经由 Public 或上层工程提供，将通过上层目标传递依赖；
不在本目录自行引入第三方库或修改上层 CMake。

---

## 4. 核心接口草案（内核）

- `load(path)`：读取并解析 JSON，区分文件缺失、JSON 损坏、必需字段缺失、字段类型错误和范围错误。
- `getInt/getDouble/getBool/getString(key)`：查询当前**已校验**配置键值，返回 `ConfigStatus`。
- `configVersion()`：单调递增的配置版本号，供消费方判断是否需要重新读取。
- `update_runtime(key, value, source, mode)`：仅接受 `source == WEB` 且 `mode` 为手动或调试；
  更新通过校验后立即作用于运行时配置并自增版本号。
- `persist()`：将已接受的运行时更新写回 JSON，并独立返回 `effective` 与 `persisted` 状态。
- `read_pid(motor_id)` / `write_pid(motor_id, pid)`：读取和保存单个电机 PID，保存后支持重启恢复。
- `read_extra(key)` / `write_extra(key, value)`：管理额外持久化变量，限制总数不超过 100。
- 参数变更事件：在更新成功且达到约定发布条件后发布，具体事件适配由调用方接入（载体见 §7）。

---

## 5. 关键行为与错误策略

1. 启动时先读取唯一配置源；必需的硬件/安全参数缺失或配置无法按规则校验时，`initialize()`
   返回失败，使系统进入 `INIT_FAILED`。
2. 缺失的可选参数使用 Config 定义的内置默认值；单字段错误必须能够独立报告。
3. 损坏文件只报告错误，不自动修复、不自动覆盖、不执行掉电保护或额外迁移。
4. PID 更新拒绝非 WEB 来源，以及非手动/调试模式；拒绝越界值，不改变活动 PID。
5. 运行时生效与文件持久化分别报告，不能在文件写入失败时谎报全部成功。
6. 持久化采用 Config 规定的普通文件写入语义，不额外承诺原子性或版本迁移。
7. 参数更新和 PID 保存使用明确的结果对象，便于上层分别处理 effective/persisted 状态和诊断信息。
8. **禁止**为 `Design/Config.md` 中标记 `[待定]` 的项臆造默认值；`[待定]` 项必须保持未实现
   并在本文件登记（`Rule/AtomIntegration.md` §5 第 3 条）。

---

## 6. CMake 集成方案

- 本目录提供 `atom15_config_persistence` 模块目标与别名 `Marshland::atom15_config_persistence`，
  作为上层 `add_subdirectory(...)` 后可链接的模块化目标。
- 有 `Src/*` 源码时建 STATIC 库，否则建 INTERFACE 库；上层统一使用别名链接。
- 不设置架构、编译器、C++ 标准或全局编译选项；全部由上层 `NewVersion/CMakeLists.txt` 决定
  （C++23、仅 aarch64/arm，非 ARM 架构 configure 直接 `FATAL_ERROR`）。
- 不创建可执行文件，不定义 `main`。
- `#include "ILifecycle.hpp"` 可直接解析：模块模板已把 `../Public` 加入包含路径。

---

## 7. 实现顺序

1. 固化公共数据结构、状态码、诊断信息与 `IConfigView` 接口（置于 `Atoms/Public/`）。
2. 实现 JSON 读取、类型/范围校验、默认值和必需项检查。
3. 实现 `LifecycleAdapter`：从 `context.configuration_path` 加载并按契约返回结果。
4. 实现运行时查询与更新，并保证更新成功立即自增 `configVersion()`。
5. 实现 JSON 持久化及 `effective`/`persisted` 双状态结果。
6. 实现电机 PID 和额外变量持久化上限检查（上限 100，命名 `reserved_{index:03d}`、`int32`）。
7. 接入热加载/参数变更事件回调。
8. 在允许的 ARM 工具链环境中编译并补充针对缺失、损坏、权限拒绝、越界和重启恢复的测试。

---

## 8. 待确认事项

1. **`dart_control.json` 的精确 schema、必需键、默认值和范围。**
   当前 `Config/` 目录与 `dart_control.json` **均不存在**，`Design/Config.md` 提供了约 200 项参数
   但未定义 JSON 键名。**这是本模块与全部消费方的前置阻塞项**：键名未定则消费方只能猜测，
   必然产生互不兼容的键（例如 `sbus.port` 与 `sbus_device` 并存）。
2. **`IConfigView` 的最终名称与签名**，以及它应放置在 `Atoms/Public/` 的哪个文件
   （是否并入已实现的 `AppInterfaces.hpp`，需 `Public` 契约决策）。
3. **"参数已变更"事件的载体**：已实现的端口集中没有参数分发口。
   需裁决是新增事件端口，还是复用 `Atoms/Public/AppInterfaces.hpp` 已有的读取端口
   （`IStateReader::CopyEvents` 属状态机事件，语义不同，不应混用）。
4. **电机 ID 的合法范围及 `PIDConfig` 的完整字段。**
5. **线程安全要求**：`update_runtime()` 由 WEB 线程触发，消费方在各自控制线程读取，
   需明确 `IConfigView` 的并发读保证与热加载时的读取一致性策略。
6. **是否允许装配层预填 `RuntimeContext::values`** 作为过渡通道；若允许，需明确其与
   `load()` 结果的优先级（本方案 §2.5 第 4 条建议以本模块校验结果为准）。

---

## 9. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.1 | 2026-09-27 | 初稿。 |
| 0.2 | 2026-09-27 | **按"已实现接口是权威"重写以适配**。原方案设计了一套独立的 `load/get/update_runtime` 接口，与已实现的 `ILifecycleModule` + `RuntimeContext` 契约不兼容，且在本模块 `initialize()` 之外无法向他人供值（原因见 §2.1 的 C1/C2/C3）。本版：新增 §2 兼容性约束（实际注入机制、必须既消费又提供、`IConfigView` 契约面、生命周期接入契约、初始化时序、热加载路径）；模块结构补入 `lifecycle_adapter`；待确认事项补齐 schema、`IConfigView` 归属、事件载体与并发四项。**本版仅修改文档，未改动任何代码。** |
