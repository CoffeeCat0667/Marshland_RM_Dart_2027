# ATOM-15 Config Persistence 实现方案

## 1. 目标与边界

ATOM-15（Marshland）负责读取唯一配置源 `./Config/dart_control.json`，完成配置解析与按 Config 规则的字段/范围校验，为运行模块提供校验后的参数；同时支持运行时配置更新、PID 参数持久化和最多 100 个额外持久化变量的读写。

本 Atom 不提供 `main` 入口，不负责启动编排、执行器生命周期、硬件控制策略或 Config 规则本身的重新定义。PID 数值、边界和持久化策略均从 Config 读取。

## 2. 拟定模块结构

```text
ATOM-15_Config_Persistence/
├─ CMakeLists.txt
├─ PLAN.md
├─ Inc/
│  └─ atom15/config_persistence.hpp      # 对外接口、数据结构、错误/状态定义
└─ Src/
   ├─ config_persistence.cpp             # 配置加载、校验、查询和更新
   ├─ json_config_store.cpp              # JSON 文件读写与损坏/缺失处理
   └─ pid_persistence.cpp                # 电机 PID 与额外持久化变量
```

实现阶段若确认依赖库已经由 Public 或上层工程提供，将通过上层目标传递依赖；不在本目录自行引入第三方库或修改上层 CMake。

## 3. 核心接口草案

- `load(path)`：读取并解析 JSON，区分文件缺失、JSON 损坏、必需字段缺失、字段类型错误和范围错误。
- `get(key)`：查询当前已校验配置键值。
- `update_runtime(key, value, source, mode)`：仅接受 `source == WEB` 且 `mode` 为手动或调试；更新通过校验后立即作用于运行时配置。
- `persist()`：将已接受的运行时更新写回 JSON，并独立返回 `effective` 与 `persisted` 状态。
- `read_pid(motor_id)` / `write_pid(motor_id, pid)`：读取和保存单个电机 PID，保存后支持重启恢复。
- `read_extra(key)` / `write_extra(key, value)`：管理额外持久化变量，限制总数不超过 100。
- 配置热加载/参数变更事件：在更新成功且达到约定发布条件后发布事件，具体事件适配由调用方接入。

## 4. 关键行为与错误策略

1. 启动时先读取唯一配置源；必需的硬件/安全参数缺失或配置无法按规则校验时返回初始化失败条件。
2. 缺失的可选参数使用 Config 定义的内置默认值；单字段错误必须能够独立报告。
3. 损坏文件只报告错误，不自动修复、不自动覆盖、不执行掉电保护或额外迁移。
4. PID 更新拒绝非 WEB 来源，以及非手动/调试模式；拒绝越界值，不改变活动 PID。
5. 运行时生效与文件持久化分别报告，不能在文件写入失败时谎报全部成功。
6. 持久化采用 Config 规定的普通文件写入语义，不额外承诺原子性或版本迁移。
7. 参数更新和 PID 保存使用明确的结果对象，便于上层分别处理 effective/persisted 状态和诊断信息。

## 5. CMake 集成方案

- 本目录提供 `atom15_config_persistence` 模块目标，作为上层 `add_subdirectory(...)` 后可链接的模块化目标。
- 当前阶段使用接口库承载公共头文件、C++23 要求和 include 路径；实现文件加入后改为实际编译目标，保持目标名和公共接口不变。
- 不在本目录设置架构、编译器或全局编译选项；Ubuntu ARM、C++23 和警告策略继续由上层 `CMakeLists.txt` 决定。
- 不创建可执行文件，不定义 `main`。

## 6. 实现顺序

1. 固化公共数据结构、状态码、诊断信息和权限校验接口。
2. 实现 JSON 读取、类型/范围校验、默认值和必需项检查。
3. 实现运行时查询与更新，并保证更新成功立即反映到有效配置。
4. 实现 JSON 持久化及 `effective`/`persisted` 双状态结果。
5. 实现电机 PID 和额外变量持久化上限检查。
6. 接入热加载/参数变更事件回调。
7. 在允许的 ARM 工具链环境中编译并补充针对缺失、损坏、权限拒绝、越界和重启恢复的测试。

## 7. 待确认事项

- Public 目录中实际可复用的 JSON、Config、事件和 PID 类型/接口名称。
- `dart_control.json` 的精确 schema、必需键、默认值和范围。
- 电机 ID 的合法范围及 PIDConfig 的完整字段。
- 热加载事件的现有发布接口与线程安全要求。
- 上层工程接入本目录时采用的 `add_subdirectory` 路径。
