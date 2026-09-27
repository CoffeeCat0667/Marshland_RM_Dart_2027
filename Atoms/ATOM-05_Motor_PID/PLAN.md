# ATOM-05 Motor PID 实现方案（初版）

## 1. 目标与边界

- 实现电机映射、命令校验、CAN 反馈解析、编码器状态判断、PID 参数管理和单电机 PID 闭环更新。
- 按电机 ID 独立维护命令、反馈快照、PID 配置、运行状态和故障状态。
- 不实现自动发射流程，不提供 `main` 入口；本模块作为上层 Marshland 工程的可复用组件被调用。
- 所有普通电机命令先经过 ATOM-02 与 ATOM-14 的安全约束；PID 修改仅允许 WEB 控制源且系统处于手动或调试模式。

## 2. 预期目录结构

```text
ATOM-05_Motor_PID/
├── CMakeLists.txt
├── PLAN.md
├── Inc/
│   └── ... 公共头文件
└── Src/
    └── ... 实现文件
```

头文件只放在 `Inc`，实现只放在 `Src`。当前阶段先完成模块 CMake 骨架和方案文档，后续根据 Public 接口实际定义补齐源文件。

## 3. 建议模块划分

1. **MotorMapping / MotorRegistry**
   - 保存当前 Config 中的电机映射：蓄力电机 1、变向电机 4、RAW 电机 3。
   - 统一按电机 ID 查找、校验和管理电机通道。
2. **MotorCommandValidator**
   - 校验控制模式、目标值、限幅、系统安全状态及命令来源。
   - 拒绝未授权或不满足安全前置条件的命令。
3. **MotorFeedbackDecoder**
   - 从 CAN 反馈提取有符号转速、方向、有效性和时间戳。
   - 将异常帧、无效编码器数据与超时状态明确区分，禁止伪装成有效读数。
4. **PidConfigService**
   - 读取并校验 `PIDConfig`。
   - 仅在 WEB + 手动/调试条件下接受修改；通过校验后立即热加载，并分别报告热加载结果和持久化结果。
5. **MotorPidController**
   - 执行单次 PID 更新和按 Config 周期调度。
   - 检测 NaN/无穷输出、连续 5 个周期输出饱和且误差未减小等故障条件。
   - 故障时输出零力矩、禁用对应闭环，并向故障管理器报告。
6. **MotorChannelState**
   - 保存启用、禁用、停止、反馈有效性、超时、故障及最近控制量等状态。
   - 提供单电机反馈快照查询。

## 4. 核心接口方向

- `configureMapping(...)`：加载并校验电机映射。
- `submitCommand(motor_id, command, safety_context)`：提交单电机命令。
- `updateFeedback(motor_id, can_feedback, timestamp)`：更新 CAN 反馈。
- `feedbackSnapshot(motor_id)`：查询反馈快照。
- `readPidConfig(motor_id)` / `requestPidUpdate(...)`：读取和受控更新 PID 参数。
- `enable(motor_id)`、`disable(motor_id)`、`stop(motor_id)`：管理单个闭环通道。
- `updateOnce(motor_id, dt)`：执行一次 PID 更新并返回控制量。
- `tick(timestamp)`：按 Config 周期调度各电机 PID 更新。
- `faultStatus(motor_id)`：查询故障及安全处置结果。

具体类型和依赖必须优先复用 `Public` 中已有接口；如果接口信息不足，先补充调查，不直接臆造跨模块协议。

## 5. 数据与安全约束

- 所有状态按电机 ID 隔离，不能用一个电机的反馈或 PID 参数覆盖另一个电机。
- 反馈处理保留有符号转速和方向信息；反馈无效或超时不得参与闭环控制。
- PID 更新请求按“来源、系统模式、参数范围、热加载结果、持久化结果”逐项记录。
- 持久化失败时只能报告失败，不能报告为已保存。
- PID 故障必须触发零力矩、停用对应闭环，并向故障管理器报告。
- 模块不得自行决定自动发射流程；只提供电机控制与反馈能力。

## 6. CMake 集成方案

- 本目录提供 `atom05_motor_pid` 模块目标，初始使用 `INTERFACE` 目标作为无源码阶段的可链接骨架。
- 上层工程通过 `add_subdirectory(Atoms/ATOM-05_Motor_PID)` 后链接该目标；编译器、目标平台、警告和构建类型均由上层工程统一决定。
- 实现文件建立后，将目标调整为实际编译目标，并继续公开 `Inc` 目录；不在本模块覆盖上层编译选项。

## 7. 实现顺序与验证

1. 调查 `Public` 中的公共类型、CAN 接口、Config、故障管理和安全上下文接口。
2. 固化电机映射和状态模型，先实现命令及反馈路径。
3. 实现 PID 参数校验、权限判断、热加载与持久化结果分离。
4. 实现单次 PID、周期调度、数值异常和连续饱和检测。
5. 接入故障安全处置及查询接口。
6. 在上层 ARM 构建链下验证 CMake 集成；补充单元测试或可测试的纯逻辑接口。
7. 验证验收条件：按电机 ID 独立管理、保留方向、WEB 权限生效、PID 立即生效、持久化结果真实报告、故障时安全停机。

## 8. 当前未决项

- `Public` 中现有公共头文件和类型名称尚未调查。
- CAN 帧格式、反馈超时阈值、PID 参数边界、Config 周期和持久化接口需要以现有公共接口及配置定义为准。
- ATOM-02、ATOM-14 和故障管理器的具体调用协议需要在实现前确认。
