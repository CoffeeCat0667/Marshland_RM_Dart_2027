# ATOM-02 State Arbitration 实现计划

## 0. 文档定位与权威来源

| 项目 | 内容 |
|---|---|
| 本计划对象 | `Atoms/ATOM-02_State_Arbitration` |
| 设计权威 | `Design/Atomic/ATOM-02_State_Arbitration.md` |
| 接口权威 | `Design/Interface.md` §3.2、§6、§7、§9、§18.2 |
| 配置权威 | `Design/Config.md` §7.2、§14 |
| 故障权威 | PEF 未定义任何故障码枚举；故障码族（`F-xxx-001` 等）与等级仅由 `Design/Config.md` §14.2 定义，故障管理接口由 `Design/Interface.md` §18.2 定义 |
| 复核结论 | 本计划 §3 的枚举已与 Interface §6 逐字对齐；凡与 Interface 不一致的旧表述一律以 Interface 为准 |
| 编码授权 | 已获授权完成首版实现与 BugLists 修复轮：类型上提 `Atoms/Public/StateArbitrationTypes.hpp`，`Inc/StateArbitration.hpp` 只保留策略与 `StateArbitrator`；编译验证按 §6.3 由用户执行 |
| 缺陷清单 | `BugLists.md`：**ATOM02-BUG-001～013 全部已修**（002 按用户裁决的**方案 B**：授权函数只做前置条件，优先级统一在意图路径判定；010 按 `IDLE` 幂等语义）；014 已关闭。逐条状态见其 §1 与 §4 |

## 1. 目标与边界

实现系统状态机、自动流程状态、当前控制源选择结果以及实际发射授权仲裁。模块只负责状态与授权决策。

明确**不做**的事：

1. 不实现 `FireAction` 动作本身。Interface §6.5 的 `FireAction` 是发射动作数据模型（`servo_id / standby_angle / fire_angle / hold_time / state`），其持有与执行归发射执行方；本模块只在进入 `FIRING` 之前提供授权判定。
2. 不发送 CAN/IIC 报文，不驱动舵机/电机，不访问任何硬件或 OS 设备。
3. 不实现故障管理。故障表、故障码、历史故障、故障时间、故障来源、自动恢复状态、人工确认状态、清除结果与 WebSocket 推送均属故障管理模块（Interface §18.2）；本模块只**消费**「是否故障 + 故障级别」。
4. 不存在 `main` 入口函数。

## 2. 目录与构建方案

- **共享类型**（跨 ATOM 共用的 `SystemMode` / `ControlSource` / `AutoFireState`）统一定义在 `Atoms/Public/`（该目录当前存在但为空）。由本模块首次落地，其他 ATOM 直接包含复用；禁止各 ATOM 重复定义同名枚举，否则同一编译单元会重定义报错。
- 本模块自有头文件放 `Inc/`，实现放 `Src/`。
- 模块目标 `atom_02_state_arbitration`，别名 `Marshland::atom_02_state_arbitration`。
- 本目录 `CMakeLists.txt` 已是「有 `Src/*` 源码 → STATIC，否则 INTERFACE」的自动模板，并已把本目录、`Inc/`、`../Public` 加入包含路径。本次实现**无需修改 CMake**；`GLOB_RECURSE CONFIGURE_DEPENDS` 会在下一次 configure 自动纳入新文件。
- 编译标准、目标平台与编译选项全部由上层 `NewVersion/CMakeLists.txt` 决定（C++23、仅 aarch64/arm，非 ARM 架构 configure 直接 FATAL_ERROR）。

## 3. 核心模型

### 3.1 共享类型（定义于 `Atoms/Public/`，逐字取自 Interface）

```text
// Interface §6.2 系统模式（11 项，顺序一致）
enum SystemMode {
    UNINITIALIZED, INITIALIZING, INIT_FAILED,
    STANDBY, MANUAL, AUTO_FIRE, DEBUG, CALIBRATION,
    RECOVERY, SOFTWARE_ESTOP, FAULT
}

// Interface §6.1 控制源（4 项）
enum ControlSource { NONE, SBUS, WEB, AUTO }

// Interface §6.3 自动流程状态（15 项，顺序一致）
enum AutoFireState {
    IDLE, AUTO_PREPARE, CHARGE, FORCE,
    SHOT_1_YAW, RELOAD_LIFT, FEED, FEED_WAIT, RELOAD_LOWER, SHOT_N_YAW,
    FIRE_READY, FIRING, FIRE_COMPLETED, AUTO_COMPLETED, AUTO_TERMINATED
}
```

说明：

- 不存在名为 `AUTO` 或 `WEB` 的**系统模式**；WEB 是控制源，自动流程由 `AUTO_FIRE` 模式承载。
- 故障级别取 Config §14.2 的四档（信息、警告、严重、急停）；故障码为 Config §14.2 的字符串族（`F-xxx-001` 等）。若上层故障模块已提供 `ErrorCode` 类型，本模块只包含该类型，不重复定义。
- `FireAction.state`（`IDLE | MOVING_TO_FIRE | HOLDING | RETURNING | COMPLETED | FAILED`，Interface §6.5）属发射执行方，不在本模块定义。

### 3.2 本模块类型（定义于 `Atoms/Public/StateArbitrationTypes.hpp`，ATOM02-BUG-013）

- **ArbitrationContext**：WEB 在线、SBUS 在线、SBUS 自动模式标志、软件急停、停止发射标志、故障状态与级别、当前系统模式、当前自动流程状态、单调时钟时间戳与序号。
- **ControlIntent**：控制源标识（`SBUS`/`WEB`/`AUTO`）、意图类别、请求标识（Interface §9.2 要求 WEB 请求必须携带请求标识与控制源标识）。
- **StateTransitionResult**：是否接受、原状态、目标状态、拒绝原因。非法切换不改变状态。
- **StateEvent**：原状态、新状态、原因、时间戳（Interface §7.2 第 6 条要求每次迁移可查询且有原因）。
- **ControlSourceChange**：原控制源、新控制源、抢占原因（ATOM-02 设计文档原子操作 13）。
- **FireAuthorization**：是否允许、按 `SBUS > WEB > AUTO` 得出的授权源、阻断原因（急停 / 停止发射 / 故障 / 未授权 / YAW 未到位）。
- **AutoFireTermination**：SBUS 接管后「允许当前硬件动作组完成，然后终止原自动流程」，不提供暂停/恢复入口，`AUTO_TERMINATED` 之后不得续跑。

## 4. 行为实现顺序

1. 提供当前系统模式、自动流程状态和当前控制源的查询接口。
2. 对模式切换执行合法性校验，转换表依据 Interface §7.1；非法切换不改变目标状态，并返回明确原因。
3. 对每次有效状态转换生成含原状态、新状态、原因的状态事件。
4. 按 Config §7.2 计算当前控制源：无 WEB 连接时初始为 `AUTO`；WEB 在线且无更高优先级条件时选 `WEB`；SBUS 在线且自动模式标志为 True 时结果为 `AUTO`；SBUS 在线且标志为 False 时按 `SBUS > WEB` 选 `SBUS`；SBUS 失联降级至 `WEB`，恢复有效帧后重新应用自动模式标志与 SBUS 优先规则。
5. 对普通 SBUS/WEB 控制意图执行仲裁；低优先级冲突命令被拒绝且不得修改控制目标。
6. 在实际发射前复核：发射授权严格按 `SBUS > WEB > 自动流程`，并重新检查停止发射标志、故障状态与软件急停（Interface §7.2 第 3 条、§3.2）。
7. SBUS 接管自动流程时置 `AUTO_TERMINATED`，允许当前硬件动作组完成后终止原流程；不暂停、不恢复（Interface §7.1、§7.2 第 4 条）。
8. 软件急停为最高安全覆盖，不作为普通控制源参与排序，不可由普通模式切换解除；将控制权交给调试/校准时切换到 `DEBUG`/`CALIBRATION` 并记录原因。
9. 记录每次控制源变化及抢占原因，供上层日志/事件适配器接入。

## 5. 安全与接口约束

- 控制源选择结果不得改变实际发射授权优先级；控制源为 `AUTO` 时 SBUS/WEB 仍可按规则介入，介入命令必须经过仲裁。
- 停止发射标志仅在发射前待命被扫描，仅当为 `False` 时允许实际发射（Interface §7.2 第 8 条）。
- 自动模式标志为 True 时控制源可显示为 `AUTO`，但介入命令仍须仲裁。
- 状态机只输出状态、授权和事件结果，不直接访问硬件驱动。
- 所有对外接口尽量保持无副作用；提交状态变化与执行授权时使用明确的结果类型，避免调用方误把拒绝当成成功。
- 本模块不生成、不持有、不执行 `FireAction`；调用点位于自动流程 `FIRE_READY → FIRING` 之前（Interface §7.1、§8.1、§8.2），由发射执行方持 `FireAction` 先取得本模块授权后再执行。

## 6. 验证计划

### 6.1 静态自查（本模块负责，不编译、不产生构建产物）

1. 条款映射表：每个公开枚举/函数逐条标注来源（Interface §6.1/6.2/6.3/7.1/7.2/9、Config §7.2/§14）。
2. 逐字比对：`SystemMode`/`ControlSource`/`AutoFireState` 的名称与顺序必须与 Interface §6 完全一致，禁止自造 `AUTO`/`WEB` 系统模式。
3. 合规自查：无 `main`、头文件在 `Inc/`、源码在 `Src/`、共享类型在 `Public/`、无硬件/OS 依赖、无第三方依赖、不修改 `CMakeLists.txt`。
4. 判定表：为下列场景写出「输入 → 期望输出」，供上层执行行为验证。

### 6.2 覆盖场景（判定表内容）

- 初始控制源、WEB 上线/下线、SBUS 自动/手动、SBUS 失联与恢复（Config §7.2 五行规则逐行覆盖）。
- 状态切换合法性、非法切换不修改状态、事件内容完整性（Interface §7.1 转换总表）。
- `SBUS > WEB > AUTO` 冲突仲裁与低优先级拒绝行为。
- 急停、停止发射、故障、未授权时禁止发射授权（Interface §7.2 第 3、5、8 条）。
- SBUS 接管后自动流程终止、`AUTO_TERMINATED` 不可续跑、不提供暂停/恢复。
- 停止发射标志为 `True` 时保持待发、不发射、不开始下一发（Interface §8.3、§7.2 第 8 条）。

### 6.3 编译验证（由用户执行，本模块不执行编译命令）

```bash
# A. WSL aarch64 交叉编译（首次 configure 后，日常只跑 --build）
cmake -S NewVersion -B build-arm -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_TOOLCHAIN_FILE=<aarch64.cmake>
cmake --build build-arm --target marshland -j

# B. 目标 Ubuntu ARM 主机（源码上传后）
cmake --build <已有build目录> --target marshland -j
```

必须为增量编译：禁止 `--clean-first`，禁止删除已有构建目录。上层 `-Wall -Wextra -Wpedantic` 的警告视为需清零，错误与警告原文回传后修正。

## 7. 文件与实现形态

采用**声明/实现分离**（非 header-only），理由：状态机转换表、控制源选择、仲裁与授权逻辑体量较大，放 `.cpp` 只实例化一次，避免 header-only 让每个包含者复制并造成改一处全量重编；本目录 CMake 已自动切 STATIC，无需改动。

- `../Public/MarshlandTypes.hpp`：Interface §6 的三个共享枚举，外加 Config §14.2 的四档故障等级 `FaultSeverity`（已落地）。
- `../Public/StateArbitrationTypes.hpp`：外部输入、意图、结果、事件类型（含 `ShotStage`，供 ATOM-03/11/12/13/14/16/17/18 复用）。
- `Inc/StateArbitration.hpp`：纯函数策略（控制源选择、转换合法性、故障等级归约、发射授权、意图仲裁）与 `StateArbitrator` 类接口。
- `Src/StateArbitration.cpp`：状态转换、控制源选择、授权仲裁及事件生成实现。

## 8. 待确认项与已知风险

1. ~~共享头文件名未定~~ 已定：`Atoms/Public/MarshlandTypes.hpp`；用户已确认其他 ATOM 未定义 `SystemMode` 等同名枚举。若后续某模块引入自己的定义，须改为包含本头文件而非重复定义。
2. 软件急停、停止发射标志、故障状态的**写入方**（SBUS 解析器、WEB、视觉或故障模块）与锁的类型（Config §15 要求软件急停为统一 `bool`）需在上层接线时敲定。
3. ~~故障输入表示~~ 已定：`bool fault_active` + `FaultSeverity`（Config §14.2 四档）。故障模块若引入 `ErrorCode` 字符串族，作为附加诊断字段接入，不改变授权判定。
4. 本模块不独立编译，也不建立测试入口 `main`；行为验证依赖 §6.2 判定表与上层集成验证。
