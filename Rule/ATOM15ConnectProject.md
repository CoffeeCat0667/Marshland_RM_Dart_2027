# ATOM-15_Config_Persistence 顶层网关接入说明

> 版本：1.0
> 日期：2026-10-11
> 适用对象：`NewVersion/Entry.cpp`、`Atoms/Public/AppCoordinator` 及顶层对象装配代码

## 1. 接入目标

ATOM-15 是唯一负责打开、解析、校验 `./Config/dart_control.json` 并提供 `marshland::config::IConfigView` 的模块。

顶层网关必须完成两件事：

1. 创建并长期持有 `marshland::atom15::ConfigStore`；
2. 创建 `marshland::atom15::LifecycleAdapter`，把它注册到 ATOM-01 的 `RuntimeStep::LoadConfiguration`。

其他 ATOM 不得自行打开 JSON、解析 JSON 或使用 `RuntimeContext::values` 代替 `IConfigView`。

## 2. CMake 依赖

上层工程已经通过 `add_subdirectory(Atoms)` 创建：

```text
Marshland::atom_15_config_persistence
Marshland::Atoms
```

ATOM-15 自己的目标为：

```text
atom_15_config_persistence
Marshland::atom_15_config_persistence
```

`Entry.cpp` 所属的最终可执行目标应链接 `Marshland::Atoms` 或直接链接 `Marshland::atom_15_config_persistence`。不需要在顶层重复加入 ATOM-15 的源码文件，也不应修改 ATOM-15 的静态库实现方式。

ATOM-15 的头文件通过其 `Inc` 包含目录提供：

```cpp
#include "atom15_config/config_store.hpp"
```

## 3. 对象图与生命周期

对象创建顺序必须保证被引用对象的寿命长于生命周期适配器：

```text
ConfigStore
  ├─ IConfigView 只读视图，供 ATOM-03～19 注入
  └─ LifecycleAdapter(ConfigStore&)
        └─ RuntimeCoordinator::addModule(...)
```

推荐的顶层声明顺序：

```cpp
marshland::atom15::ConfigStore config_store;
marshland::runtime::RuntimeContext context{};
context.configuration_path = "./Config/dart_control.json";
marshland::runtime::RuntimeCoordinator runtime{context};
```

由于 C++ 局部对象按逆序析构，以上顺序保证 `runtime` 先析构，`config_store` 后析构。`RuntimeCoordinator` 内部持有的 `LifecycleAdapter` 只保存 `ConfigStore&`，不能让 `ConfigStore` 先于 `RuntimeCoordinator` 销毁。

## 4. 注册 ATOM-15

必须在首次 `runtime.start()` 前完成注册，且检查返回值：

```cpp
#include <cstdio>
#include <memory>

#include "RuntimeCoordinator.hpp"
#include "atom15_config/config_store.hpp"

marshland::atom15::ConfigStore config_store;
marshland::runtime::RuntimeContext context{};
context.configuration_path = "./Config/dart_control.json";
marshland::runtime::RuntimeCoordinator runtime{context};

auto config_adapter = std::make_unique<marshland::atom15::LifecycleAdapter>(config_store);
const bool registered = runtime.addModule({
    marshland::runtime::RuntimeStep::LoadConfiguration,
    true,
    std::move(config_adapter)
});
if (!registered) {
    std::fprintf(stderr,
                 "[marshland] ATOM-15 registration failed: %s\n",
                 marshland::runtime::toString(runtime.lastRegistrationError()));
    return 1;
}
```

注册要求：

- `step` 必须为 `RuntimeStep::LoadConfiguration`；
- `required` 必须为 `true`，配置加载/校验失败时必须进入 `INIT_FAILED`；
- `module` 必须为非空的 `std::unique_ptr<ILifecycleModule>`；
- `addModule()` 返回 `false` 时必须读取 `lastRegistrationError()`；
- 必须在首次 `runtime.start()` 前注册；
- 不要把 ATOM-15 同时注册到 `ValidateConfiguration`，配置加载和校验由同一个适配器一次完成；
- 不要创建占位模块来掩盖注册失败。

## 5. 启动时序

ATOM-15 的适配器在 ATOM-01 启动序列的 `LoadConfiguration` 步骤执行：

```text
RuntimeCoordinator::start()
  -> ATOM-15 LifecycleAdapter::initialize(context)
       -> ConfigStore::load(context.configuration_path)
            -> 打开 JSON
            -> 解析 JSON
            -> 校验 schema 必需结构、类型和核心范围
            -> 生成并发布只读配置快照
            -> configVersion() 递增
  -> success == false
       -> required 模块失败
       -> ATOM-01 进入 RuntimeState::InitFailed
       -> 已启动模块回滚
  -> success == true
       -> 后续 ATOM 在自己的初始化阶段通过 IConfigView 查询
```

ATOM-15 不负责驱动硬件、不负责启动其他模块，也不负责业务 `SystemMode` 切换。`RuntimeCoordinator::Standby` 与业务 `SystemMode::STANDBY` 仍由既有装配逻辑区分处理。

## 6. 向其他 ATOM 注入 `IConfigView`

`ConfigStore` 实现了：

```cpp
marshland::config::IConfigView
```

顶层装配层应把同一个 `config_store` 引用注入配置消费者。例如 ATOM-03：

```cpp
marshland::atom03::LifecycleAdapter sbus_adapter(sbus_core, config_store);
```

ATOM-03 的消费者只能包含：

```cpp
#include "IConfigView.hpp"
```

并按 `Rule/ConfigSchema.md` 的键名查询。顶层不应为每个 ATOM 创建第二个配置解析器或第二个配置快照。

当前 ATOM-03 已实际查询以下键：

| 键 | 类型 | 用途 |
|---|---|---|
| `sbus.serial_device` | string | UART 设备路径 | 
| `sbus.channel_scale_x` | int | 连续通道缩放，1～10 |
| `timing.sbus_receive_period_ms` | int | SBUS 接收周期 |
| `timing.sbus_loss_timeout_ms` | int | SBUS 失联判定 |
| `sbus.frame_bytes` | int | 帧长度，25 |
| `sbus.joystick_center` | int | 中心值，1024 |
| `sbus.valid_min` | int | 有效下限，364 |
| `sbus.valid_max` | int | 有效上限，1684 |

所有查询都必须检查返回值：

```cpp
std::int64_t value = 0;
const auto status = config.getInt("sbus.channel_scale_x", value);
if (status != marshland::config::ConfigStatus::OK) {
    // 返回当前模块初始化失败，并保留 ToString(status) 诊断信息。
}
```

## 7. 热加载与运行时更新

运行时读取必须经 `IConfigView` 完成：

- 查询接口只读取内存快照，不执行文件 I/O；
- `configVersion()` 单调递增；
- 消费方可缓存上次版本号，发现版本变化后重新读取自身配置；
- 字符串指针只在当前配置版本未变化期间有效；
- 不要在控制线程中保存跨版本的字符串指针。

配置更新和 PID 更新由 ATOM-15 的 `ConfigStore` 执行，授权条件是：

```text
source == WEB
mode == MANUAL 或 DEBUG
```

更新结果必须分别处理：

```cpp
const auto result = config_store.updateRuntime(
    "sbus.channel_scale_x",
    std::int64_t{2},
    marshland::atom15::UpdateSource::WEB,
    marshland::atom15::UpdateMode::DEBUG);

// result.effective   —— 运行时快照是否已生效
// result.persisted   —— JSON 文件是否写入成功
// result.error       —— 失败原因
```

当 `effective == true` 且 `persisted == false` 时，网关不得向外报告“全部成功”；运行时状态与文件状态必须分开报告。

## 8. 初始配置文件

初始文件位置：

```text
NewVersion/Config/dart_control.json
```

`Entry.cpp` 的 `RuntimeContext` 必须使用：

```cpp
context.configuration_path = "./Config/dart_control.json";
```

程序工作目录应为 `NewVersion`，或由部署脚本保证该相对路径指向同一配置文件。

当前初始文件包含设计中已确定的 6 个电机：

- 1：右蓄力机构，`0x201`；
- 2：左蓄力机构，`0x202`；
- 3：右换弹机构（M3508），`0x203`；
- 4：左换弹机构（M3508），`0x204`；
- 5：YAW 机构，`0x205`；
- 6：变力机构，`0x206`。

所有电机的 CAN 节点已明确，初始文件完整包含这些映射。
2. `Rule/ConfigSchema.md`；
3. `Rule/ConfigSchema.schema.json`；
4. `Config/dart_control.json`；
5. ATOM-05、ATOM-10 及相关接入说明。

## 9. 启动失败处理

以下情况必须使 ATOM-15 初始化失败，并由 ATOM-01 进入 `INIT_FAILED`：

- 配置文件不存在或不可读；
- JSON 语法损坏；
- `schema_version` 不支持；
- 必需顶层结构缺失；
- 电机映射或 PID 字段缺失/类型错误；
- ATOM-03 使用的配置键缺失、类型错误或超出范围。

不得：

- 自动修复损坏文件；
- 用猜测值替换 `[待定]`；
- 自动钳位越界参数；
- 将配置文件写入失败伪装为成功；
- 在 ATOM-15 之外重新解析 JSON。

## 10. 接入自检清单

- [ ] `ConfigStore` 在 `RuntimeCoordinator` 之前创建，并且寿命长于所有适配器。
- [ ] `RuntimeContext::configuration_path` 指向 `./Config/dart_control.json`。
- [ ] ATOM-15 只注册一次，步骤为 `RuntimeStep::LoadConfiguration`。
- [ ] `required == true`。
- [ ] `addModule()` 返回值已检查。
- [ ] 注册失败时已读取 `lastRegistrationError()`。
- [ ] `runtime.start()` 之前注册已完成。
- [ ] ATOM-03 等消费者使用同一个 `IConfigView` 实例。
- [ ] 消费方检查每个 `ConfigStatus`，没有把非 `OK` 当成功。
- [ ] 没有任何 ATOM 自行打开或解析 JSON。
- [ ] 没有向 `RuntimeContext::values` 写入第二份配置真相。
- [ ] `configVersion()` 变化后消费者会重新读取配置。
- [ ] PID 更新仅接受 WEB + MANUAL/DEBUG。
- [ ] `effective` 与 `persisted` 状态分别处理。
- [ ] 初始配置中的换弹 `[待定]` 映射没有被臆造。

## 11. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-11 | 新增 ATOM-15 顶层网关注册、对象生命周期、IConfigView 注入、启动失败、热加载和初始配置接入说明。 |
