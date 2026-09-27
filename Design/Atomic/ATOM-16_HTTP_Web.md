# ATOM-16：HTTP 服务与 Web 前端

## 元信息

| 项目 | 内容 |
|---|---|
| Atom ID | ATOM-16 |
| 功能类 | HTTP 服务与 Web 前端交互 |
| 程序代号 | Marshland |
| 状态 | 启用 |
| 来源 | PRD 6、7、12；Interface 14；Config 11.1～11.4 |
| 同步基线 | PRD 0.9、PEF 1.4、Interface 1.4、Config 0.7；2026-09-26 |

## 单一职责

承载静态 Web 前端和 HTTP 请求，将每个请求分派为领域命令或查询；不直接操作硬件。监听、静态资源、路由、大小限制和错误结构由 Config 定义。无登录/权限等级，普通操作无二次确认；急停解除仍须人工确认。

## 服务与 HTTP 原子操作

1. 绑定 Config 指定的监听地址和端口。
2. 提供一个静态前端资源。
3. 检查一个请求体是否超过 Config 上限。
4. 解析一个 JSON 请求；解析失败返回统一 JSON 错误。
5. 将一个系统状态查询分派给状态聚合器。
6. 将一个诊断查询分派给 ATOM-18。
7. 将自动启动请求分派给 ATOM-13。
8. 将 stop_fire 更新请求分派给控制服务。
9. 将一个 `action` 手动控制请求分派给 ATOM-02/对应机构 Atom。
10. 将一个软件急停请求分派给 ATOM-14。
11. 将一个急停人工确认请求分派给 ATOM-14。
12. 将一个参数查询/更新请求分派给 ATOM-15。
13. 将一个校准请求分派给 ATOM-19。
14. 将一个结果序列化为 Config 定义的 JSON 响应。
15. 将一个错误序列化为 Config 定义的 JSON 错误响应。
16. 对长流程立即返回 accepted 和 request_id，不等待整个自动流程完成。
17. 未注册的模式切换路由返回错误；初始化由进程启动执行，不新增独立模式路由。

## WEB 手动 action 集

`POST /api/control/manual` 仅接受 Config 11.4 定义的 action：

- `charge_start`、`charge_stop`、`charge_home`；
- `reload_lift`、`reload_lower`、`feed_once`；
- `force_set`、`yaw_set`、`servo_set`、`fire_once`；
- `stop_fire_set`、`stop_fire_clear`；

- stop_fire_clear 是第三条停止发射标志清除路径，仅允许在手动/调试模式执行；执行前须退出自动模式进入手动模式。该动作清除标志但终止当前自动流程，不允许自动流程继续或恢复。
- `auto_start`、`auto_stop`。

每个 action 到达后须分别校验 target、允许模式、控制权和安全状态，再分派给相应 Atom。不得接受 Config 列表之外的隐含 action。

### 特殊 WEB 控制约束

- `yaw_set` 和 `servo_set` 按 Config 的模式与控制权约束执行；手动 YAW 目标为 rpm。
- `servo_set` 仅允许在 Config 指定的调试模式；软件急停时发射舵机所有普通目标一律拒绝。
- PID 参数更新必须是 WEB 来源且处于手动或调试模式；经校验后热加载并持久化，结果分别反馈生效和写入状态。
- WEB 与 SBUS 冲突时服从 `SBUS > WEB > 自动流程`。

## 前端原子操作

1. 显示初始化结果、系统模式和当前控制源。
2. 显示当前发次和自动流程阶段。
3. 显示一个机构状态。
4. 显示一个电机目标/反馈状态。
5. 显示一个编码器状态。
6. 显示 GPIO/限位状态。
7. 显示视觉 YAW 目标和 bool 到位状态。
8. 显示一个舵机最近目标及驱动状态。
9. 显示 CAN/IIC 状态和活动故障。
10. 通过 HTTP 提交一个 Config 允许的控制、参数、校准或安全请求。
11. 显示该请求的接受、拒绝、错误或恢复结果。
12. 仅在 WEB 来源且手动/调试模式时显示 PID 参数的可编辑能力。

## 安全与验收条件

- 所有控制请求经状态机、ControlArbiter 和 SafetyManager。
- 急停、故障、模式错误或高优先级抢占时，普通 WEB 执行请求拒绝或按 PEF 规则终止。
- WEB 断线不得启动、恢复或继续已经终止的自动流程。
- 急停时拒绝一切发射舵机普通命令。
- 静态资源请求不触发机构动作。
- API action 集和请求字段与 Config 11.2、11.4 一致。