# ATOM-01_Runtime 工程接入说明

> 版本：1.0  
> 日期：2026-10-16  
> 适用模块：`Atoms/ATOM-01_Runtime`  
> 关联文档：`Design/Atomic/ATOM-01_Runtime.md`、`Atoms/ATOM-01_Runtime/PLAN.md`

本文档面向 `Entry.cpp`、`AppCoordinator` 和各 ATOM 适配器的装配人员，只描述生命周期协调器的工程接线，不重复各 ATOM 的硬件参数、配置键和内部实现。

## 1. 接入目标

ATOM-01 负责进程内各生命周期模块的注册、按序初始化、readiness 汇总、必需模块失败回滚，以及关闭时的逆序资源释放。

ATOM-01 不负责：

- 具体硬件驱动、发射流程或机构动作；
- 业务 `SystemMode` 转换、控制权仲裁或执行器授权；
- 打开或解析 JSON 配置；
- 创建 `main()` 或独立可执行文件。

各 ATOM 通过实现 `ILifecycleModule` 接入。普通 ATOM 不自行创建 `RuntimeCoordinator`，也不直接驱动整机启动或关闭；顶层统一由 `AppCoordinator` 连接 ATOM-01 与 ATOM-02。

## 2. CMake 接入方向

```text
Marshland executable
  -> Atoms/Public/AppCoordinator（顶层装配桥）
  -> Marshland::atom_01_runtime
  -> Atoms/Public 公共生命周期契约
```

要求：

1. 上层工程通过 `add_subdirectory(Atoms/ATOM-01_Runtime)` 或既有 Atoms 聚合加入本模块。
2. 本模块提供静态库 `atom_01_runtime` 和别名 `Marshland::atom_01_runtime`。
3. 需要直接链接本模块的装配目标使用：

   ```cmake
   target_link_libraries(<装配目标> PRIVATE Marshland::atom_01_runtime)
   ```

4. 本模块只依赖 C++ 标准库和 `Atoms/Public/ILifecycle.hpp`，不依赖第三方库或操作系统设备接口。
5. ARM 工具链、C++ 标准、警告选项和最终可执行文件由上层工程决定；本模块不定义 `project()`，不设置全局编译选项。
6. 顶层不得重复把 `RuntimeCoordinator.cpp` 加入其他目标。

## 3. 公共契约与头文件边界

| 头文件 | 接线用途 | 使用方 |
|---|---|---|
| `Atoms/Public/ILifecycle.hpp` | `RuntimeState`、`RuntimeStep`、`RuntimeContext`、`ModuleInitResult`、`StepResult`、`ILifecycleModule` 及对应公共工具 | 各 ATOM 适配器与装配层 |
| `Atoms/ATOM-01_Runtime/Inc/RuntimeCoordinator.hpp` | `RuntimeCoordinator`、`ModuleRegistration`、`RuntimeStartResult` 和注册错误类型 | 仅装配层与模块测试 |
| `Atoms/Public/AppCoordinator/Inc/AppCoordinator.hpp` | 整机 `Start()` / `Shutdown()`、业务状态映射和启动诊断快照 | 仅顶层装配层 |

边界要求：

- 普通 ATOM 只包含 `ILifecycle.hpp` 及自身所需的 Public 端口，不包含其他 ATOM 的私有头。
- 依赖方向保持 `Atom -> Public`，不得形成 `Atom -> Atom` 横向生命周期依赖。
- `RuntimeState::Standby` 只表示硬件与服务启动完成，不等于业务 `SystemMode::STANDBY`。
- 不得使用 `RuntimeState` 替代 ATOM-02 的业务状态、控制源或发射授权结论。

## 4. 生命周期适配器契约

每个需要纳入整机启动流程的 ATOM 提供一个非空 `std::unique_ptr<ILifecycleModule>`：

```cpp
class ILifecycleModule {
public:
    virtual ~ILifecycleModule() = default;

    virtual ModuleInitResult initialize(const RuntimeContext& context) = 0;
    virtual void shutdown() noexcept = 0;
    virtual const char* name() const noexcept = 0;
};
```

适配器要求：

1. `initialize()` 只执行本模块初始化，不隐藏其他 ATOM 的副作用。
2. 返回结果填写 `success` 和必要的错误信息；`module_name` 为空时，协调器使用 `name()` 回填。
3. `name()` 应返回稳定、非空且寿命覆盖调用期间的字符串，用于注册和初始化诊断。
4. `shutdown()` 必须幂等、不抛出异常，并释放本模块持有的线程、句柄和其他资源。
5. 初始化失败或抛出异常前，适配器应自行清理本次部分初始化获得的资源。协调器只会对返回成功的模块登记启动状态并随后调用 `shutdown()`。
6. 适配器只保存装配层注入的引用时，被引用对象的寿命必须覆盖适配器在 `RuntimeCoordinator` 中的整个持有期。

## 5. RuntimeContext 接线

`RuntimeContext` 包含：

- `configuration_path`：配置文件路径；
- `values`：只用于明确约定的无类型透传参数。

顶层常规配置路径为：

```cpp
marshland::runtime::RuntimeContext context{};
context.configuration_path = "./Config/dart_control.json";
marshland::runtime::RuntimeCoordinator runtime{context};
```

要求：

- ATOM-01 只保存并透传上下文，不打开或解析 JSON。
- 配置加载和校验由 ATOM-15 完成；普通配置消费者使用同一个 `IConfigView`。
- 不得用 `RuntimeContext::values` 建立第二份配置真相。只有具体 ATOM 接入文档明确要求的透传键才能写入该字典。
- 构造 `RuntimeCoordinator` 后，调用方对原始 `context` 的后续修改不会替换协调器已经保存的上下文。

## 6. 装配对象图

顶层创建并持有以下核心对象：

```text
StateArbitrator arbitrator
IMonotonicClock 实现 clock
RuntimeContext context
RuntimeCoordinator runtime{context}
AppCoordinator app{runtime, arbitrator, clock}
各 ATOM 核心对象与 LifecycleAdapter
  -> 所需 Public 端口由 app 或其他长期服务对象提供
  -> adapter 的所有权通过 addModule() 转移给 runtime
```

参考构造关系：

```cpp
#include "AppCoordinator.hpp"
#include "RuntimeCoordinator.hpp"
#include "StateArbitration.hpp"
#include "SystemMonotonicClock.hpp"

marshland::state_arbitration::StateArbitrator arbitrator;
marshland::app::SystemMonotonicClock clock;
marshland::runtime::RuntimeCoordinator runtime{context};
marshland::app::AppCoordinator app{runtime, arbitrator, clock};

// 随后创建各 ATOM 适配器、注入 Public 端口并完成注册。
// 全部注册结束后才能调用 app.Start()。
```

寿命要求：

1. `runtime`、`arbitrator` 和 `clock` 必须比 `app` 长寿。
2. `app` 必须比所有借用其 Public 端口的对象和状态视图长寿。
3. 各 ATOM 核心对象必须比引用它们的适配器长寿；适配器所有权转交后，不得在顶层继续保留并释放同一份所有权。
4. 顶层必须保证正常关闭发生在适配器依赖对象销毁之前。上述代码只展示构造关系，具体声明顺序还需同时满足各 ATOM 接入文档的寿命约束。

## 7. 模块注册

### 7.1 启动步骤

不同步骤按以下枚举顺序执行：

```text
CreateDiagnostics
LoadConfiguration
ValidateConfiguration
LoadPidParameters
InitializeSocketCan
InitializeMotorAndEncoderChannels
InitializeGpioAndLimitInputs
InitializeIicPca9685
InitializeServoOutputs
InitializeSbusAndVision
StartHttpAndWebSocket
```

同一步骤允许注册多个模块，并按注册先后顺序执行。各 ATOM 的步骤和 `required` 属性以其接入文档及系统启动策略为准，不得在 ATOM-01 内另行猜测或补造。

### 7.2 注册示例

```cpp
auto adapter = std::make_unique<ConcreteLifecycleAdapter>(dependencies);

const bool registered = runtime.addModule({
    marshland::runtime::RuntimeStep::InitializeSocketCan,
    true,
    std::move(adapter)
});

if (!registered) {
    std::fprintf(stderr,
                 "[marshland] lifecycle registration failed: %s\n",
                 marshland::runtime::toString(runtime.lastRegistrationError()));
    // 按顶层失败路径退出，不得继续启动。
}
```

注册要求：

- 只能在 `RuntimeState::Created` 阶段、首次启动之前注册；
- `module` 必须非空，`required` 应显式填写；
- 每次 `addModule()` 都必须检查返回值，失败时读取 `lastRegistrationError()`；
- 不得创建占位模块掩盖注册失败，也不得把初始化结果当作注册结果；
- 同一步骤需要多个适配器时分别注册，不得合并成一个隐藏多模块副作用的适配器；
- `registeredModuleCount()` 返回真实注册数量，供 `AppCoordinator` 在启动前拒绝空注册表。

## 8. 整机启动

正常产品装配统一调用 `AppCoordinator::Start()`，而不是绕过它直接调用 `runtime.start()`：

```cpp
marshland::runtime::ModuleInitResult detail{};
const int result = app.Start(&detail);
if (result != marshland::app::AppCoordinator::kOk) {
    std::fprintf(stderr,
                 "[marshland] startup failed: %s: %s\n",
                 detail.module_name.c_str(),
                 detail.error_message.c_str());
    // 记录诊断并按顶层失败路径关闭或退出。
}
```

`Start()` 的接入语义：

1. 启动前必须完成全部模块注册，且至少注册一个真实模块。
2. 运行时必须仍处于 `Created`，业务模式必须满足 ATOM-02 的启动前置条件。
3. `AppCoordinator` 先推进业务 `INITIALIZING`，再调用 ATOM-01 启动序列，最后把结果映射为业务 `STANDBY` 或 `INIT_FAILED`。
4. 不支持进程内重启。启动失败后不得通过重复调用 `Start()` 或 `runtime.start()` 制造第二次初始化；需要重新初始化时重启进程。

## 9. 启动结果与诊断

ATOM-01 的启动结果关系如下：

```text
RuntimeStartResult
  ├─ state：Created / Starting / Standby / InitFailed / ShuttingDown / Stopped
  └─ steps：StepResult 列表
       ├─ step / required / attempted
       └─ ModuleInitResult
            ├─ success
            ├─ module_name
            └─ error_message
```

结果规则：

- `RuntimeStartResult::succeeded()` 仅在状态为 `RuntimeState::Standby` 时返回 `true`。
- 可选模块失败会保留诊断，但不阻断后续启动。
- 任一必需模块失败会停止后续初始化，进入 `InitFailed`，并逆序关闭此前已成功启动的模块。
- 模块异常会转换为失败结果；协调器内部失败同样进入回滚路径，不向 `Start()` 调用方抛出异常。
- 单独调用 `runtime.start()` 不会拒绝空注册表；空注册表拦截由 `AppCoordinator::Start()` 负责。

顶层诊断使用拥有数据副本的接口：

```cpp
const auto startup = app.startup();
const auto steps = app.stepResults();
```

`RuntimeCoordinator` 当前不提供独立的 `stepResults()` getter；不要依据旧接线代码调用该函数。启动中的调用方直接使用本次 `start()` 的返回值，运行期查询使用 `AppCoordinator` 保存的快照。

## 10. 关闭与释放

1. 正常产品退出统一调用 `app.Shutdown()`，不要在各 ATOM 中分别停止其他模块。
2. ATOM-01 只对成功启动的模块调用 `shutdown()`，并按启动顺序的逆序执行。
3. 单个模块关闭失败不得阻止其余模块释放；`RuntimeCoordinator::shutdown()` 和析构路径均为幂等关闭边界。
4. `Stopped`、`InitFailed` 或 `Standby` 状态之后再次调用 `runtime.start()` 只返回既有快照，不会重新初始化。
5. 不得以 `RuntimeState::Standby` 作为普通执行器动作的独立安全闸门；业务放行由 `AppCoordinator` 和 ATOM-02 判定。

## 11. 并发与职责边界

- 模块注册、`Start()` 和 `Shutdown()` 由顶层集中驱动；各 ATOM 工作线程不得直接并发调用 `RuntimeCoordinator` 的生命周期函数。
- `AppCoordinator` 负责序列化整机启动和关闭，并在调用硬件生命周期前后处理业务状态锁。
- 初始化或关闭期间需要上报输入、读取状态或提交意图的适配器，必须使用注入的 Public 端口，不得包含或另存 ATOM-02 私有对象。
- ATOM-01 只报告硬件与服务生命周期事实，不实现第二套业务模式或执行器安全策略。

## 12. 接线验收清单

- [ ] CMake 目标 `Marshland::atom_01_runtime` 可用，未创建独立可执行文件或 `main()`。
- [ ] 普通 ATOM 只依赖 `ILifecycle.hpp` 和 Public 窄端口，没有横向包含其他 ATOM 私有头。
- [ ] 每个适配器均实现 `initialize()`、幂等 `shutdown()` 和稳定的 `name()`。
- [ ] 所有模块在首次启动前注册，步骤正确、`required` 明确、模块指针非空。
- [ ] 同一步骤的多模块注册顺序符合预期，没有合并隐藏副作用。
- [ ] 每次注册都检查返回值，失败时读取 `lastRegistrationError()`，未使用占位模块。
- [ ] `RuntimeContext::configuration_path` 指向约定配置文件，未用 `values` 建立第二份配置真相。
- [ ] 正常产品启动经 `AppCoordinator::Start()`，空注册表和非法重复启动会被拒绝。
- [ ] 必需模块失败能进入 `INIT_FAILED` 并回滚，可选模块失败仍保留诊断。
- [ ] 诊断使用 `app.startup()`、`app.stepResults()` 或本次启动返回值，没有调用已移除的运行时 getter。
- [ ] 正常退出经 `app.Shutdown()`，所有已启动模块逆序释放，未发生重复释放。
- [ ] 业务模式与执行器授权仍由 ATOM-02 / `AppCoordinator` 负责。
- [ ] 上层 ARM 增量编译通过，并执行启动顺序、可选失败、必需失败、异常转换、空注册表和析构释放测试。

## 13. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-16 | 初版：明确 ATOM-01 的 CMake 目标、公共契约、适配器要求、上下文透传、模块注册、AppCoordinator 启动/关闭、诊断快照和寿命边界；对齐当前接口，诊断不使用已移除的 `RuntimeCoordinator::stepResults()`。 |

本文档编写阶段不执行编译；ARM 增量构建和运行时验证由上层工程完成。
