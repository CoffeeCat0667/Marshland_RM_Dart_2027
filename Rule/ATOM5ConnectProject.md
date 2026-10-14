# ATOM-05_Motor_PID 工程接入说明

> 版本：1.0
> 日期：2026-10-14
> 适用模块：`Atoms/ATOM-05_Motor_PID`
> 验证范围：源码走查、文本静态检查、有限状态模型枚举；未编译，未运行 CTest、模拟器或硬件。

## 1. 接入目标与职责

ATOM-05 提供六个已配置电机的命令校验、CAN 反馈解析、速度 PID、通道启停、反馈查询、受控 PID 热加载、持久化请求及故障通知。模块不决定自动发射流程，不实现换弹位置控制、回零、限位判定或设备错误码的猜测映射，不提供 `main()`。

外部装配负责人实现 CAN 输出桥、权限与安全桥、故障桥、持久化后端并注册生命周期。本说明不是这些外部实现已经完成的声明。

## 2. CMake 接入

```text
上层工程 -> Marshland::atom_05_motor_pid
                         -> Marshland::PublicHeaders
                         -> Threads::Threads
外部 CAN 输出桥 -> ATOM-04
```

1. 先创建 `Marshland::PublicHeaders`，再 `add_subdirectory(Atoms/ATOM-05_Motor_PID)`。
2. 保持目标 `atom_05_motor_pid` 和别名 `Marshland::atom_05_motor_pid`。当前五个 `.cpp` 组成静态库，公开 `Inc/`。
3. 模块不链接 ATOM-04 实现，不包含 ATOM3/4、RuntimeCoordinator、StateArbitrator 或 AppCoordinator 的实现头；外部桥负责调用这些对象。
4. 编译器、Ubuntu ARM 平台、C++23 与编译选项由上层决定。整树聚合与 `Entry.cpp` 接线不在本轮修改范围。
5. `Threads::Threads` 是 PID 工作线程的传递依赖。本模块没有独立可执行目标或测试入口。

## 3. 对外接口位置

本轮没有新增或修改 `Atoms/Public` 文件。新接口属于 ATOM-05 的公开 API，位于 `Inc/atom05_motor_pid/`，消费方链接本模块即可使用。

| 头文件 | 内容 |
|---|---|
| `motor_types.hpp` | 命令、模式、反馈、快照、PID、更新结果、故障事件、输出批次及参数校验 |
| `motor_interfaces.hpp` | `IMotorSafetyGate`、`IMotorCurrentOutput`、`IMotorFaultSink`、`IPidPersistence` |
| `motor_config.hpp` | `MotorRuntimeConfig`、配置加载结果与加载函数 |
| `motor_core.hpp` | `MotorCore`，实现 Public 的 `ICanFrameSink` |
| `pid_controller.hpp` | 无线程、无 I/O 的单次 PID 计算器 |
| `feedback_decoder.hpp` | C620 反馈解析，不推测保留字节语义 |
| `lifecycle_adapter.hpp` | 两个生命周期阶段的薄适配器 |

复用 Public 的 `CanFrame`、槽位映射、`IConfigView`、`IMonotonicClock`、`ILifecycleModule`、`ControlSource` 和 `SystemMode`，不重复定义它们。

## 4. 装配对象与生命周期

外部创建并持有以下对象：

```text
配置视图 + 单调时钟
安全权限桥 + 故障桥 + PID 持久化后端
尚未绑定 CAN 的输出桥（ready() == false）
  -> MotorCore（注入以上五个依赖）
  -> ATOM-04 CanCore（frame_sink 绑定到 MotorCore）
  -> 输出桥绑定到 ATOM-04
  -> LOAD_PID 与 INITIALIZE_CHANNELS 两个 LifecycleAdapter
```

构造示例仅展示本模块 API；其中五个依赖由外部实现并持有：

```cpp
using namespace marshland::atom05;
MotorCore core({clock, safety_gate, current_output, fault_sink, pid_store});
LifecycleAdapter pid_stage(core, config_view, LifecycleStage::LOAD_PID);
LifecycleAdapter channel_stage(core, config_view, LifecycleStage::INITIALIZE_CHANNELS);
```

| 适配器 | 注册步骤 | 行为 |
|---|---|---|
| `pid_stage` | `RuntimeStep::LoadPidParameters` | 从只读配置视图加载完整配置，校验后一次提交；不启动线程 |
| ATOM-04 适配器 | `RuntimeStep::InitializeSocketCan` | 外部负责注册并打开 CAN，开始投递反馈 |
| `channel_stage` | `RuntimeStep::InitializeMotorAndEncoderChannels` | 确认配置版本未变化、输出桥就绪，先锁存零电流再启动 PID 线程 |

建议将两个 ATOM-05 步骤注册为必需步骤；最终 `required` 由启动策略决定。装配层必须检查注册返回值并读取注册失败原因，模块不自行注册。

通道阶段启动成功不代表各节点已经收到有效反馈。上层须先检查快照新鲜度再提交动作；在无反馈时提交命令会拒绝并锁存反馈故障，不能自动猜测编码器数据。

依赖对象必须比 core 存活更久。关闭时先阻止新控制/PID 请求并等候进行中的持久化请求结束，再关闭 ATOM-05 线程，随后关闭 ATOM-04 并停止其反馈投递，最后销毁 core 与桥。两个适配器的 `shutdown()` 都调用幂等 `core.stop()`；重复关闭不再次提交 CAN 输出。销毁前仍须保证没有并发调用或未结束的回调。

## 5. CAN 输出桥契约

外部实现 `IMotorCurrentOutput`：

- `ready()`：确认 ATOM-04 已初始化且桥已绑定；不是普通动作授权。
- `commit(batch)`：把两组 `currents` 对应到 `0x200`、`0x1FF`，调用 ATOM-04 的锁存接口 `latchMotorCurrents()`。只表示锁存成功，绝不表示已经发送或设备已确认。
- `forceZero()`：通过 ATOM-04 的 `forceZeroCurrents()` 等安全锁存路径清空两组槽位，不依赖可用的 batch 代次。返回值仍只表示本地零电流锁存结果。

`generation` 在一个 core 实例中递增；桥拒绝过期或重复批次，`forceZero()` 不重置已接受的代次。若更换 core 实例，先进入零力矩、停用旧实例和 TX，再重新绑定并初始化桥的代次。

`active_mask` 的 bit 0～5 对应 node `0x201`～`0x206`，不是电机数组下标；两组中未使用的槽位永远为零。桥必须持有并验证整个批次。任一组提交失败，必须清空两组并返回 false；不能只成功更新一组，不能虚报成功。

两次 `latchMotorCurrents()` 本身不能证明跨组原子性。外部必须与 ATOM-04 TX/安全锁存方共享提交屏障，或采用等价的整批原子机制，使 TX 不观察到半个新批次；若现有 ATOM-04 API 无法提供该机制，这是外部接入阻塞项，不能用两个普通调用冒充原子提交。

桥还必须协调 ATOM-04 的零力矩/控制输出门控：默认零力矩，安全锁存高于普通控制；禁止周期性交替发送零帧和非零控制帧。安全覆盖拒绝活动批次时返回 false，不允许静默接受；安全覆盖发生时撤销 core 的旧目标，解除覆盖不自动恢复它们。

`commit()`、`ready()` 和 `forceZero()` 不执行阻塞 I/O、不回调 core，且必须在控制周期预算内返回。CAN 物理发送及发送失败发布由 ATOM-04/安全模块负责。`zero_latched=true` 也不等于实际零力矩帧已发送成功。

## 6. 单位与反馈

内部控制输出单位为 mA，最终电流绝对值不超过 `15000`。C620 命令编码为 `truncate(mA * 16384 / 20000)`，例如 ±15000 mA 对应 ±12288 协议计数；输出桥传递计数，不再次缩放。联调负责人须核对设备型号与协议缩放，不能把协议计数直接当成 mA。

反馈使用 DLC=8、node `0x201`～`0x206` 的 C620 布局：

| 字节 | 语义 |
|---|---|
| 0～1 | 大端无符号转子角度，0～8191；不是累积位置 |
| 2～3 | 大端有符号转速 rpm；保留正负方向 |
| 4～5 | 大端有符号设备电流原始值；快照不臆造反馈缩放 |
| 6 | 温度；达到 75℃ 判为过温 |
| 7 | 保留字节，不据此推测错误码 |

`abs(rpm)>7500`、结构错误和过温使反馈无效。时间戳必须与注入时钟共享单调纪元；未来时间戳无效，重复或更旧的帧不刷新反馈。时间戳 0 的真实首帧有效，以 `has_received` 区分未收到。

RX 回调只更新固定容量状态并唤醒工作线程，不回调 ATOM-04、不写文件、不调用故障桥。解析异常保持为待处理故障，即使随后来了有效帧，也不能在工作线程处理前消失。

## 7. 配置读取

配置只经 `IConfigView` 读取，不打开 JSON。以下键必须存在，类型错误、缺失或越界导致初始化失败：

| 键 | 范围/结构 |
|---|---|
| `timing.max_control_cycle_ms` | 整数 1～100 |
| `timing.motor_loop_period_ms`、`timing.pid_period_ms` | 正整数，不超过最大控制周期 |
| `timing.can_comm_timeout_ms` | 整数 1～60000，部署基线 200 ms；外部已补充该键，本轮不重新修改 JSON |
| `persistence.directory` | 非空、无内嵌 NUL，UTF-8 字节数小于 256 |
| `persistence.file_name` | 非空、无内嵌 NUL，UTF-8 字节数小于 128 |
| `motor` | 六个对象；允许数组重排，不允许缺失或重复映射 |
| `motor[].id`、`node`、`role` | 按下表校验；node 使用 `0x` 加三个十六进制数字 |
| `motor[].bus` | `CAN0`；Linux 设备实际打开名由 ATOM-04 决定 |
| `motor[].control_mode`、`encoder_source`、`target_unit` | `CLOSED_LOOP_PID`、`CAN`、`rpm` |
| `motor[].pid.*` | 见下一节，元素内键通过 `getIntAt("motor", i, "pid.kp", ...)` 等读取 |

| ID | node | role |
|---|---|---|
| 1 | `0x201` | `charge_right` |
| 2 | `0x202` | `charge_left` |
| 3 | `0x203` | `reload_right` |
| 4 | `0x204` | `reload_left` |
| 5 | `0x205` | `yaw` |
| 6 | `0x206` | `force` |

加载期间配置视图必须被冻结，或其字符串存储由外部读租约保护。`IConfigView` 的裸字符串指针只保证热加载前有效，结束时检查版本不能代替指针寿命保护。初始化两个阶段之间版本变化会拒绝启动；core 不在 PID 线程内重新查询配置。

当前编码器 7500 rpm、温度 75℃、执行器 15000 mA 保护常量来自已核对设计；若要配置化或变更这些值，必须同步设计、加载接口和代码，不能只修改 JSON。

## 8. PID 与运行期更新

| 参数 | 允许值 |
|---|---|
| `kp`、`ki`、`kd` | 整数 -1000～1000，允许负增益 |
| `integral_limit` | 整数 0～16000；积分项贡献绝对值上限，0 禁止积分贡献 |
| `output_limit` | 整数 1～16000；实际输出还受 15000 mA、命令和安全许可限幅约束 |
| `control_period_ms` | 整数 1～100，且不超过本次加载的最大控制周期 |
| `enabled` | 布尔值 |

`3/0/0/16000/16000/20/true` 合法。不自动钳位非法参数。参数边界依据用户裁决确定，应由外部维护者同步至设计/Schema；它们不构成稳定性保证。

PID 公式采用 `P=kp*error`、`I+=ki*error*dt`、`D=kd*(error-prev_error)/dt`。首周期不计算差分，时间单位为秒，积分项贡献限幅；仅阻止把输出进一步推入饱和的积分增量。达到有效输出限幅且误差未改善的连续序列计数达到 5 时锁存故障；误差改善重新从 1 计数，未饱和清零。

`updatePid()` 只在 core 运行、请求源 WEB 且外部许可确认 MANUAL/DEBUG 时接受。热加载在控制锁内提交、重置历史；活动速度通道立即以新参数计算。禁用 PID 撤销目标，换弹一侧禁用会同时停止另一侧。更新不会清故障，也不会自动重启已停用通道。

`PidUpdateResult.applied` 与 `persistence` 独立：

- `applied=false`：参数未修改，未尝试持久化。
- `applied=true`：活动参数已提交；`status` 仍可能报告输出或控制故障。
- `persistence=SAVED`：外部后端明确报告保存成功。
- `FAILED/UNAVAILABLE`：活动参数不回滚，但不能声称已保存。

`IPidPersistence::saveMotorPid()` 在请求线程上、控制锁外调用；同一 core 的修改与保存串行，避免旧编辑晚保存。后端必须按 motor ID 修改单个 PID 对象、保留其他 JSON 字段、与其他配置写入方协调，不回调生命周期或 `updatePid()`。日志与 HTTP 请求关联由外部保留 `request_id` 并分别记录生效和保存结果。

## 9. 控制、安全与故障接线

`IMotorSafetyGate` 必须联合 ATOM-02 控制权和 ATOM-14 安全状态判断。请求携带的来源不是可信授权：桥应核对真实请求上下文。`checkCommand()`、每次输出的 `checkOutput()`、`checkPidEdit()` 和 `allowFaultReset()` 都应失败关闭，不调用 core，不能持有 `StateReadView` 租约再回调其他状态端口。

`VELOCITY` 为 rpm；`CURRENT/TORQUE` 均为 mA 电流目标，并不提供牛顿米到电流的转换。换弹 3/4 只接受 VELOCITY 或 DISABLED。POSITION 明确返回 `UNSUPPORTED_MODE`，不新增换弹位置环。普通速度目标范围为 ±15000 rpm，YAW 为 ±16000 rpm；反馈有效范围仍为 ±7500 rpm，两者语义不同，目标不能用来伪造反馈。

`enable()` 不恢复目标；`submitCommand()` 的 OK 仅表示该次本地提交通过最终检查，不是未来持续授权或实际发送成功。重复同源、同模式目标更新保留积分/饱和历史，不能借连续提交规避故障检测。

ATOM-10 分两侧提交换弹目标时，任一侧拒绝或失败，必须调用 `stopMotor(3)` 或 `stopMotor(4)` 撤销整对目标并取消动作与计时。两个调用不是事务，也没有本模块内的动作计时器。正常到位必须提交两侧 VELOCITY=0；这是有效闭环目标，可能产生非零保持电流，不能用 DISABLED 代替。

`stopMotor()`/`disable()` 对换弹任一侧联动撤销两侧目标；安全覆盖不能被旧 PID 输出覆盖。软件急停装配层先锁住 CAN 零力矩阶段，再调用 `stopAll()`，解除后需要新的授权命令。恢复时只放行被许可的 `SAFETY_RECOVERY` 目标。

`IMotorFaultSink` 在零电流锁存尝试后接收 `onMotorFault()`；换弹根因还触发 `onReloadStopRequested()`，外部绑定到 ATOM-10 动作/计时取消。反馈、PID、输出故障保持锁存，后续有效反馈不自动清除。`clearFault()` 对换弹两侧一起检查新鲜反馈、输出可用和安全许可；通知排队或执行中返回 BUSY，清除后仍无旧目标。

故障桥应将原因映射至 ATOM-14/诊断模块的现有故障码，不凭枚举顺序臆造跨模块错误码。事件最多按通道首次锁存发布，持续相同故障不重复刷屏；桥应线程安全、有限时返回，允许查询或停通道，但禁止在回调中 start/stop/configure/updatePid/destroy core。

若事件 `zero_latched=false`，故障桥必须升级安全处置并禁止继续普通输出，不能解释成零力矩已经成功。物理零帧发送失败仍由 ATOM-04 的安全通知路径报告。

## 10. 调度与验证责任

core 自有一个 PID 工作线程；工作唤醒周期取全局 motor/PID 周期与各电机周期的最小值，各通道按自己的绝对期限更新。超期不追赶多次计算，超过最大控制间隔则停止对应回路。手工 `tick()`/`updateOnce()` 与工作线程共享同一控制锁和期限，不能无视周期强制多算一遍。

外部桥的执行时间、Linux 调度延迟、RX 锁等待和物理急停响应时限未通过文本检查得到证明。接入方仍须完成 ARM 增量编译、CTest、线程与关闭竞态、物理总线和设备联调；尤其核对整批输出提交、安全恢复门控、反馈缩放与超时边界。

本模块可复查的静态入口是 `Src/verify_static.ps1`，报告为 `STATIC_REVIEW.md`。该脚本只读源码并枚举独立数学/状态模型，绝不编译或执行 C++ 模块；有限模型不等于 C++ 程序的完整机器证明。

## 11. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-14 | 明确 API、生命周期两阶段、配置、PID 边界、CAN 桥、换弹联动、并发前提及静态验证局限。 |
