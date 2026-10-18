# ATOM-03 三文件实现

- 日期：2026-10-17。
- 用户确定的唯一功能：串口读到 SBUS -> 解码 -> 转换到已确定范围 -> 写入公共结构体。
- 本次只改本模块、公共 Sbus.hpp 与对应设计说明；不迁移外部代码。

## 1. 最终代码文件

1. Inc/atom03_SBUS/sbus_private.hpp：唯一私有头，内部环形缓冲和解码声明。
2. Src/sbus.cpp：唯一实现，串口配置与读取、组帧、解码、映射、数据更新。
3. ../Public/Sbus.hpp：唯一公共头，SbusData 和 ReceiveSbus 声明。

CMakeLists.txt 为工程构建描述，不增加任何代码管理层。私有 Inc 不向消费者导出。

## 2. 删除范围

- 删除 sbus_core、sbus_config、linux_sbus_byte_stream、lifecycle_adapter 的全部独立头和源文件。
- 不保留 SbusCore、LinuxSbusByteStream、UART 抽象、生命周期接口、配置视图或线程管理器。
- 不保留 start/stop/cancel 对象、后台线程、控制权解释、急停解释、发射确认、授权、序号、时间戳或失联降级。
- 保留的停止标志仅用于退出阻塞接收循环，文件描述符在函数返回时关闭。

## 3. 公共调用

```cpp
bool ReceiveSbus(const char* serial_device, SbusData& data,
                 const std::atomic<bool>& stop_requested,
                 std::uint8_t channel_scale_x, std::uint16_t frame_bytes,
                 std::uint16_t channel_center, std::uint16_t channel_minimum,
                 std::uint16_t channel_maximum,
                 std::uint32_t read_timeout_ms = 20U) noexcept;
```

- 设备名和全部必要的数值参数直接传入，不读取 IConfigView 或 RuntimeContext。
- 函数阻塞运行，线程由调用方提供；调用方负责数据和停止标志的存活期。
- stop_requested=true 后在 poll 返回或当前数据批次结束后退出；最坏等待还受超时、锁等待和调度影响，不承诺立即 cancel。
- 返回 true 仅表示正常请求停止；打开失败、配置失败、参数错误和永久 I/O 错误返回 false。
- 固定 100000 baud、8E2；不进行软件反转。

## 4. 数据和映射

- 公共 SbusData 只保留 mutex、16 路 scaled_channels 和 online，不公开原始通道。
- 只解析 CH1～CH15 的 11-bit 原始值，保存在接收函数局部数组中；CH16 完全丢弃，公共 scaled_channels[15] 每次有效帧固定写 0。帧头 0x0F，不校验帧尾，不解析 Byte 23，不缩短传入帧长。
- 帧长由参数决定，半帧保存在有限环形缓冲区；没有额外帧副本。
- 通用输出 `(raw-center)*x`，x 为 uint8_t、1～10。
- 推荐 364/1024/1684 输出 -660*x/0/+660*x；CH12 独立输出 -2000/0/+2000 rpm。
- CH7 发射输入位于 scaled_channels[6]，CH13 供弹舵机输入位于 scaled_channels[12]；二者独立使用通用缩放，无持续帧数确认；仅 CH14～CH16 保留。
- 不固定中点或有效范围，只检查最短载荷和运算边界，避免越界/除零。
- 有效整帧加锁一次更新全部映射通道、刷新局部有效帧时间并置 online=true；只有无有效帧满 200 ms 才置 false，坏帧/失步/读取超时/错误/停止均不能提前清离线，保留上次数组。
- 读者接收 const SbusData&，持有其 mutex 同时复制 scaled_channels 和 online，不持锁等待接收函数结束；离线时不能使用旧数组作为新输入。
- online 由本接收线程内的单调时钟期限判定，不增加看门狗线程或执行授权。ReceiveSbus 参数和返回含义不变；若在线时退出，先关闭设备并等完剩余 200 ms 期限后再返回。

## 5. 验证范围

- 清点模块恰好一个私有头、一个 cpp、一个公共 Sbus.hpp。
- ARM 严格警告语法检查、Public-only 调用示例检查。
- 现有 WSL ARM 构建目录中增量构建 atom_03_sbus，不构建最终程序。
- 环形回绕、半帧、多帧、配置帧长、通道边界及 CH12 映射检查。
- git diff --check；确认旧文件和管理层依赖已经移除。
- 未进行真实串口测试，不声明外部装配兼容。

## 6. 三文件重构时的验证记录

- 精确文件检查通过：一个 47 行私有头、一个 149 行 cpp、一个 34 行公共 Sbus.hpp，共 230 行代码。
- 唯一 cpp 通过 ARM 编译器 `-fsyntax-only -Wall -Wextra -Werror` 检查。
- 只包含 Sbus.hpp 的接收/消费示例通过 ARM 语法检查；非系统头依赖只有 Sbus.hpp 本身。
- 独立算法模型验证 1024 帧、16384 个通道和 512 个回绕帧；覆盖分块读取、不同帧长、x=1/10、最短载荷访问及对称/非对称 CH12 范围。不是生产函数运行测试。
- 模块增量构建未通过：上层 Atoms/CMakeLists.txt 引用了当前不存在的 ATOM-04_CAN 和 ATOM-05_Motor_PID，CMake 配置阶段中止，尚未进入本模块编译。
- 未修改这些外部模块、未进行全量重编译，也未用其他工程掩盖上层配置失败。
- 范围限定 git diff --check 通过；真实串口、停止等待和目标机运行仍未测试。

## 7. 历史：公共数据再次收缩

- 用户已明确授权：SbusData 只保留 scaled_channels 和 mutex。
- 删除 raw_channels、online、setOffline 和所有公共状态更新；不新增状态替代它们。
- 保持一私有头、一 cpp、一公共 Sbus.hpp；串口、解码、缩放、锁保护和接收函数返回契约不变。
- 本次 ARM 增量构建通过：`[100%] Built target atom_03_sbus`，只构建模块，没有进行全量重编译或最终程序构建。
- ARM 严格语法检查通过：`-fsyntax-only -Wall -Wextra -Werror`。
- Public-only 编译期契约检查通过：结构化绑定确认恰好两个成员；raw_channels/online 不存在；const SbusData& 可以持锁复制；ReceiveSbus 参数与返回类型保持不变。
- 删除符号检查、三文件边界检查和范围限定 git diff --check 通过。
- 映射与组帧算法本轮未改动；未进行真实串口或多线程压力运行测试，也未迁移外部代码。

## 8. 历史：按用户决定恢复 online（清离线规则已由第 12 节替代）

- 公共数据字段固定为 mutex、scaled_channels、online；raw_channels 仍只保存在接收局部数组中。
- 恢复 setOffline 内部辅助函数；有效帧的数组和 online 在同一把锁内更新，所有异常/退出只清 online、不清数组。
- 不增加文件、生命周期或统计功能，不修改接收函数签名和数值映射。
- 本次 ARM 模块增量构建通过：`[100%] Built target atom_03_sbus`，未进行全量重编译或最终程序构建。
- ARM 严格语法与 Public-only 契约检查通过：恰好 mutex/scaled_channels/online 三个成员，不公开 raw_channels，const 消费者可持锁同时复制数组和在线状态。
- ReceiveSbus 签名、三文件边界及范围限定 git diff --check 检查通过；未进行真实串口或并发压力运行测试。

## 9. CH7/CH13 统一缩放

- 用户确认废弃旧 CH7 确认规则，CH7/CH13 均直接发布 `(raw-center)*x`；不添加计数器、动作执行或新的公共字段。
- 解码循环已覆盖这两个通道且算法相同，补充源码和公共头用途说明，不增加重复分支。
- 同步 Config、Interface、ConfigSchema、机器 Schema 和 ATOM3 接入说明；CH12 专用映射保持不变，保留通道改为 CH14～CH16。
- 文档版本为 Config 1.6、Interface 1.9、ConfigSchema 1.11、ATOM3ConnectProject 1.4；保留原有其他版本记录，不回退已有修订。
- ARM 模块增量构建与严格语法检查通过；Public-only 消费示例确认公共结构仍只有 mutex/scaled_channels/online，可持锁读取 CH7/CH13。
- 机器 Schema 的 11 个正反例通过，验证 CH7/CH13 正确绑定、错绑/空值/漏填/重复错误绑定拒绝；未增加必填功能条目，原有 SWITCH/JOYSTICK 输入标签保持兼容。
- 独立算法模型验证 720 帧、2160 个 CH7/CH13/CH12 结果及 360 个回绕帧；覆盖 x=1～10、两种标定、两个帧长和 CH7/CH13 独立的低/中/高组合。此项是模型检查，不是生产函数或真实串口运行测试。
- 未增加源码文件、计数器、执行器调用或公共字段；未迁移其他模块与运行配置文件。

## 10. 历史：online 现场诊断日志

- 用户报告遥控器连接前后 online 均为 false，尚无真实串口证据，不能判定唯一根因。
- 通过 Public/Log.hpp 的 marshland::log::Log() 发布 [SBUS] 日志，不调用 Init，不添加公开接口和数据字段，不改变 ReceiveSbus 签名。
- 启动 INFO 日志包含 device/data 地址/100000 8E2/x/frame_bytes/center/valid/timeout；错误 ERROR 日志包含失败系统调用及 errno，或 poll revents。
- 首批 UART 字节通过 INFO 记录最多 32 字节 hex；之后不逐次 dump 或逐帧记录。
- WARN 记录超时和丢弃的半帧长度、失步丢弃的字节、首个越界通道及 raw/range；DEBUG 记录等待半帧；INFO 记录从离线恢复为在线的有效帧与函数退出。
- 重复事件使用本函数局部 steady_clock 限为每原因每秒最多一条；日志在公共数据锁之外调用，无公共统计信息或日志管理器。
- 解码器只增加内部 rejected_channel 输出，原始值检查、数值映射和 online 事件语义保持不变；公有头不改动，仍只有三个代码文件。
- 根据日志定位：没有 start=函数未进入/阈值或日志链路问题；open/ioctl 错误=设备/权限/串口能力；只有 timeout=无输入；partial 后丢弃=长度或分包/超时；frame rejected=越界通道或误同步；accepted 后 sync/timeout=在线状态被后续事件清除。
- 所有真实设备结论需目标机启动后的 [SBUS] 日志确认，不以静态检查或模型代替现场测试。
- 验证：ARM atom_03_sbus 和最终 marshland 增量构建均通过，确认公共 Log() 的实现已被最终程序链接；构建保留 HTTP 模块 getaddrinfo 的静态链接告警。
- ARM 严格语法、Public-only 接口契约、日志覆盖、三文件边界和范围限定 git diff --check 均通过。未运行真实串口或日志捕获测试，不声称已修复 online 现场问题。

## 11. 历史：丢弃 CH16

- 用户明确不需要 CH16，直接从解码循环与范围检查中排除；CH16 的任何输入均不能导致拒帧，也不参与 online 判定。
- 公共数组仍保留 16 元素和原下标，scaled_channels[15] 发布为 0；raw[15] 局部占位也清零，防止残留值。
- 启动与有效帧日志明确 CH16=ignored，不再把占位 raw[15] 当作实际输入输出日志。
- CH1～CH15 的校验、通用缩放、CH12 专用映射、帧长和超时语义不变；保持三个代码文件和 ReceiveSbus 签名。
- 本次不修改调用方传入的 minimum/maximum。原范围若仍为 364～1684，日志样本中的 CH5/6/7=352 仍会被拒绝；需要接入方传入认可的 350～1700 才能放行这些数值，不能将本次 CH16 修改宣称为完整现场修复。
- 验证：ARM 模块和最终 marshland 增量构建通过，严格语法与 Public-only 布局/函数签名检查通过；完整帧的最小长度约束保持不变。
- 独立模型遍历 CH16 的全部 2048 种 11-bit 输入，CH1～CH15 原始/映射结果不变，CH16 占位为 0；原范围仍拒绝 CH5=352，CH15 的越界值仍会拒帧。此为模型检查，不是实际生产函数或串口运行测试。
- 三文件边界、诊断字段和范围限定 git diff --check 通过；未修改外部调用方或运行参数。

## 12. 200 ms 为唯一离线来源

- 用户明确授权：200 ms 连续无有效帧才置 online=false，其他原因不得直接清离线；CH3 原因暂不处理，任一路被检查的通道越界仍按完整帧长丢弃。
- 删除 setOffline 和各异常分支的直接清离线，统一由 expireOnline 检查 steady_clock 最近有效帧期限；源码只有这一处运行时 false 写入。
- 仅有效整帧能刷新计时，并与数组/online=true 在同一把锁内提交；无效数据、丢字节、EINTR/EAGAIN、单次 poll 超时均不刷新计时。
- poll 等待取传入读取超时与剩余有效帧期限的较小值，避免大读取超时推迟失联检查；单次 poll 超时保留半帧，不清 ring。
- 无效候选帧仍消费 frame_bytes，不改变为滑动重同步；不改通道范围、CH7/CH13 缩放、CH12 映射或 CH16 忽略规则。
- 退出时先关闭串口，如仍在线就在当前线程等待剩余期限，再调用同一个超时判定清离线；不能既立即退出，又让无人维护的 online 永久为 true。没有新增线程或管理器。
- 公共默认 online=false 是初始化状态，不是异常强制清离线。后续重启调用不在入口重置状态；应保证一个数据对象只有一个接收写入者。
- 公共字段、ReceiveSbus 签名、三个代码文件和原 bool 返回含义不变；增加的有效帧时间只保存在调用局部。
- 日志区分 poll 超时与真正有效帧失联，只有超时判定转换输出唯一离线原因，其他事件只说明未刷新有效帧。
- ARM 模块与最终 marshland 增量构建通过；仍有 HTTP 模块原有的 getaddrinfo 静态链接告警，未改动外部代码。
- 严格 ARM 语法和编译期检查通过：生产常量为 200 ms、199999 us 不到期、200000 us 到期、剩余不足 1 ms 的 poll 等待向上取整；公共布局/签名和 CH16 忽略规则不变。
- 确定性时间线模型覆盖 200 ms 前各类异常、200 ms 边界、恢复重计时、持续坏帧、24 ms 有效帧间穿插丢字节与 20 ms poll 超时、1 ms 至 INT_MAX 的配置等待和停止/错误退出期限，均通过；该项是模型，不是生产串口运行测试。
- 源码检查确认唯一运行时 data.online=false 写入受 age>=200 ms 保护；只有初始化和有效帧提交两处局部时间赋值，越界仍完整 frame_bytes 消费，poll 超时不再清 ring。
- 三文件边界、Public 契约与范围限定 git diff --check 通过；目标机真实串口及并发压力运行尚未验证。
