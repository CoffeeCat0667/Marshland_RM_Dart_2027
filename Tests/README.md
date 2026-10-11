# 网关与公共接入验证

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
