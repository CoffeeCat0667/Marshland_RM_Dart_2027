# 网关与公共接入验证

## ATOM-02 当前接口验证

2026-10-17：旧 state.* 用例针对已删除的 StateArbitrator/SystemInputs API，不能
作为当前接口验证。新增 StateMachineTests.cpp 覆盖控制源矩阵、CH5/CH7、x=1..10、
断连/重连、只在调用时更新、互斥边界、并发读写、限时信号等待和主线程关闭。

根 CMake 当前仍引用已删除的 Atoms/Public/Headers 目录及 marshland_app 目标。
本次不修复无关装配重构；使用下面的聚焦 Makefile，只增量编译相关 ARM 单元，
同时检查 HttpServer、control_loop 和运行时示例的接口兼容性，不运行 clean。

```bash
# Repository root, inside WSL; always uses an ARM-targeting compiler.
make -f Tests/StateMachineTests.mk -j 3
qemu-aarch64 -L /usr/aarch64-linux-gnu cmake-build-ubuntu-arm/atom02-check/state_machine_tests
qemu-aarch64 -L /usr/aarch64-linux-gnu cmake-build-ubuntu-arm/atom02-check/state_entry_tests
QEMU_AARCH64=qemu-aarch64 bash Tests/StateMachineExitSmoke.sh
```

重复 make 会复用对象和依赖文件，无变化时不再编译。可通过 QEMU_AARCH64 指定
本地模拟器路径；本次验证器下载解包在忽略的构建目录内，不安装或更改系统包。
退出冒烟分别发送 SIGINT/SIGTERM，检查主线程正常返回且所有已启动线程完成 join。
StateMachineEntryTests 直接运行真实 Entry 主循环，以模拟生产者修改同一份共享
数据，检查 SBUS CH5 切换、Web 接管、双离线 AUTO 及退出后清空全局访问器。
Entry 仍含原有设备/服务 TODO，冒烟不能代替真实 SBUS、HTTP 和硬件联调。

## 旧网关验证（历史接口）

## 构建

只在既有 ARM 构建目录增量配置/编译，不使用 x86 编译链，不执行 clean-first。

```bash
cmake -S . -B cmake-build-ubuntu-arm -DMARSHLAND_BUILD_TESTS=ON
cmake --build cmake-build-ubuntu-arm --target marshland marshland_gateway_tests marshland_public_contracts_check --parallel 4
```

既有缓存必须保持 aarch64 工具链。PublicContractsCompile 目标只获得 PublicHeaders 的路径，用来验证公共端口不依赖 ATOM/AppCoordinator 私有头。

## 运行

在 Ubuntu ARM 主机直接运行对应 ARM 产物；WSL 交叉构建需使用 ARM 模拟器与匹配的目标库根。

```bash
qemu-aarch64 -L /usr/aarch64-linux-gnu cmake-build-ubuntu-arm/marshland_gateway_tests <test-name>
```

可通过 CMake 缓存参数 MARSHLAND_QEMU_AARCH64 指向独立的 qemu-aarch64/static 二进制；检测到模拟器或在原生 ARM 主机上时，全部具名用例注册为 CTest 的 `marshland.*`。

## 验证边界

- CTest 注册 37 个具名用例，覆盖 `public.*`、`runtime.*`、`state.*` 和 `app.*` 四组：公共类型/时钟；Runtime 注册、排序、失败、异常、析构；仲裁策略、意图、急停、事件、四发流程；AppCoordinator 启动、闸门、端口、提交、读取视图、并发和事件。
- 这些用例覆盖注册信息、Runtime 排序/失败/异常/回滚/关闭、公共类型和策略表、字段输入、急停、事件、发射授权、自动流程、AppCoordinator 端口、标志提交临界区、时间戳、零拷贝读取、并发序号和 SIGINT/SIGTERM。
- 当前 ATOM-01 已提供 `registeredModuleCount()`，测试覆盖真实成功启动、空注册表、失败回滚和不可重启路径。
- ATOM02-BUG-018/019/020 由 `state.stop_clear_matrix`、`app.stop_flag_atomic_commit` 直接验证已实现策略；测试不在网关复制仲裁逻辑。
- 模拟器验证不代替实机硬件、部署 ABI 或 100 ms 急停响应压力验收；未接入 ATOM03-19 时不能宣称全机可运行。
