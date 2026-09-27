# Marshland — RoboMaster 2027 飞镖系统镖架控制主程序

> 程序代号 **Marshland**　·　目标平台 **Ubuntu 22.04 ARM64**　·　语言标准 **C++23**（设计基线）
> 设计基线：**PRD 0.9 / PEF 1.4 / Interface 1.4 / Config 0.7**（2026-09-26）

---

## 1. 项目简介

本项目是 RoboMaster 2027 飞镖系统的**镖架控制主程序**，负责镖架各机构的统一控制、动作协调、状态管理、通信交互与故障处理，并直接控制相关电机、舵机与输入信号。

程序实现**固定四发自动发射流程**，并支持手动（SBUS / WEB）、调试、校准、一般故障自动恢复与软件急停等运行模式。

设计过程中遵循一条硬规则：**具体数值（通道、角度、CAN ID、PID、阈值等）只能来自 `Design/Config.md`，代码中不得隐含默认值，也不得臆造 `[待定]` 项。**

---

## 2. 系统组成

### 2.1 硬件与接口

| 接口 | 设备 / 用途 |
|---|---|
| SocketCAN（CAN0，1 Mbps） | 蓄力电机 ×2、YAW 电机、变力电机的命令与反馈、编码器数据、心跳、错误状态、零力矩帧 |
| IIC → PCA9685A | 16 通道 12 位 PWM 舵机驱动（MG996R 180°）：换弹 4 个、供弹 1 个、发射 1 个（x 号） |
| GPIO | 限位开关等高低电平输入（蓄力机构 2 个限位开关用于回零判据） |
| UART（SBUS） | 天地飞 ET16S 遥控接收机，16 通道，100000 8E2，已硬件反相 |
| TCP | 视觉系统：接收 YAW 目标控制量（rpm）与 YAW 到位 `bool` |

压力传感器**本版本不纳入**，不参与流程与安全判定。

### 2.2 软件组成

- **主控制程序**（本仓库，C++）
- **Web 前端**：静态资源由主程序直接提供，通过 HTTP 下发命令、通过 WebSocket 接收只读状态推送

---

## 3. 架构设计

### 3.1 分层结构

自上而下共 6 层：

| 层 | 内容 |
|---|---|
| 外部控制与状态来源 | Web/HTTP、WebSocket、ET16S/SBUS、视觉系统、CAN、GPIO、IIC |
| 输入适配层 | HTTP 适配、WebSocket 适配、SBUS 适配、视觉适配、CAN 适配、GPIO/IIC 适配 |
| 控制与安全层 | 控制权仲裁、系统状态机、软件急停、故障管理、状态聚合、日志诊断 |
| 流程协调层 | PEF 自动发射流程、手动控制流程、调试流程、校准流程、自动恢复流程 |
| 机构控制层 | 蓄力、换弹、供弹、变力、发射、YAW、电机闭环、舵机动作组 |
| 硬件驱动层 | SocketCAN、电机/PID、编码器、GPIO、PCA9685A/IIC、持久化 |

### 3.2 核心模块

`Bootstrap`、`ControlArbiter`、`StateMachine`、`PEFExecutor`、`ManualController`、`SafetyManager`、`ChargeController`、`ReloadController`、`FeedController`、`ForceController`、`FireController`、`YawController`、`MotorController`、`ServoController`、`CANAdapter`、`PCA9685AAdapter`、`GPIOAdapter`、`SBUSAdapter`、`VisionAdapter`、`WebAdapter`、`Persistence`、`Diagnostics`、`DebugController`、`CalibrationController`。

机构控制器统一抽象为 `initialize / enable / disable / stop / getState / getLastError / resetFault`，**不得绕过控制权仲裁、状态机与安全管理**。

### 3.3 原子功能类（ATOM-01 ~ ATOM-19）

`Design/Atomic/` 把全部功能拆成 19 个可独立评审的功能类，`Atoms/` 目录与之**一一对应**：

| Atom | 功能类 | 职责 |
|---|---|---|
| ATOM-01 | 进程生命周期与启动协调 | 按安全顺序建模块、汇总 readiness，决定 `STANDBY` / `INIT_FAILED` |
| ATOM-02 | 系统状态机与控制权仲裁 | 维护系统/自动流程状态、控制源选择与实际发射授权 |
| ATOM-03 | SBUS 遥控输入 | ET16S 帧接收解析、自动模式标志、失联降级与重连 |
| ATOM-04 | CAN/SocketCAN 传输适配 | 初始化、单帧收发、协议分派、节点与总线错误 |
| ATOM-05 | 电机、编码器与 PID | 电机命令/反馈、编码器、闭环 PID、WEB 授权热加载 |
| ATOM-06 | GPIO 与限位输入 | GPIO 采样去抖、蓄力双电机回零判定 |
| ATOM-07 | PCA9685A/IIC 舵机底层 | 舵机目标 → 配置通道 PWM，处理 IIC 错误 |
| ATOM-08 | 蓄力机构控制 | 两个蓄力电机：每发蓄力、停止、双限位回零 |
| ATOM-09 | 变力机构控制 | 变力目标 → 电机闭环，按配置时间判定到位 |
| ATOM-10 | 换弹与供弹控制 | 四舵机抬起/放下动作组、单舵机供弹动作 |
| ATOM-11 | 发射机构控制 | x 号舵机一次发射动作（待机 → 发射 → 保持 → 待机） |
| ATOM-12 | 视觉接口与 YAW 控制 | 视觉 rpm 目标 → YAW 电机 CAN 闭环、到位信号与超时回退 |
| ATOM-13 | 固定四发自动流程 | 按 `PEF.md` 顺序协调各机构 Atom，最多 4 发 |
| ATOM-14 | 安全、故障与恢复 | 故障记录、命令安全闸门、一般故障自动恢复、急停锁存与人工解除 |
| ATOM-15 | 配置与持久化 | 读取/校验配置、运行时热加载与保存 PID |
| ATOM-16 | HTTP 服务与 Web 前端 | 静态前端 + HTTP 分派为领域命令/查询，不直接触碰硬件 |
| ATOM-17 | WebSocket 实时状态 | 只读订阅、心跳、统一信封事件推送（控制命令一律走 HTTP） |
| ATOM-18 | 故障诊断与日志 | 故障/状态/流程可查询记录、JSON Lines 日志与轮转 |
| ATOM-19 | 调试与校准 | 只读调试查询 + 校准步骤管理（不定义校准算法） |

---

## 4. 目录结构

```text
.
├── CMakeLists.txt              # 顶层工程：平台守卫 + 可执行文件 + 模块组装
├── CMakePresets.json           # WSL aarch64 交叉编译预设（Debug/Release）
├── Entry.cpp                   # 进程入口（仅入口，不含启动逻辑）
├── cmake/
│   └── aarch64-linux-gnu.toolchain.cmake   # 交叉编译 toolchain
├── Atoms/                      # 19 个原子功能类模块（目录名与 ATOM-ID 对应）
│   ├── CMakeLists.txt          # 模块聚合清单 → marshland_atoms
│   ├── Public/                 # 各模块共享的公共头文件目录
│   ├── ATOM-01_Runtime/        # Inc/ + Src/ + CMakeLists.txt（同一模板）
│   └── ... ATOM-19_Debug_Calibration/
├── Design/                     # 架构设计文档（唯一权威来源）
│   ├── PRD.md  PEF.md  Interface.md  Config.md  Atoms.md
│   └── Atomic/ATOM-01 ~ ATOM-19
├── Docs/                       # 外设参考资料（如 C620 电调说明书）
├── Lib/                        # 参考库（如 PCA9685A 驱动参考实现）
└── Rule/                       # 工程规则文件
```

每个 ATOM 模块的构建契约完全统一：**不定义 `project()`、不产生可执行文件与 `main()`、不设置编译器/平台/C++ 标准/编译选项**（全部由顶层工程决定）；头文件放 `Inc/`、源码放 `Src/`，header-only 模块头文件直接放模块根目录；**有源码时建 `STATIC` 库，无源码时建 `INTERFACE` 库**，两种情况下上层都用同一个目标名 `Marshland::atom_NN_<名称>` 链接。

---

## 5. 构建

### 5.1 前置条件

- 运行目标：**Ubuntu 22.04 ARM64** 主机
- 开发方式：Windows + CLion，编译通过 **WSL（Ubuntu）交叉编译**完成；或在目标 ARM 主机上直接编译
- WSL 内需要：`cmake`（≥ 3.16）、`make` 或 `ninja`、`g++-aarch64-linux-gnu`（含 `libstdc++-*-dev-arm64-cross`）
- **禁止使用 x86 编译链编译本程序**（由顶层 `CMakeLists.txt` 的架构守卫强制拦截）

### 5.2 CLion + WSL 交叉编译

1. **Toolchains**：新建 WSL 工具链，C/C++ 编译器的 Toolset 路径必须指向
   `/usr/bin/aarch64-linux-gnu-gcc` 与 `/usr/bin/aarch64-linux-gnu-g++`
2. **CMake Profile**：选择该 WSL 工具链，并在 CMake options 中提供交叉编译配置，二选一：
   - 使用预设：启用 `CMakePresets.json` 中的 `arm64-debug` / `arm64-release`（预设已用 `vendor` 字段绑定 WSL 工具链，且不含任何绝对路径）
   - 或手填：`-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64`
3. `Reload CMake Project` → `Build`（**增量编译，不要使用 "Rebuild Project"**）

> 只把编译器换成 aarch64 版本、却不设置 `CMAKE_SYSTEM_NAME` 时，CMake 仍按本机构建，架构守卫会直接报错——这是有意设计，避免误产出 x86 程序。

### 5.3 命令行构建（WSL）

```bash
cmake -S /mnt/d/Project/C++/RM2027/Cocoon-Dart2027/NewVersion \
      -B build-arm64 \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.toolchain.cmake
cmake --build build-arm64            # 增量
```

验证产物架构：

```bash
file build-arm64/marshland          # 期望：ELF 64-bit LSB ... ARM aarch64
```

### 5.4 构建规则

- 所有代码变更后的构建**必须增量编译**，禁止全量重编译
- 顶层工程只负责**网关与各模块组装**，模块实现位于各自的 `Atoms/ATOM-XX_*/` 目录，并遵循该目录下的规则文件

---

## 6. 关键运行约束

### 6.1 控制权与优先级

- 控制源：`ControlSource { NONE, SBUS, WEB, AUTO }`
- 实际发射授权优先级：**`SBUS > WEB > 自动流程`**；低优先级不得覆盖、绕过或抢占高优先级
- **软件急停优先级最高**，覆盖一切普通控制源
- SBUS 接管自动流程时，原流程在当前硬件动作组完成后**终止**（不暂停、不恢复）
- 控制源选择规则与发射授权优先级**分开建模**；SBUS 失联降级至 WEB，重连后恢复最高控制权

### 6.2 固定四发流程

- 总弹数固定 **4 发**，不存在第 5 发，连续发射无额外固定间隔
- **第 1 发**：蓄力 → 变力 → 视觉 YAW 控制 → 发射
- **第 2~4 发**：蓄力 → 换弹抬起 → 供弹 → 等待 → 换弹放下 → 变力 → 视觉 YAW 控制 → 发射
- 每次发射后主程序必须把 YAW 到位信号设回 `False`
- 发射前必须重新检查控制权、停止发射标志、故障状态与软件急停状态

### 6.3 关键时间与周期参数

| 参数 | 值 |
|---|---|
| 系统最长控制周期 | 100 ms（硬性上限） |
| 电机闭环 / PID / CAN 收发 / GPIO / SBUS / WebSocket 推送 | 20 ms |
| 软件急停响应上限 | 100 ms |
| 视觉 YAW 无响应超时 | 30 s（超时由控制主系统置到位 `True` 并清除停止发射标志） |
| 换弹抬起/放下到位判定 | 角度命令发送后 500 ms |
| 供弹角度保持 / 供弹后等待 | 100 ms / 2 s |
| 发射角度保持 | 100 ms |
| YAW 调整最大动作时间 | 2000 ms（不阻断 30 s 等待） |
| 电机离线 / 反馈超时 | 200 ms |
| 电机温度阈值 | 75 ℃ |
| 控制周期与安全帧 | 零力矩帧每 20 ms 开环发送 |

### 6.4 安全与故障

- 通信异常：电机全部零力矩、舵机保持角度、禁止改变发射机构舵机角度、停止并禁止继续自动发射
- **一般故障自动恢复**：蓄力两电机回零（两个限位开关均为低电平）→ 仅发射机构执行 3 次发射动作（其他电机零力矩、其他舵机不动）→ 换弹机构抬起 → 进入手动状态
- **软件急停**：由 Web 前端或 ET16S 触发；持续发送零力矩帧、换弹机构抬起、拒绝一切发射机构舵机指令、禁止自动发射；**必须人工确认解除**，解除后不重新初始化、不恢复原自动流程
- 本版本不新增硬件急停方案

---

## 7. 对外接口

### 7.1 CAN（SocketCAN，CAN0 / 1 Mbps）

| 用途 | 说明 |
|---|---|
| 电机控制 | `0x200` → 0x201~0x204；`0x1FF` → 0x205~0x208；4 个 `int16_t` 电流值（mA），大端序，每 20 ms |
| 电机反馈 | `0x201`~`0x208`：转子角度、转速、转矩电流、温度；同时作为在线心跳 |
| 编码器反馈 | 取自电机反馈的转速字段，正负号区分方向 |
| 错误状态 | 设备上报的错误码 |
| 零力矩帧 | `1.200#0000000000000000`、`2.1FF#0000000000000000`，每 20 ms，发送失败进入错误状态并全机静默 |

电机 CAN ID：蓄力 = 0x201、0x202；YAW = 0x203；变力 = 0x204。
所有多字节量一律**大端序**；DLC 固定 8。

### 7.2 视觉系统

| 方向 | 内容 |
|---|---|
| 视觉 → 主程序 | YAW 目标控制量（rpm，`yaw_speed`）、YAW 到位 `bool`（按发次作用域） |
| 主程序 → 视觉 | 清除 YAW 到位（`False`） |
| 通信 | TCP `0.0.0.0:10001`；200 ms 无有效更新判失联；30 s 无响应走超时回退 |

### 7.3 SBUS

天地飞 ET16S 接收机，16 通道；摇杆量以 1024 为中心（范围 364~1684），开关量只取 364 / 1024 / 1684；支持手动控制、自动模式切换与软件急停触发；失联降级至 WEB。

### 7.4 HTTP

监听 `0.0.0.0:10000`，UTF-8，无登录、无权限分级、普通操作无二次确认（急停恢复必须人工确认）。

| 方法 | 路由 | 说明 |
|---|---|---|
| GET | `/api/state` | 总状态查询（始终可读） |
| GET | `/api/diagnostics` | 诊断与故障查询（始终可读） |
| POST | `/api/control/auto/start` | 启动自动发射（仅允许状态） |
| POST | `/api/control/auto/stop` | 设置停止发射标志（始终可用） |
| POST | `/api/control/manual` | 手动动作集（受仲裁限制） |
| POST | `/api/control/estop` | 触发软件急停（始终可用） |
| POST | `/api/control/estop/confirm` | 人工确认解除急停 |
| GET/POST | `/api/parameters` | 参数查询 / 修改 |
| POST | `/api/calibration` | 校准操作 |

手动动作集包括：`charge_start`、`charge_stop`、`charge_home`、`reload_lift`、`reload_lower`、`feed_once`、`force_set`、`yaw_set`、`servo_set`、`fire_once`、`stop_fire_set`、`stop_fire_clear`、`auto_start`、`auto_stop`。

### 7.5 WebSocket

仅用于**状态与结果推送**，控制命令一律走 HTTP；连接本身不获得高于 SBUS 的控制权。

统一信封：

```json
{ "version": "1.0", "type": "...", "timestamp": 0, "request_id": "...", "source": "WEB", "payload": {} }
```

消息类型：`system_state`、`mechanism_state`、`motor_state`、`sensor_state`、`yaw_state`、`fire_progress`、`fault`、`estop_state`、`control_arbiter`、`parameter_changed`、`error`。

心跳 1000 ms，连接超时 3000 ms（连续 3 个心跳周期无有效数据判定断开），推送周期 20 ms。

---

## 8. 配置与持久化

- **唯一值来源**：`Design/Config.md`（配置表）。代码、文档与测试都不得自带隐含默认值
- **运行时配置文件**：`./Config/dart_control.json`（JSON），程序初始化阶段加载与校验；缺失项使用内置默认值，加载失败按故障处理
- **热加载**：更新成功后立即生效并**持久化写回同一文件**；无掉电保护、无损坏恢复、无版本迁移
- **必须持久化**：各电机 PID、蓄力/变力时间、发射角度、YAW 速度、变力范围、校准参数，另预留 100 个 `reserved_001..100`（int32）
- **授权**：PID 参数仅允许来自 WEB，且仅手动/调试模式

| 类别 | 关键值示例 |
|---|---|
| CAN | CAN0、1 Mbps、经典 CAN、采样点 87.5%、SJW=1 |
| PID（电机 1~4） | Kp = 3、Ki = 0、Kd = 0，积分/输出限幅 16000 |
| 限幅 | 电流/力矩 ±15000 mA；编码器异常值 \|rpm\| > 7500 |
| 舵机 | `/dev/i2c-7`、地址 0x40、16 通道、12 位、50 Hz、脉宽 500~2500 µs、0~180° |
| 视觉 | TCP `0.0.0.0:10001`，`yaw_speed` ±16000 rpm |
| HTTP / WebSocket | `0.0.0.0:10000` / 消息版本 1.0 |
| 日志 | `./Logs/`，UTF-8 JSON Lines，10 MiB × 10 文件，上限 100 MiB，历史故障 1000 条 |
| 线程 | 独立线程 + IIC 串行；`SCHED_OTHER`（非硬实时）；线程栈 1 MiB |

仍为 `[待定]` 的参数：其他电机标识与 PID、软件急停人工确认/自动发射启动通道、蓄力限位开关 GPIO 编号、部分舵机 PCA9685A 通道与发射舵机角度、校准参数；压力传感器相关参数本版本不接入。

---

## 9. 设计文档索引

| 文档 | 版本 | 内容 |
|---|---|---|
| `Design/PRD.md` | 0.9（2026-09-26） | 项目需求设计文档 |
| `Design/PEF.md` | 1.4（2026-09-26） | 程序运行流程（四发流程、仲裁、急停、恢复） |
| `Design/Interface.md` | 1.4（2026-09-26） | 接口与架构定义（分层、模块边界、对外接口） |
| `Design/Config.md` | 0.7（2026-09-26） | 配置表，**具体数值的唯一来源** |
| `Design/Atoms.md` | 0.10（2026-09-26） | 原子功能清单（19 个功能类） |
| `Design/Atomic/ATOM-01 ~ 19` | 同步基线同上 | 各原子功能类详细规格 |

---

## 10. 工程协作规则（摘要）

完整规则见根目录 `AGENTS.md`（工程级规则的唯一来源）：

1. **代码修改必须先获得明确许可**
2. **构建必须增量编译**，禁止全量重编译
3. **目标平台为 Ubuntu ARM**，禁止使用 x86 编译链
4. 需求变更必须先对照 `Design/` 检查是否冲突或超出设计范围
5. 只读取根目录 `AGENTS.md` 作为工程级规则；其他规则文件统一放在 `./Rule/`

---

## 11. 当前实现状态与已知差异

**实现状态**

- 19 个 ATOM 模块目录与统一 CMake 骨架已建立，顶层工程已通过 `add_subdirectory(Atoms)` 全部组装并链接到 `marshland`
- `ATOM-01_Runtime` 已包含实现（`RuntimeCoordinator`：生命周期状态、逐步启动结果、安全门控、失败回滚与关闭释放）
- 其余模块目前为 `INTERFACE` 占位目标，等待各自实现落地；`Atoms/Public/` 为共享头文件目录
- `Entry.cpp` 目前只提供进程入口，**尚未接入启动协调**（启动协调属 ATOM-01 职责）

**已知差异（需要修订）**

当前无未修订的设计基线差异。

（原登记项「C++ 标准：顶层 `CMakeLists.txt` 为 C++17、设计基线为 C++23」已于 2026-09-27 关闭：顶层已改为 `CMAKE_CXX_STANDARD 23`，`CMAKE_CXX_EXTENSIONS OFF` 保证使用严格 `-std=c++23`，全部编译单元已通过 aarch64 交叉编译验证，零警告。）

**尚未建立的工程化设施**

- 单元测试与 CI
- 运行时配置基线 `Config/dart_control.json`（当前由程序在运行时生成/写回，已被 `.gitignore` 排除）

**已建立的工具链守卫**

顶层 `CMakeLists.txt` 在配置阶段执行 C++23 守卫，防止标准被静默降级（`CMAKE_CXX_STANDARD_REQUIRED` 只强制"请求"，编译器过旧时 CMake 仍会按 23 → 20 → 17 → 14 → 11 逐级回退并构建成功）。守卫含两项独立判据：

1. CMake 特性表必须包含 `cxx_std_23`；
2. 真实探测编译必须确认标准已生效（`static_assert(__cplusplus >= 202100L)`）。

通过时打印 `C++23 support confirmed: <id> <version>`；失败时 `FATAL_ERROR` 并给出编译器路径、id/版本与 target。

守卫**刻意不使用** C++23 的 `deducing this`（显式对象形参）作为探测：GCC 13.3 是可用且本项目实际使用的 C++23 工具链，但该特性到 GCC 14 才实现，以它设卡会产生误拒。
