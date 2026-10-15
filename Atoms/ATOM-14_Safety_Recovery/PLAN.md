# ATOM-14_Safety_Recovery 工作计划

- **更新日期**：2026-10-15
- **设计同步基线**：PRD 0.10、PEF 1.5、Interface 1.5、Config 0.8
- **权威设计文档**：`Design/Atomic/ATOM-14_Safety_Recovery.md`（只读例外）
- **编码指南**：本目录 `Guide.md`（0.3）
- **唯一规则来源**：本目录 `AGENTS.md`

## 1. 规则定位与职责修正（重要）

本计划以本目录 `AGENTS.md` 为唯一规则来源。其规定：

- 允许读取/写入 `Atoms/Public`；只读 `Design/Atomic/ATOM-14_Safety_Recovery.md` 与上层 `CMakeLists.txt`；其余上层文件禁止读取或修改。
- 本目录是 ATOM-14 原子模块的工作目录：头文件放 `Inc/`，源码放 `Src/`，禁止 `main()`；本目录 `CMakeLists.txt` 构成可被上层 `add_subdirectory` 调用的模块工程。
- 任何文件修改必须获得用户明确许可；信息不足时先询问，不得擅自改动。

**对旧版 PLAN 的修正**：旧版 PLAN（2026-10-10）引用上层根 `AGENTS.md` 的「禁止实现 `Design/Atomic` 功能」条款，将本目录定位为只做装配。该条款约束的是**根工程组装 Agent**（其职责是网关与模块组装），并不约束本原子工作目录。本目录的职责就是**实现 ATOM-14 原子功能本身**：故障登记/查询/更新/清除与事件发布、普通命令安全闸门、一般故障自动恢复编排、软件急停锁定与人工解除编排、零力矩安全帧的持续发送调度。本次 PLAN 据此重写。

仍然成立的边界（来自设计文档与 Guide，属于 ATOM-14 自身职责边界，而非外部禁令）：

- 不实现 PID、CAN 报文构造/解析、SocketCAN、I2C、PCA9685A、电机/舵机直接驱动。
- 不判定 `SystemMode` 权威迁移与发射授权（ATOM-02），不推进四发自动流程（ATOM-13），不定义硬件就绪门控与启动结果映射（AppCoordinator）。
- 不包含 AppCoordinator/StateArbitrator/RuntimeCoordinator 具体头，不持有其具体引用；一切交互经由 `Atoms/Public` 的窄端口/类型。

## 2. 上游现状（2026-10-10 根 PLAN 同步）

- 公共端口已落地：`Atoms/Public/AppInterfaces.hpp` 提供 `IInputReporter`、`IControlAuthority`、`ISafetyStateControl`、`IStateReader`，由 AppCoordinator 实现；`ISafetyStateControl` 只注入 ATOM-14。
- `Atoms/Public/IMonotonicClock.hpp` 提供独立单调时钟接口；等待/超时/周期一律使用它，不用墙上时钟。
- 网关已有统一状态锁（唯一同步入口）：上报、仲裁、确认/解除、闸门与读取在同一同步域。ATOM-14 不跨线程保存仲裁器内部引用，不在锁内做 I/O、日志、sleep 或长等待。
- 可用公共类型/端口头：`StateArbitrationTypes.hpp`（`ControlIntent`、`IntentKind::SAFETY` / `SAFETY_RECOVERY`、拒绝原因等）、`CanInterfaces.hpp` / `CanTypes.hpp`（零力矩帧输出委托）、`ForceInterfaces.hpp` / `ForceTypes.hpp`、`IConfigView.hpp`（配置读取）、`ILifecycle.hpp`、`MarshlandTypes.hpp`。
- 机构动作、配置和生命周期事件等端口在根 PLAN 中标注为「仍未实施」——凡本模块需要而 Public 尚无对应窄端口的，列入待确认清单（见 §6），不得退回注入具体实现。
- ATOM02-BUG-018/019/020（WEB 清除授权、故障闸门等）由 ATOM-02 负责人处理；本模块只消费确认后的端口语义，不复制仲裁算法或故障等级表。

## 3. 必须遵守的业务契约摘要（实现时以设计文档与 Guide 为准）

### 3.1 软件急停两步契约

1. `ConfirmSoftwareEstop()` 只做 `SOFTWARE_ESTOP -> MANUAL` 与人工确认，急停变量与锁存保持 `true`。
2. 确认后以 `IntentKind::SAFETY_RECOVERY` 执行解除动作：双 M3508 换弹抬起（按 Config 定时运行 3 s 后两台 0 rpm 目标均成功才到位，继续速度 PID 保持）与蓄力双电机回零（两限位均稳定低电平）。
3. 全部恢复成功后调用 `ReleaseSoftwareEstopLatch()`，返回 `true` 才算解除；不重新初始化、不续跑原自动流程。
4. 禁止用 `SetSoftwareEstopInput(false)` 解除；再次急停立即撤销恢复目标并全部零力矩；恢复抬起再次失败进入 FAULT 锁定，保留锁存，不自动重试。

### 3.2 分阶段 CAN 安全输出（组帧/发送委托 ATOM-04）

| 阶段 | 输出 |
|---|---|
| 锁定等待确认 | `0x200` / `0x1FF`、DLC 8、全零、大端、周期 20 ms（Config），两组均发，覆盖全部受控电机；换弹不抬起 |
| 确认后的安全恢复 | 获准电机按恢复目标输出，同组未获准电机零电流；禁止全零帧覆盖恢复出力 |
| 再次急停 / 恢复失败 | 立即撤销恢复目标，全部零力矩 |
| 发送失败 | 按 Config 进入错误状态并全机静默，不静默忽略 |

### 3.3 一般故障恢复固定顺序（不得调换）

蓄力电机 1、2 回零（双限位均低）→ 仅发射机构三次单次发射（待机→发射→保持 100 ms→待机，期间其他电机零力矩、其他舵机不动）→ 双 M3508 换弹抬起（两侧目标均成功后按 Config 运行 3 s，两台 0 rpm 均成功且无取消/失败）→ 进入 `MANUAL`。根因清除且关联接口有效才触发；`W-VISION-002` 不触发；急停锁定期间不推进。

### 3.4 故障管理

故障码与等级全部取自 Config §14.2，不自造；历史 1000 条环形、最新优先；事件记录 UTC 展示时间戳、内部计时用单调时钟（保留两种基准）；等级归约复用仲裁端口判定，不自建等级表；不新增故障清除 HTTP 路由；诊断不得成为修改控制命令的通道。

### 3.5 意图分类

零力矩用 `IntentKind::SAFETY`（锁定阶段允许）；一切出力型安全动作用 `IntentKind::SAFETY_RECOVERY`（锁定阶段会被拒，仅在确认后的恢复阶段提交）；不得用 `IntentKind::CONTROL` 代替。

## 4. 实施计划（分阶段，均需先获用户明确授权）

1. **阶段 0 — 端口核对**：逐项核对 `Atoms/Public` 现有端口是否覆盖 §6 所需窄端口（安全状态控制、机构动作请求、限位/反馈读取、配置读取、CAN 安全帧输出、事件发布）。缺口先向用户报告并请求确认，不臆造接口。
2. **阶段 1 — 骨架**：创建 `Inc/`、`Src/`，定义模块类与注入端口（构造注入、无具体头依赖），CMake 目标 `Marshland::atom_14_safety_recovery` 接入上层模板并链接 `Marshland::PublicHeaders`。
3. **阶段 2 — 故障管理**：登记/查询/更新/清除、1000 条环形历史、事件发布、UTC+单调时钟双基准。
4. **阶段 3 — 安全闸门与急停锁定**：普通命令拒绝、锁定期两组全零帧周期调度（委托 CAN 端口，单调时钟绝对期限）。
5. **阶段 4 — 确认后安全恢复编排**：换弹抬起 + 蓄力回零、阶段输出切换、解除判定与 `ReleaseSoftwareEstopLatch()`。
6. **阶段 5 — 一般故障恢复编排**：固定顺序状态机、联动故障处理、FAULT 锁定不重试。
7. **阶段 6 — 集成校验**：按 Guide §10 自检清单逐项核对，WSL ARM 增量编译 `-Wall -Wextra -Wpedantic` 零警告。

## 5. 构建约束

- 只使用 WSL aarch64 交叉编译或目标 ARM 主机编译；禁止 x86 编译链；只用增量编译。
- 编译器、C++ 标准（C++23）、编译选项由上层 CMake 决定；本目录不设置全局选项。
- 本目录现有 `CMakeLists.txt` 已按统一模板组织 `atom_14_safety_recovery`；补充对 `Marshland::PublicHeaders` 的 `if(TARGET ...)` 链接（与上层模板约定一致），其他不改。

## 6. 端口核对结论与遗留待确认项

### 6.1 已确认可用（Public 现有契约）

- `ISafetyStateControl`（Confirm/ReleaseSoftwareEstopLatch，仅注入本模块）、`IControlAuthority::SubmitIntent`、`IStateReader::ReadState`、`IMonotonicClock`、`IConfigView`：急停两步、意图提交、状态读取、计时、配置均已覆盖。
- `CanTypes.hpp` 已编译期固化槽位映射：`reload_right = 0x203`、`reload_left = 0x204`（`dart_control.json` motor id 3/4 与 static_assert 一致，换弹映射待定项关闭）。
- `ICanSafetyNotifier` / `ICanEventSink` / `ICanNodeStatusReader`：CAN 侧事件与节点状态入口已存在，装配层可绑定到本模块。

### 6.2 已由本目录窄接口覆盖（2026-10-15 用户裁决）

`Inc/SafetyRecoveryPorts.hpp` 声明：`ISafetyTorqueGate`（对应 Public 注释引用但未落地的 ICanTorqueGate）、`IRecoveryActions`（蓄力回零/恢复发射/换弹抬起/撤销）、`ILimitSwitchReader`、`IMotorFeedbackReader`、`IFaultEventSink`。装配层后续把 ATOM-04/05/10/11 与诊断/WEB 适配到这些端口。

### 6.3 遗留待确认（已全部裁决，2026-10-15）

- **故障码目录来源（已裁决并落地）**：用户授权检索 Design 目录，故障表确认位于 `Design/Config.md` §14.2，共 23 个故障码；已落地为 `Inc/SafetyRecoveryFaultCatalog.hpp` 常量目录（含 `pef_recovery` 列，区分是否触发 PEF 一般恢复）。`dart_control.json` 无故障段，运行时不从配置文件读取故障码。表中 `F-YAW-001` 残缺行已于 2026-10-15 经用户授权修复：定为**警告级、不自动恢复、不需人工确认**（依据 PEF.md「不终止当前发次」与严重级会全局阻断的矛盾排除）。
- **UTC 展示时间来源（已裁决并落地）**：本模块自行取系统时钟生成 UTC 展示文本（`formatUtcNow`，仅展示用）；内部等待/超时/周期仍一律使用注入的单调时钟端口，两种基准同时保留。

## 7. 交付前检查清单

- [ ] 无 `main()`；头文件在 `Inc/`、源码在 `Src/`。
- [ ] 未包含 AppCoordinator/StateArbitrator/RuntimeCoordinator 具体头，未持有其具体引用。
- [ ] 未构造/解析 CAN 报文，未直接访问 SocketCAN/I2C/PCA9685A/电机寄存器/舵机通道。
- [ ] 急停确认与解除两步分离；未用 `SetSoftwareEstopInput(false)` 解除；解除后不重新初始化、不续跑旧流程。
- [ ] 锁定阶段持续两组全零帧；确认恢复阶段无全零帧覆盖恢复目标；再次急停/失败立即全部零力矩。
- [ ] 安全意图分类正确（SAFETY / SAFETY_RECOVERY），未用 CONTROL 代替。
- [ ] 一般恢复顺序正确；双限位均低判据；急停锁定期间不推进恢复；FAULT 锁定不自动重试。
- [ ] 故障码/等级取自 Config，历史 1000 条环形，UTC 与单调时钟双基准，未自建等级表。
- [ ] 未修改任何上层文件、设计文档或其他目录规则文件；每项修改均有用户明确授权。
- [ ] ARM 增量编译在 `-Wall -Wextra -Wpedantic` 下零警告。

## 8. 当前状态

- 目录现有：`CMakeLists.txt`（模块模板，已链接 `Marshland::PublicHeaders`）、`Guide.md`（0.3）、`PLAN.md`（本文件）、`Inc/`（SafetyRecoveryTypes.hpp、SafetyRecoveryPorts.hpp、SafetyRecoveryFaultCatalog.hpp、SafetyRecovery.hpp）、`Src/`（SafetyRecovery.cpp）。
- 2026-10-15 上午：按例外授权读取上层根 `PLAN.md` 并重写本计划，修正职责定位（本目录负责实现 ATOM-14），同步公共端口落地现状。
- 2026-10-15 阶段 0+1（已获授权）：完成 Public 端口核对（结论见 §6）；换弹映射经 `Config/dart_control.json` 确认为 0x203/0x204；创建 Inc/Src 骨架与本目录窄端口声明；CMake 补链 `Marshland::PublicHeaders`。
- 2026-10-15 阶段 2~5（已获授权）：修复 `Design/Config.md` §14.2 `F-YAW-001` 残缺行（警告级/不自动恢复/不需人工确认）；落地故障码常量目录（23 项）；实现故障管理（活动表 + 1000 条环形历史最新优先 + UTC/单调双时间基准 + 事件发布 + 等级归约上报）、安全闸门（`isOrdinaryCommandAllowed`）、急停锁定/确认/解除编排（两步分离、再次急停回锁定、F-RELOAD-002 FAULT 锁定不重试）、一般故障恢复状态机（回零→三次发射→换弹抬起→MANUAL）、分阶段力矩门控调度与 CAN 事件接入。
- 验证：WSL `aarch64-linux-gnu-g++ -std=c++23 -Wall -Wextra -Wpedantic -fsyntax-only` 通过，零警告。尚未做全工程增量构建与链接（窄端口需装配层绑定实现后方可链接入整机目标）。
- 遗留：`CanEvent` 的 NODE_STATE_CHANGE 不携带目标状态，当前按离线/异常登记，在线恢复由 `IMotorFeedbackReader` 查询确认；如需精确区分需上层扩展事件类型。
- 2026-10-15：应用户要求编写 `Rule/ATOM14ConnectProject.md`（装配层接线指南：端口注入、入向/出向接线、窄端口适配器语义、并发约束、启动顺序、验收清单）。
- 下一步：装配层按 `Rule/ATOM14ConnectProject.md` 绑定窄端口（`ISafetyTorqueGate`/`IRecoveryActions`/`ILimitSwitchReader`/`IMotorFeedbackReader`/`IFaultEventSink`）后进行整机增量构建与阶段 6 集成校验。
