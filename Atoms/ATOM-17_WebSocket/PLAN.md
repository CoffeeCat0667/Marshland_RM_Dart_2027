# ATOM-17 WebSocket 实现方案（初步）

## 1. 目标与边界

ATOM-17 负责建立只读 WebSocket 状态订阅、连接心跳、统一事件封装和实时广播；不负责执行控制命令。任何控制类 WS 消息必须拒绝执行，并提示调用方改用 HTTP。

本模块不提供 `main` 入口，不独立决定平台、编译器或编译选项；这些内容由上层 `NewVersion/CMakeLists.txt` 统一决定。

## 2. 计划目录结构

```text
ATOM-17_WebSocket/
├─ CMakeLists.txt
├─ PLAN.md
├─ Inc/
│  └─ atom17_websocket/       # 对外头文件
└─ Src/                       # 非 header-only 实现
```

当前先完成模块 CMake 骨架和本方案；`Inc`、`Src` 以及具体实现将在接口和依赖进一步确认后添加。

## 3. 初步组件设计

### 3.1 订阅与连接管理

- `WebSocketServer`：接受握手、创建只读订阅、维护客户端集合。
- `WebSocketSession`：保存连接标识、最近心跳时间、订阅资源和发送接口。
- `SubscriptionRegistry`：广播事件到所有仍有效的只读客户端。
- 断开或心跳超时后移除会话并释放对应订阅资源；单个客户端故障不得影响其他客户端或硬件控制流程。

### 3.2 心跳

- 默认心跳周期为 1000 ms，会话超时为 3000 ms。
- 校验客户端 ping，生成带相同关联 `request_id` 的 pong。
- 心跳只用于维持连接，不改变执行器状态、不授予控制权限。
- 推送周期、超时、重连退避等参数通过 Config 注入，不在模块中硬编码为不可配置值。

### 3.3 消息模型

所有服务端推送使用统一信封：

```text
version / type / timestamp / request_id / source / payload
```

支持的事件类型至少包括：

- `system_state`
- `mechanism_state`
- `motor_state`
- `sensor_state`
- `yaw_state`
- `fire_progress`
- `fault` / `error`
- `estop_state`
- `control_arbiter`
- `parameter_changed`

其中 `yaw_state` 必须能够表达目标、到位 `bool` 以及主控制程序清除状态。

### 3.4 控制隔离

收到控制类 WebSocket 消息时，只返回明确的拒绝/改用 HTTP 提示，不调用任何执行器或控制流程。客户端重连也不恢复自动流程、不改变执行器状态。

## 4. 依赖与接口策略

1. 先定义与具体 WebSocket 库解耦的传输适配接口，避免把第三方库类型泄漏到公共头文件。
2. Config、领域事件和时间源通过接口或依赖注入提供；具体配置键、序列化库和网络库待上层公共接口确认后接入。
3. 本模块只声明自身需要的依赖，不修改上层 CMake、设计文件或其他上层目录文件。
4. 当前 CMake 使用 `INTERFACE` 目标作为模块化占位，待实现文件落地后可平滑改为静态/对象库，而不改变上层链接名称。

## 5. 实现阶段

1. 明确 Config、事件模型、时间源、日志和网络适配器的公共接口。
2. 创建 `Inc/`、`Src/`，实现会话生命周期、心跳超时和只读权限隔离。
3. 实现统一消息信封、事件序列化及各类状态事件广播。
4. 接入上层构建并补充测试：握手、ping/pong、超时清理、多客户端广播、控制消息拒绝、断开隔离、消息信封字段校验。
5. 在允许的 ARM 构建环境下由上层 CMake 验证编译和集成。

## 6. 验收对照

- 所有推送消息包含统一信封和版本字段。
- ping/pong 关联 `request_id`，心跳超时会清理会话。
- WebSocket 始终是只读通道，控制命令不会执行。
- 一个客户端断开不影响其他客户端和硬件控制流程。
- 配置项由 Config 提供，客户端重连不恢复自动流程。
