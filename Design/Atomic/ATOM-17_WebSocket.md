# ATOM-17：WebSocket 实时状态

## 元信息

| 项目 | 内容 |
|---|---|
| Atom ID | ATOM-17 |
| 功能类 | WebSocket 连接与实时事件推送 |
| 程序代号 | Marshland |
| 状态 | 启用 |
| 来源 | PRD 12.1、12.3；Interface 15；Config 12、4 |
| 同步基线 | PRD 0.9、PEF 1.4、Interface 1.4、Config 0.7；2026-09-26 |

## 单一职责

建立状态订阅、心跳和服务端消息推送。WebSocket 为只读控制通道；控制命令必须使用 HTTP。连接参数、消息版本、状态周期等读取 Config。

## 内部原子操作

1. 接受一个 WebSocket 握手并创建只读订阅。
2. 校验一个客户端 ping 消息。
3. 返回一个关联 request_id 的 pong。
4. 判断一个连接是否超过心跳超时。
5. 关闭一个失效连接并释放其订阅资源。
6. 将一个领域事件序列化为 version/type/timestamp/request_id/source/payload 信封。
7. 推送一个 `system_state`。
8. 推送一个 `mechanism_state`。
9. 推送一个 `motor_state`。
10. 推送一个 `sensor_state`。
11. 推送一个 `yaw_state`。
12. 推送一个 `fire_progress`。
13. 推送一个 `fault` 或 `error`。
14. 推送一个 `estop_state`。
15. 推送一个 `control_arbiter`。
16. 推送一个 `parameter_changed`。
17. 向每个只读订阅客户端广播单个事件。
18. 收到控制类 WS 消息时拒绝执行并提示改用 HTTP。

## 配置/负载约束

- 推送周期、心跳 1000 ms、3000 ms 会话超时、重连退避和消息字段依 Config。
- YAW 状态包含目标、到位 bool 和主控制程序清除状态。
- 客户端重连不恢复自动流程，也不改变执行器状态。

## 验收条件

- 所有推送消息遵守统一信封和版本。
- WS 心跳只维护连接，不授予控制权。
- 一个客户端断开不影响其他只读客户端或硬件控制流程。