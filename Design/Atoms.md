# RoboMaster 2027 飞镖系统镖架控制主程序原子功能清单（Atoms）

## 1. 文档信息

| 项目 | 内容 |
|---|---|
| 文档名称 | 程序原子功能清单 |
| 程序代号 | Marshland |
| 关联文档 | `Design/PRD.md`、`Design/PEF.md`、`Design/Interface.md`、`Design/Config.md` |
| 文档状态 | 草案，供用户增删改 |
| 版本 | 0.10 |
| 日期 | 2026-09-26 |
| 功能类数量 | 19 |
| 同步基线 | PRD 0.9、PEF 1.4、Interface 1.4、Config 0.7 |

本清单把同一大类操作归并为 19 个功能类 Atom。每个 Atom 在 `Design/Atomic/` 有一份详细规格；详细规格中的命令、状态变更、硬件写入和等待仍拆成单一原子操作。

## 2. 原子操作要求

- 每个功能类 Atom 是一个可独立评审和验收的模块边界。
- 详细文档中的内部操作为单一输入处理、状态转换、硬件命令、查询或持久化操作。
- 多步流程由 PEFExecutor/StateMachine 协调，不能将整轮四发或跨机构流程包装成单一硬件原子操作。
- 逻辑状态更新须避免部分提交；硬件 I/O 必须返回明确成功/失败，不承诺多个物理执行器可回滚。
- 本文件为功能拆分规格，不是源代码。
- 根目录 `AGENTS.md` 禁止 Agent 实施 `Design/Atomic/` 文档提及的功能；本次仅维护设计文档，不实施代码。

## 3. 功能类索引

| Atom ID | 功能大类 | 范围摘要 | 详细文档 |
|---|---|---|---|
| ATOM-01 | 进程生命周期与启动协调 | 配置读取、模块初始化、待机/失败、关闭 | `Atomic/ATOM-01_Runtime.md` |
| ATOM-02 | 系统状态机与控制权仲裁 | 状态迁移、控制源选择、SBUS>WEB>AUTO 发射授权 | `Atomic/ATOM-02_State_Arbitration.md` |
| ATOM-03 | SBUS 遥控输入 | ET16S 帧接收、解析、自动模式标志、失联降级和重连 | `Atomic/ATOM-03_SBUS.md` |
| ATOM-04 | CAN/SocketCAN 传输适配 | CAN 初始化、单帧收发、协议分派、8-byte 零力矩帧 | `Atomic/ATOM-04_CAN.md` |
| ATOM-05 | 电机、编码器与 PID | 所有电机命令/反馈、编码器、闭环 PID、WEB 授权热加载 | `Atomic/ATOM-05_Motor_PID.md` |
| ATOM-06 | GPIO 与限位输入 | GPIO 采样、去抖、蓄力双电机回零判定 | `Atomic/ATOM-06_GPIO_Sensors.md` |
| ATOM-07 | PCA9685A/IIC 舵机底层 | PCA 初始化、单通道舵机目标和错误 | `Atomic/ATOM-07_Servo_Driver.md` |
| ATOM-08 | 蓄力机构控制 | 两个蓄力电机；四发每发蓄力及双限位回零 | `Atomic/ATOM-08_Charge.md` |
| ATOM-09 | 变力机构控制 | 四发每发变力、电机闭环和通用故障上报 | `Atomic/ATOM-09_Force.md` |
| ATOM-10 | 换弹与供弹控制 | 四舵机动作组、单舵机供弹步骤 | `Atomic/ATOM-10_Reload_Feed.md` |
| ATOM-11 | 发射机构控制 | x 号舵机一次发射动作 | `Atomic/ATOM-11_Fire.md` |
| ATOM-12 | 视觉接口与 YAW 控制 | rpm 目标、CAN YAW 闭环、视觉 bool 和 30 s 到位回退 | `Atomic/ATOM-12_Vision_YAW.md` |
| ATOM-13 | 固定四发自动流程 | 四发每发蓄力/变力、后 3 发换弹供弹、停止/授权 | `Atomic/ATOM-13_Auto_Fire.md` |
| ATOM-14 | 安全、故障与恢复 | 双电机回零、三次发射、急停和 8-byte 零力矩 | `Atomic/ATOM-14_Safety_Recovery.md` |
| ATOM-15 | 配置与持久化 | JSON 配置、PID WEB 授权、运行时热加载与保存 | `Atomic/ATOM-15_Config_Persistence.md` |
| ATOM-16 | HTTP 服务与 Web 前端 | Config action 集、控制/查询 UI、PID 授权 | `Atomic/ATOM-16_HTTP_Web.md` |
| ATOM-17 | WebSocket 实时状态 | 心跳、版本信封、状态和结果推送 | `Atomic/ATOM-17_WebSocket.md` |
| ATOM-18 | 故障诊断与日志 | 故障历史、状态快照、日志与轮转 | `Atomic/ATOM-18_Diagnostics_Logging.md` |
| ATOM-19 | 调试与校准 | 只读调试、校准步骤与结果 | `Atomic/ATOM-19_Debug_Calibration.md` |

## 4. 同步结论

已按当前主设计基线同步 `Design/Atomic/ATOM-01` 至 `ATOM-19`。功能细节以 PRD 0.9、PEF 1.4、Interface 1.4、Config 0.7 为准；每份 Atom 详细规格的元信息均记录同一同步基线。

1. 固定四发；第 1～4 发每发均执行蓄力和变力；第 2～4 发额外执行换弹/供弹。
2. YAW 目标单位 rpm、视觉内部到位判据为像素误差；YAW 电机动作超时按 Config 故障处理。视觉 30 s 无响应属正常回退路径：由控制主系统将 YAW 到位信号置为 `True`、清除停止发射标志，再通过发射授权检查。
6. 30 s 回退后的迟到视觉状态仅影响尚未实际发射的当前发次；已发射完成的发次不得回滚，仅记录诊断事件。
3. 压力传感器不属于本版本，不执行接入或流程/安全判定。
4. PID 参数修改仅 WEB 来源，且仅手动/调试模式；成功更新按 Config 运行时热加载并持久化。
5. 零力矩帧按 Config 的 CAN ID 0x200/0x1FF、8 字节和大端序规则。
6. 控制源选择规则与实际发射授权优先级分开建模；实际发射授权为 `SBUS > WEB > 自动流程`。
7. SBUS 失联降级至 WEB；恢复后重新取得 SBUS 优先级，但不恢复已终止的自动流程。
8. Config 的 `[待定]` 参数为预留接入点，Atom 规格不推定通道、角度、CAN ID、阈值或其他具体值。
9. 校准模式与调试模式的进入均无需前提（急停或活动故障锁定时除外），不新增独立模式路由。
10. `/api/control/manual` 动作集链路用于正常状态下的手动/自动操作；`/api/control/auto/start` 与 `/api/control/auto/stop` 专用路由链路用于应急操作；两条链路同时存在。
11. SBUS 抢占统一为“当前硬件动作组完成后终止”，不暂停、不恢复。
12. 蓄力回零统一作用于两个蓄力电机（电机 1、2）。
13. `Config.md` v0.7 已确定绝大多数参数（CAN 采样点、通道、角度、PID、视觉/TCP 字段等），Atom 规格中的具体值一律以 `Config.md` 为准；预留接入点以 `[待定]` 表示。

## 5. `Design/Atomic/` 同步状态

`Design/Atomic/` 的 19 份规格已按 PRD 0.9、PEF 1.4、Interface 1.4、Config 0.7 基线同步，此前登记的差异均已处理：

| 已处理项 | 结论 | 落点 |
|---|---|---|
| 30 s 回退写入 YAW 到位 bool | 允许；由控制主系统将 YAW 到位信号置为 `True` 后按到位处理 | `ATOM-12` 单一职责、第 9 项、发射安全条件、验收条件 |
| PEF 版本引用 | PEF 1.4 | `ATOM-13` 单一职责 |
| 校准模式进入前提 | 无需前置校验；由主状态机在移交校准操作时转入 CALIBRATION | `ATOM-19` 校准第 1 项与验收条件；`ATOM-02` 第 14 项 |
| 换弹到位计时措辞 | “动作组角度命令发送后 500 ms” | `ATOM-10` 抬起/放下计时与验收条件 |
| 同步基线版本与日期 | PRD 0.9、PEF 1.4、Interface 1.4、Config 0.7；2026-09-26 | 全部 Atom 元信息 |

## 6. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.1 | 2026-09-23 | 原有细粒度 Atom 清单。 |
| 0.2 | 2026-09-23 | 归并为 19 个功能大类并建立详细规格。 |
| 0.3 | 2026-09-23 | 记录当时待同步差异。 |
| 0.4 | 2026-09-25 | 修订 Atomic 规格以对齐 PRD 0.6、PEF 1.1、Interface 1.1、Config 0.4。 |
| 0.5 | 2026-09-25 | 补齐各 Atom 的来源引用与单一职责表述。 |
| 0.6 | 2026-09-25 | 明确 30 s 视觉无响应回退时由控制主系统清除停止发射标志；正常 YAW 到位时仍由视觉系统清除，并同步 Atom 规格与主流程文档。 |
| 0.7 | 2026-09-25 | 同步 19 份 Atomic 规格至 PRD 0.7、PEF 1.2、Interface 1.2、Config 0.5 基线。 |
| 0.8 | 2026-09-25 | 按第二轮裁决统一：30 s 回退由控制主系统将 YAW 到位置 `True`；校准/调试模式进入无需前提；动作集与应急路由双链路；蓄力回零为两个电机；版本与日期元数据统一；登记 Atomic 待同步差异。 |
| 0.9 | 2026-09-25 | 同步 `Design/Atomic/ATOM-01`～`ATOM-19` 至 PRD 0.8、PEF 1.3、Interface 1.3、Config 0.6：允许超时回退置 YAW 到位为 `True`、PEF 引用改为 1.3、校准进入无需前置校验、换弹计时措辞统一、全部同步基线更新。 |
| 0.10 | 2026-09-26 | 加入程序代号 Marshland；同步至 `Config.md` v0.7 参数基准（PRD 0.9、PEF 1.4、Interface 1.4、Config 0.7），占位术语统一为 `[待定]`，统一蓄力回零为两个电机表述。 |



