# ATOM-15 Config Persistence 实施方案与报告

## 1. 已核对的规范与实际用法

已只读核查并按以下资料实现：

- `Design/PRD.md`、`Design/PEF.md`、`Design/Interface.md`、`Design/Config.md`；
- `Design/Atomic/ATOM-01_Runtime.md`、`ATOM-02_State_Arbitration.md`、`ATOM-03_SBUS.md`、`ATOM-15_Config_Persistence.md`；
- `Design/LifecycleEvents.md`；
- `Rule/AtomIntegration.md`、`Rule/ConfigSchema.md`、`Rule/ConfigSchema.schema.json`、`Rule/ATOM3ConnectProject.md`；
- `Atoms/Public/IConfigView.hpp`、`ILifecycle.hpp`、`AppInterfaces.hpp`；
- ATOM-01 的 `RuntimeCoordinator` 实现、ATOM-02 的状态接口、ATOM-03 的真实 `IConfigView` 消费代码。

已确认的实际配置消费键：

- `sbus.serial_device`
- `sbus.channel_scale_x`
- `sbus.frame_bytes`
- `sbus.joystick_center`
- `sbus.valid_min`
- `sbus.valid_max`
- `timing.sbus_receive_period_ms`
- `timing.sbus_loss_timeout_ms`

ATOM-03 通过构造注入 `const marshland::config::IConfigView&`，并检查 `ConfigStatus`；配置模块从 `RuntimeContext::configuration_path` 加载文件，不能依赖 `RuntimeContext::values` 向其他模块提供类型安全配置。

## 2. 当前实现

### 2.1 公共实现头

`Inc/atom15_config/config_store.hpp` 提供：

- `ConfigStore`，实现完整的 `marshland::config::IConfigView`；
- `LifecycleAdapter`，实现 `marshland::runtime::ILifecycleModule`；
- `ConfigLoadResult`、`ConfigDiagnostic`、`ConfigError`；
- `ConfigValue` 和运行时更新结果 `PersistResult`；
- `PidConfig`、PID 读写结果；
- 预留变量读写接口；
- 配置变更回调和单调递增版本号。

### 2.2 JSON 与校验

`Src/config_store.cpp` 当前包含无第三方依赖的 JSON 解析、序列化和快照存储：

- JSON 对象、数组、字符串、整数、浮点数、布尔值和 `null`；
- 重复对象键、非法 JSON 和文件错误可诊断；
- 必需顶层结构检查：`schema_version`、`baseline`、`mode`、`timing`、`motor`、`can`、`sbus`、`persistence`；
- 电机映射及 PID 字段存在性和类型检查；
- ATOM-03 真实消费键的类型/基础范围检查；
- `null` 必需值拒绝为未实现配置；
- 查询只读内存快照，不在 `IConfigView` 查询方法中执行文件 I/O。

### 2.3 并发与热加载

- 采用 `shared_mutex` 保护快照；
- 读查询支持并发调用；
- 成功加载和运行时更新递增 `configVersion()`；
- 字符串指针在下一次配置版本变化前保持快照内存有效；
- 更新回调在版本变更后调用。

### 2.4 运行时更新与持久化

- 只有 `WEB` 来源且处于 `MANUAL` 或 `DEBUG` 模式才允许更新；
- 类型不匹配、关键配置越界、未知键、未加载配置均拒绝；
- 运行时更新分别返回 `effective` 与 `persisted`；
- 文件损坏不自动修复，不执行版本迁移、钳位或掉电保护；
- PID 支持按电机 ID 读取和整体写入；
- PID 字段包括 `kp`、`ki`、`kd`、`integral_limit`、`output_limit`、`control_period_ms`、`enabled`；
- 预留变量范围为 `reserved_001` 至 `reserved_100`，类型为 `int32`，支持缺失键首次写入。

## 3. CMake 集成

本目录仍不定义 `project()`、`main()` 或可执行目标。当前目标：

- `atom_15_config_persistence`；
- `Marshland::atom_15_config_persistence`；
- 有 `Src` 时生成 STATIC 库；
- 链接 `Marshland::PublicHeaders`；
- 编译标准、ARM 工具链、警告和全局选项由上层 CMake 管理。

## 4. 验证结果

已使用既有 ARM 交叉编译构建目录执行增量构建：

```text
wsl cmake --build cmake-build-ubuntu-arm \
  --target marshland marshland_gateway_tests marshland_public_contracts_check \
  --parallel 4
```

结果：

- `atom_15_config_persistence` 编译通过；
- `marshland` 链接通过；
- `marshland_gateway_tests` 链接通过；
- `marshland_public_contracts_check` 通过；
- 未执行 clean-first 或全量重编译。

## 5. 当前限制与后续风险

1. 当前仓库没有 `Config/dart_control.json` 样例，因此尚未进行真实文件加载、PID 重启恢复和损坏文件运行测试。
2. 当前实现是轻量 JSON 解析器，尚未完整执行 `Rule/ConfigSchema.schema.json` 的所有 `additionalProperties`、枚举、正则、数组和语义范围约束；当前已覆盖启动必需结构、已知 ATOM-03 消费键和核心 PID/持久化字段。生产前建议接入经过批准的 JSON Schema 校验库或继续补齐逐字段校验。
3. `effective` 更新与普通文件写入发生在同一更新操作中；文件写入失败时内存值仍然生效，并明确返回 `effective=true`、`persisted=false`。未实现自动重试，符合“不额外承诺”的设计边界。
4. `ChangeCallback` 是本模块提供的窄事件回调；当前 `Public` 与 `Design/LifecycleEvents.md` 没有已落地的配置变更事件公共端口，因此没有擅自修改 `Public` 或复用状态事件接口。
5. 其他 Atom 当前只有 ATOM-03 存在真实 C++ `IConfigView` 查询实现；其余模块的配置键消费仍主要停留在规范/计划文档层面。
6. 未新增上层装配对象图注册代码，因为上层 `Entry.cpp` 和 `AppCoordinator` 的最终 ATOM-15 实例注入位置不属于本目录，且修改上层文件不在本次工作范围。

## 6. 文件变更

- 修改：`PLAN.md`；
- 新增：`Inc/atom15_config/config_store.hpp`；
- 新增：`Src/config_store.cpp`；
- 修改：本目录 `CMakeLists.txt`，补充 `Marshland::PublicHeaders` 依赖。

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.4 | 2026-10-11 | 获得 Coding 授权后完成首版 ATOM-15 配置快照、IConfigView、生命周期适配器、JSON 读写、PID/预留变量持久化及 ARM 增量构建验证。 |
