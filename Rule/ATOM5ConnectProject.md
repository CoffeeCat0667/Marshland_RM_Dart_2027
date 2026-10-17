# ATOM-05_Motor_PID 工程接入说明

> 接口版本：2.0（破坏性调整，替代 1.0 全文）
> 日期：2026-10-16
> 模块：`Atoms/ATOM-05_Motor_PID`
> 命名空间：`marshland::atom05`
> 验证：仅源码走查、文本静态检查与有限状态模型；未编译、未运行 CTest/模拟器/硬件。

## 1. 接口边界

ATOM-05 对业务调用方只提供六个服务操作。电机状态、反馈缓存、配置映射、PID 历史、工作线程、锁和调度期限都在内部。业务输入通过构造函数建立，查询与修改结果按值返回；值对象的字段为私有，不提供 setter、可修改引用或内部存储指针。

这不意味着程序没有传输数据类型：命令和结果仍是公开契约，只是不公开可写内部状态。CAN 批次、安全许可、故障事件等属于顶层装配契约，放在单独的头文件，不由普通业务调用方处理。

ATOM-05 负责命令、反馈、速度 PID、受控参数更新和故障联动，不决定自动发射流程；不新增换弹位置控制、回零、限位判定，不推测设备错误码，不提供 `main()`。ATOM-02/14 权限、安全编排、ATOM-04 输出桥、ATOM-10 动作取消、ATOM-15 保存及顶层注册均由外部负责。

## 2. 头文件与 CMake

| 使用者 | include 路径 | 用途 |
|---|---|---|
| 业务 | `atom05_motor_pid/motor_core.hpp` | 六个服务操作与值对象 |
| 业务/装配 | `atom05_motor_pid/motor_types.hpp` | 命令、PID、查询及更新结果 |
| 装配 | `atom05_motor_pid/motor_interfaces.hpp` | 四个外部端口、依赖集合、桥传输对象 |
| 装配 | `atom05_motor_pid/lifecycle_adapter.hpp` | 两阶段生命周期适配器 |
| 装配 | `atom05_motor_pid/can_feedback_adapter.hpp` | ATOM-04 原始反馈投递适配器 |

物理目录：公开头位于 `Inc/Public/atom05_motor_pid/`，内部头位于 `Inc/Private/detail/`。上层只获得 `Inc/Public` 包含路径；`Inc/Private` 是本目标的 PRIVATE 路径，不允许消费者手工添加。`motor_core.hpp` 不包含内部头、锁、线程、CAN 或配置实现。

保留目标 `atom_05_motor_pid` 和别名 `Marshland::atom_05_motor_pid`。上层先建立 `Marshland::PublicHeaders`，再加入模块并链接别名：

```cmake
add_subdirectory(Atoms/ATOM-05_Motor_PID)
target_link_libraries(your_assembly_target PRIVATE Marshland::atom_05_motor_pid)
```

`your_assembly_target` 是上层现有装配目标的占位名称，不要求创建同名新目标。若整树已加入模块，不重复 add_subdirectory。模块依赖 PublicHeaders 和 Threads::Threads，不直接链接 ATOM-04/AppCoordinator。Ubuntu ARM 工具链、C++23、告警与最终可执行目标由上层决定；本模块不独立编译、不覆盖全局选项。

## 3. 六个业务 API

```cpp
MotorStatus submitCommand(const MotorCommand& command) noexcept;
MotorStatus stopMotor(MotorId motor_id) noexcept;
MotorStatus stopAll() noexcept;
PidUpdateResult updatePid(const PidUpdateRequest& request) noexcept;
MotorStatus clearFault(MotorId motor_id) noexcept;
MotorQueryResult queryMotor(MotorId motor_id) const noexcept;
```

`MotorId` 是 `std::uint8_t` 别名，当前合法电机为 1～6；不是数组下标。六个操作不包括构造、析构、值对象的只读访问器，也不包括装配端口。

| 操作 | 合同 |
|---|---|
| `submitCommand` | 校验目标/来源/限幅/反馈及安全许可，通过时激活目标；OK 只表示本次本地提交通过，不是物理发送成功或未来持续授权 |
| `stopMotor` | 撤销目标、PID 历史并尝试提交零输出；3/4 任一侧联动停止另一侧；输出提交失败返回 OUTPUT_FAILED |
| `stopAll` | 撤销全部目标并尝试提交零输出，返回状态，不再静默丢失失败；不关闭 PID 线程，未来新命令仍须重新授权 |
| `updatePid` | 校验 WEB+MANUAL/DEBUG 许可与参数，立即应用后尝试保存，分别返回结果 |
| `clearFault` | 只在反馈新鲜、输出可用且安全许可下清除；换弹两侧一起检查；不恢复旧目标或主动启用通道 |
| `queryMotor` | 在一次控制锁内取得一致副本，查询无副作用，不返回内部引用 |

未配置或生命周期已停止时，停止操作返回 NOT_READY，不再访问关闭后的输出桥。查询仍可在已配置、已停止的 core 上获得状态，此时 running() 为 false。

### 3.1 命令输入

```cpp
MotorCommand(MotorId id, MotorMode mode, double target,
             std::int32_t output_limit_ma, ControlSource source,
             CommandPurpose purpose = CommandPurpose::CONTROL,
             std::uint32_t request_id = 0);
```

只读访问器：`motorId()`、`mode()`、`target()`、`outputLimitMa()`、`source()`、`purpose()`、`requestId()`。构造只组装值，真正校验由服务执行；默认构造不是一个有效出力命令。

模式：VELOCITY 的目标单位为 rpm；CURRENT/TORQUE 的目标单位均为 mA，不实现牛顿米转换；POSITION 返回 UNSUPPORTED_MODE。3/4 仅接受 VELOCITY，停止用 stopMotor；兼容保留 DISABLED 模式时直接走同一停止路径。

普通速度目标为 ±15000 rpm，YAW 为 ±16000 rpm；命令限幅为 1～15000 mA。CURRENT/TORQUE 目标绝对值不能超过命令限幅。非有限目标拒绝。反馈有效范围仍为 ±7500 rpm，目标范围和反馈范围不能混淆。

purpose 仅为 CONTROL 或 SAFETY_RECOVERY。请求中的 source/purpose 不是可信授权，外部安全桥必须核对真实请求与系统状态。

### 3.2 查询返回值

MotorQueryResult 所有访问器均为 const，返回基础值、枚举或 PidConfig 副本：

| 访问器 | 说明 |
|---|---|
| `status()` | 查询本身状态：未配置 NOT_READY、ID 不存在 UNKNOWN_MOTOR、找到时 OK |
| `motorId()`、`running()` | 请求的电机 ID、模块工作线程是否运行 |
| `feedbackStatus()` | 当前反馈可用性：OK、FEEDBACK_INVALID、FEEDBACK_TIMEOUT 或 CLOCK_INVALID |
| `hasReceivedFeedback()` | 是否曾收到并接受过有效帧；区分首帧时间戳 0 与从未收到 |
| `speedRpm()`、`direction()` | 有符号转速、STOPPED/POSITIVE/NEGATIVE |
| `rotorAngle()`、`torqueCurrentRaw()`、`temperatureC()` | 设备反馈值；角度不是累积位置，电流保持设备原始值 |
| `feedbackTimeUs()`、`feedbackSequence()` | 最近接受帧的单调时间戳和序号 |
| `mode()`、`target()` | 当前接受的模式和目标 |
| `enabled()`、`targetActive()`、`fault()` | 通道启用、活动目标、锁存故障 |
| `outputMa()` | 当前内部控制输出；不表示真实电机电流或设备确认 |
| `pid()`、`pidRevision()` | 当前实际参数副本及参数版本 |

必须先检查 status()，再按用途检查 feedbackStatus()。查询成功不等于反馈有效或无故障。反馈无效/超时时，其数值只用于诊断，不可控制；0 rpm 是合法反馈，不能用作查询失败标志。结果保存后不会自动刷新，旧结果也不能修改 core。queryMotor 没有节点映射、饱和计数、调度期限或内部通知标记的访问器。

```cpp
const auto result = core.queryMotor(1);
if (result.status() == MotorStatus::OK &&
    result.feedbackStatus() == MotorStatus::OK) {
    const auto rpm = result.speedRpm();
    const auto pid = result.pid();
    (void)rpm;
    (void)pid;
}
```

## 4. PID 参数与返回语义

```cpp
PidConfig(std::int32_t kp, std::int32_t ki, std::int32_t kd,
          std::int32_t integral_limit, std::int32_t output_limit,
          std::uint32_t period_ms, bool enabled);
PidUpdateRequest(MotorId id, const PidConfig& parameters,
                 ControlSource source, std::uint32_t request_id = 0);
```

PidConfig 访问器为 `kp()`、`ki()`、`kd()`、`integralLimit()`、`outputLimit()`、`controlPeriodMs()`、`enabled()`。PidUpdateRequest 访问器为 `motorId()`、`parameters()`、`source()`、`requestId()`；parameters() 按值返回。

| 参数 | 约束 |
|---|---|
| kp/ki/kd | 整数 -1000～1000，允许负值 |
| integral_limit | 0～16000，表示积分项输出贡献的绝对限幅；0 禁止积分贡献 |
| output_limit | 1～16000；实际输出另受 15000 mA、命令和许可限幅约束 |
| period_ms | 1～100，且不超过已加载的系统最大控制周期 |
| enabled | bool |

现有 `PidConfig(3, 0, 0, 16000, 16000, 20, true)` 合法。非法参数拒绝而非自动钳位。默认构造的 PidConfig 是未配置值，不是安全隐含默认配置。输入边界不保证闭环稳定性，外部维护者须同步设计/Schema 中的边界。

```cpp
const PidConfig pid(3, 0, 0, 16000, 16000, 20, true);
const auto result = core.updatePid(PidUpdateRequest(1, pid, ControlSource::WEB, 42));
const bool live = result.applied();
const bool saved = result.persistence() == PersistenceStatus::SAVED;
(void)live;
(void)saved;
```

PidUpdateResult 只提供 `status()`、`applied()`、`persistence()`、`revision()`：applied=false 时未保存；applied=true 表示运行参数已经应用，即使 status 报告后续输出故障也不抹去应用事实。SAVED 只能由后端明确返回；FAILED/UNAVAILABLE 不回滚热加载，也不能展示为已保存。

活动速度通道在更新锁内清历史并立即重算；更新不解除故障，不恢复停用目标。禁用换弹任一侧 PID 会停止双侧。相同来源/模式的连续目标更新保留 PID 历史，不能通过反复提交规避连续饱和检测。

内部 PID 采用秒为时间单位，限制积分贡献，并抑制进一步推入饱和的积分增量；首周期无差分。连续 5 个饱和周期且误差不改善时锁存故障；改善则重新计数，未饱和清零。相关算法与验证函数不是公共 API。

## 5. 装配与生命周期

顶层包含三个装配头，提供四个依赖端口和单调时钟，然后创建：

```cpp
const MotorDependencies dependencies{clock, safety_gate, output_bridge,
                                     fault_sink, pid_store};
MotorCore core(dependencies);
CanFeedbackAdapter feedback(core);
LifecycleAdapter pid_stage(core, config_view, LifecycleStage::LOAD_PID);
LifecycleAdapter channels(core, config_view, LifecycleStage::INITIALIZE_CHANNELS);
```

以上示例需要相应 using 声明；clock 等对象由外部实现。MotorCore 构造时分配一次私有实现，构造可能抛出分配异常；后续六个业务操作均 noexcept。core 不可复制或移动，适配器和所有依赖必须保持稳定寿命。

输出桥可先构造为未绑定状态，再创建 core、feedback 和 ATOM-04，最后绑定输出桥到 ATOM-04。将 feedback（不是 core）传给 ATOM-04 的 ICanFrameSink 依赖。

| 生命周期对象 | 外部注册步骤 | 行为 |
|---|---|---|
| pid_stage | RuntimeStep::LoadPidParameters | 从 IConfigView 校验完整配置并提交，不启动线程 |
| ATOM-04 适配器 | RuntimeStep::InitializeSocketCan | 外部打开 CAN 并开始反馈投递 |
| channels | RuntimeStep::InitializeMotorAndEncoderChannels | 复核配置版本、输出桥就绪，先零输出再启动 PID 线程 |

LifecycleAdapter 只公开 initialize(context)、shutdown()、name()；不自行注册。上层选择 required、检查注册返回值并读取失败原因。建议两个阶段作为必需步骤，最终由启动策略决定。通道初始化成功不等于节点已反馈；动作前先查询有效性，无反馈时提交命令会拒绝并锁存反馈故障。

关闭顺序：停止接收新业务请求，等待进行中的保存/调用结束，先 shutdown ATOM-05，再停止 ATOM-04 投递，最后销毁适配器/core/桥。两个阶段都可调用 shutdown，重复关闭不重新提交输出。销毁前必须没有并发调用，所有依赖须比 core 存活更久。业务 stopAll 不是生命周期 shutdown。

## 6. 四个外部端口

四个接口及其结构体仅在 motor_interfaces.hpp 中公开；它们是桥传输合同，不是可访问的内部状态。安全许可和输出批次等仍使用结构化数据，不能据此修改通道内存。

### 6.1 IMotorSafetyGate

checkCommand(command)、checkOutput(command) 返回 `MotorPermit{allowed, output_limit_ma}`；checkPidEdit(request) 返回 `PidEditPermit{allowed, source, mode}`；allowFaultReset(id) 返回 bool。

联合 ATOM-02 控制权和 ATOM-14 安全策略判断；许可限幅必须为 1～15000。PID 必须确认真实 WEB 来源、MANUAL/DEBUG 模式。每次输出重新检查许可。检查有界且不回调 core；不得在持有状态读租约时调用会再次取状态锁的端口。

### 6.2 IMotorCurrentOutput

ready() 查询后端就绪，commit(batch) 提交两组电流计数，forceZero() 不依赖代次地清两组锁存。接口不执行物理发送或阻塞 I/O，不回调 core。

MotorOutputBatch 包含 currents[2][4]、active_mask、generation。第一组对应 0x200/node 0x201～0x204，第二组对应 0x1FF/node 0x205～0x206，未用槽位零；mask bit 0～5 对应 node 0x201～0x206。数据已经是 C620 命令计数：`truncate(mA*16384/20000)`；±15000 mA 为 ±12288，桥不得再次缩放。

bridge 将批次转给 ATOM-04 的 latchMotorCurrents() 等锁存能力。成功只表示锁存，不能声称实际发送或设备确认；forceZero() 同样只报告锁存状态。

外部必须拒绝重复/过期 generation，并协调整批写入与 TX/安全覆盖：两次独立 latchMotorCurrents() 不自动构成跨组原子提交。若现有 ATOM-04 不支持提交屏障或等价机制，这是外部接入阻塞项，不能虚报满足。任一组失败应清空两组并返回 false；活动批次因安全覆盖被拒时也必须返回 false。

默认零力矩，安全锁存优先。安全覆盖先锁 CAN 零力矩再撤销 core 目标，解除后不能自动恢复旧目标；禁止零帧和控制帧周期性交替覆盖。forceZero() 不重置已接受代次。更换 core 实例前先停旧实例和 TX、清零并重新绑定代次，不能把新实例从 1 开始的批次混入旧流。

### 6.3 IMotorFaultSink

onMotorFault(event) 接收 FaultEvent 的 motor_id、reload_peer_id、fault、timestamp_us、zero_latched；onReloadStopRequested(failed_id, peer_id) 先请求 ATOM-10 取消换弹动作与计时。事件在零锁存尝试之后、控制锁之外发布。

zero_latched=false 时必须升级安全处置，禁止继续普通输出；true 也不是物理零帧发送成功。后者由 ATOM-04 发送故障/安全通知负责。外部映射既有故障码，不按枚举序号猜测。

回调须线程安全、有界，允许 queryMotor/stopMotor/stopAll；禁止在回调中生命周期操作、updatePid 或销毁 core。通知排队或执行中 clearFault 返回 BUSY；后续有效反馈不自动清故障。

### 6.4 IPidPersistence

saveMotorPid(id, revision, parameters, location) 返回 PersistenceStatus；location 包含固定容量 directory/file_name。读取参数改用 parameters.kp() 等只读方法。

后端按电机 ID 更新其 PID、保留其他 JSON 字段，协调其他配置写入者；完成真实保存才返回 SAVED。运行于请求线程、控制锁外，同一 core 的编辑和保存串行，不阻塞 PID 线程。不得回调生命周期/updatePid。HTTP/日志负责人保留 requestId() 并分别记录应用和保存结果。

## 7. 配置与反馈约定

配置只经 IConfigView 读取，不自行解析 JSON。配置加载期间须冻结发布或持有有效读租约；结束时检查 configVersion 不能代替裸字符串寿命保护。两个初始化阶段之间版本变化拒绝启动。完整加载失败不部分提交。

| 配置键 | 必需值/范围 |
|---|---|
| timing.max_control_cycle_ms | 整数 1～100 |
| timing.motor_loop_period_ms、timing.pid_period_ms | 正整数，不超过最大控制周期 |
| timing.can_comm_timeout_ms | 整数 1～60000；部署基线 200 ms，缺失拒绝，无隐含回退 |
| persistence.directory、persistence.file_name | 非空、无内嵌 NUL；UTF-8 字节长度分别小于 256、128 |
| motor | 恰好六个唯一映射，可重排数组，不可用数组位置当身份 |
| motor[].bus/control_mode/encoder_source/target_unit | CAN0 / CLOSED_LOOP_PID / CAN / rpm |
| motor[].pid.* | kp、ki、kd、integral_limit、output_limit、control_period_ms、enabled，见第 4 节 |

| ID | node | role |
|---|---|---|
| 1 | 0x201 | charge_right |
| 2 | 0x202 | charge_left |
| 3 | 0x203 | reload_right |
| 4 | 0x204 | reload_left |
| 5 | 0x205 | yaw |
| 6 | 0x206 | force |

node 字符串为 0x 加三个十六进制数字；CAN0 是配置逻辑名，Linux 实际设备打开名由 ATOM-04 负责。getIntAt("motor", index, "pid.kp", value) 等嵌套查询须由配置视图支持。

反馈按 C620 DLC=8 大端解析：0～1 角度（0～8191），2～3 有符号 rpm，4～5 设备电流原始值，6 温度，7 保留。角度不累积；保留字节不猜测错误码。abs(rpm)>7500、温度>=75℃ 或结构错误无效。物理设备型号与缩放仍须联调确认。

时间戳与注入时钟共享单调纪元；未来帧拒绝，重复/旧帧不更新。首帧时间戳 0 合法。`now-last_valid>=timeout` 时超时。RX 只更新固定容量数据并唤醒工作线程，不回调 CAN/故障端口或保存文件。已观察异常保持待处理状态，不能被随后有效帧静默覆盖。

## 8. 换弹与安全调用规则

两侧正常运行使用 VELOCITY 目标；正常结束提交两侧 VELOCITY=0，仍是有效闭环，可能需要非零保持电流。不要用 stopMotor 或 DISABLED 代替正常 0 rpm 到位目标。

ATOM-10 分两次提交时没有跨调用事务；任一侧命令失败，外部必须 stopMotor(3) 或 stopMotor(4) 撤销整对目标，并取消动作/计时。反馈或 PID 等根因故障在本模块内联动双侧撤销和锁存，由故障桥取消动作。

stopMotor/stopAll 不保留可自动恢复的旧目标。故障清除需要双侧新鲜反馈和安全许可，清除后仍停止。软件急停由外部先关闭出力门控再 stopAll；恢复阶段只允许经过授权的 SAFETY_RECOVERY 新命令。若输出锁存或物理零帧失败，外部安全模块负责更高级处置。

## 9. 从 1.0 迁移

本版不保留旧字段或旧业务方法的兼容入口。

| 旧用法 | 新用法 |
|---|---|
| MotorCore::Dependencies | motor_interfaces.hpp 中的 MotorDependencies |
| 把 core 作为 ICanFrameSink | 创建 CanFeedbackAdapter feedback(core)，绑定 feedback |
| snapshot(id) / MotorSnapshot 公开字段 | queryMotor(id) / MotorQueryResult 只读访问器 |
| readPidConfig(id, out) | queryMotor(id) 成功后调用 pid() |
| config.kp、command.motor_id 等公开字段 | 构造值对象；读取用 kp()、motorId() 等 |
| result.applied、result.persistence | result.applied()、result.persistence() |
| enable(id, source) | 直接提交获准的新命令，由服务激活；不恢复旧目标 |
| disable(id) | stopMotor(id) |
| void stopAll() | 检查 MotorStatus stopAll() 的返回值 |
| configure/start/stop/configurationVersion | 通过两个 LifecycleAdapter 阶段，不直接调用 core |
| tick/updateOnce/PidController/解码/配置加载/电流转换 | 内部实现，不再由消费者调用或包含 |
| 手工添加模块 Inc 目录 | 链接模块目标，使用传递的 Inc/Public；不添加 Inc/Private |

外部桥须同步将 PidConfig、MotorCommand、PidUpdateRequest 的字段访问改为只读方法。四个端口的职责和桥结构体字段保持原有合同；不要把装配头塞回业务头。

## 10. 验证与剩余责任

Src/verify_static.ps1 检查恰好六个服务操作、业务值对象无公开字段或借用返回、公开头不依赖内部 include 路径、方法定义、CMake 边界及控制保护条件，并枚举数值/权限/饱和/双侧状态模型。STATIC_REVIEW.md 记录本版源码走查和模型范围。

本轮不编译 C++，不求值 static_assert，不配置 CMake，不执行 CTest、模拟器或硬件。静态模型不是源码等价证明，文本检查也不是类型或链接验证。

接入负责人仍须完成 Ubuntu ARM 增量编译、旧调用迁移、整批 CAN 提交/安全覆盖、字符串读租约、线程/关闭竞态、真实故障与保存后端、时序及设备闭环联调；未满足这些前提，不能宣称整机可安全运行。
