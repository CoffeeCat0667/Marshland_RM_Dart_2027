# ATOM-05_Motor_PID 工作方案（2026-10-13 更新）

## 1. 文档基线与工作边界

### 已确认的最新设计基线

- ATOM-05 设计文档同步基线：`PRD 0.10`、`PEF 1.5`、`Interface 1.5`、`Config 0.8`，日期为 `2026-10-10`。
- ATOM-05 的产品职责是为 Config 映射的电机提供命令、反馈、编码器转速和 PID 闭环能力，但不决定自动发射流程步骤。
- 当前映射：
  - 蓄力电机 1（右）：CAN node `0x201`
  - 蓄力电机 2（左）：CAN node `0x202`
  - 换弹电机 3（右）：CAN node `0x203`
  - 换弹电机 4（左）：CAN node `0x204`
  - YAW 电机 5：CAN node `0x205`
  - 变力电机 6：CAN node `0x206`
- 换弹两侧使用速度 PID 和 CAN 反馈；不新增换弹位置控制、回零或限位判定。
- PID 修改只能来自 `WEB`，且系统模式只能是 `MANUAL` 或 `DEBUG`。
- PID 参数热加载和持久化必须分别报告结果；持久化失败不得报告为成功。
- 单电机反馈异常不得伪装成有效编码器读数。
- 换弹正常到位的零转速是有效速度 PID 目标，不等同于零力矩；手动停止、急停或联动故障时必须撤销速度目标并优先输出零力矩。

### 本目录的实际职责

最新工作规则明确规定：本工程只负责网关和各模块组装，禁止实现 `Design/Atomic` 中描述的任何 Atomic 功能。因此，本目录的计划仅覆盖：

- ATOM-05 模块目标的 CMake 组织与上层组装。
- 未来外部实现接入时所需的公共契约、依赖方向和生命周期注册边界。
- 对公共接口缺口的记录与上游协调项。

本目录不得新增电机控制器、CAN 解码器、PID 算法、命令仲裁、持久化实现或 `main` 入口。不能把 ATOM-05 设计文档中的功能直接落地为本目录源码。

## 2. 已阅读的公共代码与结论

### PublicHeaders 叶子契约

`Atoms/Public/Headers/CMakeLists.txt` 定义 `Marshland::PublicHeaders`，只包含纯公共类型和接口：

- `MarshlandTypes.hpp`：`SystemMode`、`ControlSource`、`FaultSeverity` 等共享枚举。
- `StateArbitrationTypes.hpp`：`ExternalInputs`、`ArbitrationContext`、控制意图、授权结果和状态事件。
- `ILifecycle.hpp`：`RuntimeContext`、`RuntimeStep`、`ILifecycleModule`、模块初始化结果。
- `IConfigView.hpp`：只读配置查询契约。
- `IMonotonicClock.hpp`：单调时钟接口。
- `AppInterfaces.hpp`：装配桥的窄端口接口。
- `SbusTypes.hpp`、`SbusInterfaces.hpp`：SBUS 公共契约。

ATOM-05 只能依赖这些公共契约，不能包含 `Atoms/Public/AppCoordinator/` 下的实现桥头文件。

### 配置读取边界

`IConfigView` 的约束已经足以确定装配方向：

- 查询接口全部为 `const`、`noexcept`，不能在调用期间执行 I/O。
- 配置加载状态必须通过 `ConfigStatus` 显式表达，不能以默认值掩盖 `NOT_LOADED`、`MISSING`、`INVALID_TYPE` 或 `OUT_OF_RANGE`。
- `configVersion()` 单调递增，消费者可据此发现热加载。
- 该接口没有写入方法；运行时 PID 更新和 JSON 持久化不应在本目录通过自定义写接口实现。

### 生命周期与装配边界

`ILifecycle.hpp` 已提供启动步骤 `LoadPidParameters` 和 `InitializeMotorAndEncoderChannels`。因此未来 ATOM-05 适配器若由上层接入，应作为生命周期模块注册，由 RuntimeCoordinator 按既定步骤调用，而不是自行创建线程入口或绕过 RuntimeCoordinator。

`AppCoordinator` 是唯一允许同时连接 RuntimeCoordinator 与 StateArbitrator 的装配桥。ATOM-05 不得依赖 `AppCoordinator.hpp`，也不应直接操作 StateArbitrator；需要状态信息时只能通过公共窄接口和上层注入完成。

`StateArbitrationTypes.hpp` 提供的相关状态信息包括：

- 系统模式：`MANUAL`、`DEBUG`、`AUTO_FIRE`、`SOFTWARE_ESTOP`、`FAULT` 等。
- 控制源：`SBUS`、`WEB`、`AUTO`，且公共类型定义了优先级关系。
- 安全状态：故障、软件急停、控制权和意图判定结果。
- 单调时间戳：所有超时和控制周期不得依赖墙上时钟。

这些类型只描述跨模块契约，不授权本目录实现 ATOM-05 的业务逻辑。

## 3. CMake 与依赖方案

当前目录的 `CMakeLists.txt` 已符合模块模板要求：

- 不定义 `project()`，由上层工程统一管理。
- 不产生可执行文件，不定义 `main()`。
- 不设置编译器、目标平台、C++ 标准或编译选项。
- 无 `Src/` 源码时使用 `INTERFACE` 目标；以后若有允许接入的模块适配源码，由模板自动切换为 `STATIC`。
- 目标名为 `atom_05_motor_pid`，并提供 `Marshland::atom_05_motor_pid` 别名。
- 公开包含目录包含本目录、`Inc/`（若存在）和同级 `Public`。
- 若 `Marshland::PublicHeaders` 已由上层先创建，可通过目标依赖接入；不得自行复制公共类型。

上层工程的组装顺序为：

1. `Atoms/Public/Headers`
2. `Atoms/`
3. `Atoms/Public`，其中包含 `AppCoordinator` 和 `marshland_public`

上层还强制 Ubuntu ARM 架构和 C++23，并禁止使用 x86 编译链。任何后续构建必须使用 WSL ARM 交叉工具链或目标 Ubuntu ARM 主机，并遵守增量编译要求。

## 4. 允许的后续装配工作

在获得明确授权且不触碰 Atomic 功能实现红线的前提下，只考虑以下工作：

1. 检查 ATOM-05 目标是否被 `Atoms/CMakeLists.txt` 纳入 `marshland_atoms` 聚合目标。
2. 为外部提供的、已经符合架构边界的 ATOM-05 适配目标补充 CMake 链接关系。
3. 确认适配器只依赖 `Marshland::PublicHeaders` 和明确注入的窄接口。
4. 确认生命周期注册信息能被 RuntimeCoordinator 查询，且启动步骤顺序与 `ILifecycle.hpp` 一致。
5. 使用 ARM 工具链进行增量构建和公共契约编译检查。
6. 仅在上层设计明确提供公共接口后，更新本计划中的接口依赖记录；不在本目录臆造 CAN、PID 或 JSON 写入协议。

## 5. 禁止事项

- 禁止在本目录实现 ATOM-05 设计文档中的任何电机、编码器、CAN、PID、故障处置或控制权限功能。
- 禁止新增 `main`、独立可执行文件或绕过上层 RuntimeCoordinator 的启动线程。
- 禁止包含或链接 `AppCoordinator.hpp` 作为普通 ATOM 的实现依赖。
- 禁止直接修改 `Atoms/Public`、上层 `CMakeLists.txt` 或 `Design` 文档以弥补本目录缺口。
- 禁止用本目录自定义类型替代 `Marshland::PublicHeaders` 中已有公共类型。
- 禁止使用 x86 编译链或进行全量重编译。

## 6. 当前未决项与需要上游提供的内容

公共代码目前没有提供 ATOM-05 专用的 CAN、MotorCommand、MotorFeedback、PIDConfig、故障上报或持久化写入接口。因此无法仅凭现有 `Public` 代码确定合法的 ATOM-05 接入 API。

需要上游设计或公共契约明确：

- CAN 发送/接收适配器的所有权、线程模型和反馈帧结构。
- `MotorCommand`、电机状态快照和 PID 参数的数据结构。
- 反馈超时阈值、PID 周期、输出限幅及“连续 5 周期饱和且误差未减小”的检测数据来源。
- PID 更新请求如何携带 `ControlSource`、`SystemMode` 和请求结果。
- 热加载成功与 JSON 持久化成功的独立报告通道。
- 单侧换弹故障联动另一侧零力矩，以及由 ATOM-10 取消动作/计时的窄接口。
- 故障管理器的公共上报接口。
- ATOM-05 生命周期模块的注册位置和依赖注入方式。

在这些契约出现之前，不应创建 `Inc/`、`Src/` 中的功能实现文件，也不应将设计文档中的原子操作转写成代码。

## 7. 验证计划

当前仅验证模块装配配置，不验证 ATOM-05 功能实现：

1. 检查本目录目标可被上层 `add_subdirectory` 处理，并能作为 `Marshland::atom_05_motor_pid` 参与聚合。
2. 使用 Ubuntu ARM 或 WSL ARM 交叉工具链进行增量 configure/build。
3. 在启用 `MARSHLAND_BUILD_TESTS` 时运行公共契约编译检查；测试执行使用 ARM 主机或 `qemu-aarch64`。
4. 确认本目录没有 `main`、没有独立可执行目标、没有 AppCoordinator 依赖。
5. 对任何未来适配接入，优先验证依赖方向、生命周期注册和失败闭环，而不是在本目录验证 Atomic 业务行为。

## 8. 本轮更新记录

- 更新日期：2026-10-13。
- 阅读并对齐：当前工作目录 `AGENTS.md`、ATOM-05 最新设计文档、上层 `CMakeLists.txt`、`Atoms/Public` 全部现有公共代码与其 CMake 文件。
- 主要修正：将原先“实现电机/PID 功能”的计划改为“仅做模块装配和公共契约接入”的计划，以符合最新 AGENTS 的工程职责限制。
- 未修改任何公共代码、上层工程文件或设计文档。
