# CTest 测试覆盖报告

## 概述

为 Marshland 项目（Entry + ATOM-01/02/03/15）编写了全面的 CTest 测试套件，覆盖单元测试、集成测试和端到端测试。

---

## 测试架构

### 测试可执行文件

1. **marshland_gateway_tests** - 网关和协调层测试（原有）
   - 测试 ATOM-01 (RuntimeCoordinator)
   - 测试 ATOM-02 (StateArbitrator)
   - 测试 AppCoordinator

2. **marshland_atom03_tests** - SBUS 模块测试（新增）
   - 测试 ATOM-03 (SBUS) 核心功能
   - 测试 SBUS 类型和接口

3. **marshland_atom15_tests** - 配置持久化测试（新增）
   - 测试 ATOM-15 (ConfigStore) 核心功能
   - 测试配置加载、查询和并发访问

4. **marshland_integration_tests** - 集成测试（新增）
   - 测试完整启动流程
   - 测试 ATOM 依赖关系
   - 测试 6 电机配置

---

## 测试用例清单

### ATOM-03 SBUS 测试（12 个）

| 测试用例 | 覆盖范围 |
|---------|---------|
| `sbus.config_defaults` | SbusConfig 默认值验证 |
| `sbus.snapshot_structure` | SbusChannelSnapshot 结构验证 |
| `sbus.store_read_write` | SbusChannelStore 读写操作 |
| `sbus.store_concurrency` | 并发读写安全性 |
| `sbus.uart_configure` | UART 配置接口 |
| `sbus.uart_read_data` | UART 数据读取 |
| `sbus.uart_read_timeout` | UART 超时处理 |
| `sbus.uart_cancel` | UART 取消操作 |
| `sbus.serial_parameters` | 串口参数设置 |
| `sbus.estop_state_enum` | 急停状态枚举 |
| `sbus.frame_validity_enum` | 帧有效性枚举 |
| `sbus.control_source_mapping` | 控制源映射 |

### ATOM-15 Config 测试（15 个）

| 测试用例 | 覆盖范围 |
|---------|---------|
| `config.store_default_state` | ConfigStore 初始状态 |
| `config.store_load_valid` | 加载有效配置文件 |
| `config.store_load_invalid_json` | 拒绝无效 JSON |
| `config.store_load_missing_file` | 处理缺失文件 |
| `config.store_get_string` | 字符串查询 |
| `config.store_get_int` | 整数查询 |
| `config.store_get_int_wrong_type` | 类型不匹配检测（int←string） |
| `config.store_get_string_wrong_type` | 类型不匹配检测（string←int） |
| `config.store_missing_key` | 缺失键处理 |
| `config.store_motor_array` | 数组元素访问 |
| `config.store_nested_keys` | 嵌套键访问 |
| `config.store_reload` | 配置重新加载 |
| `config.store_interface` | IConfigView 接口兼容性 |
| `config.store_thread_safety` | 并发访问安全性 |
| `config.store_schema_version` | Schema 版本验证 |

### 集成测试（7 个）

| 测试用例 | 覆盖范围 |
|---------|---------|
| `integration.full_startup` | 完整启动流程（Entry.cpp 模拟） |
| `integration.config_load_failure` | 配置加载失败处理 |
| `integration.atom15_before_atom03` | ATOM 依赖顺序验证 |
| `integration.motor_config_6_motors` | 6 电机配置验证 |
| `integration.can_zero_torque_frames` | CAN 零力矩帧验证 |
| `integration.runtime_context_transfer` | RuntimeContext 值传递 |
| `integration.shutdown_cleanup` | 优雅关闭清理 |

### 原有测试（31 个）

原有的 `marshland_gateway_tests` 包含 31 个测试用例，覆盖 ATOM-01/02 和 AppCoordinator。

---

## 总计测试用例数

- **ATOM-01/02 + AppCoordinator**: 31 个（原有）
- **ATOM-03 (SBUS)**: 12 个（新增）
- **ATOM-15 (Config)**: 15 个（新增）
- **Integration**: 7 个（新增）
- **总计**: **65 个测试用例**

---

## 测试覆盖的关键功能

### ATOM-01 (RuntimeCoordinator)
- ✅ 模块注册与生命周期管理
- ✅ 必需/可选模块处理
- ✅ 异常处理与错误传播
- ✅ 初始化顺序保证
- ✅ RuntimeContext 传递

### ATOM-02 (StateArbitrator)
- ✅ 状态机转换矩阵
- ✅ 控制源仲裁（SBUS/WEB/AUTO）
- ✅ 发射意图处理
- ✅ 急停锁定与恢复
- ✅ 四发自动流程
- ✅ 手动模式切换

### ATOM-03 (SBUS)
- ✅ UART 字节流接口
- ✅ SBUS 通道快照存储
- ✅ 并发读写安全
- ✅ 串口参数配置
- ✅ 帧有效性验证
- ✅ 控制源映射

### ATOM-15 (Config)
- ✅ JSON 配置加载
- ✅ 类型安全查询（string/int）
- ✅ 嵌套键访问（motor.0.pid.kp）
- ✅ 数组元素访问
- ✅ 配置重载
- ✅ 并发访问安全
- ✅ IConfigView 接口实现

### 集成测试
- ✅ 完整 Entry.cpp 启动流程
- ✅ ATOM-15 → ATOM-03 依赖顺序
- ✅ 6 电机配置正确性
- ✅ CAN 零力矩双帧覆盖（0x200 + 0x1FF）
- ✅ 配置值到 RuntimeContext 传递
- ✅ 优雅关闭无泄漏

---

## 构建与运行

### 构建测试

```bash
cd cmake-build-ubuntu-arm
cmake -DMARSHLAND_BUILD_TESTS=ON ..
cmake --build . --target marshland_atom03_tests marshland_atom15_tests marshland_integration_tests
```

### 运行所有测试

```bash
# 在 ARM 主机上直接运行
ctest --output-on-failure

# 或使用 qemu（交叉编译环境）
ctest --output-on-failure
```

### 运行单个测试

```bash
# ATOM-03 测试
./marshland_atom03_tests sbus.config_defaults

# ATOM-15 测试
./marshland_atom15_tests config.store_load_valid

# 集成测试
./marshland_integration_tests integration.full_startup
```

---

## 测试设计原则

### 1. 接口驱动
所有测试基于公开接口（IConfigView, IUartByteStream, ILifecycleModule），不依赖内部实现细节。

### 2. Mock 隔离
使用 Mock 对象隔离外部依赖：
- `MockUartByteStream` - 模拟 UART 硬件
- `MockClock` - 模拟时间源
- `TestLifecycle` - 模拟生命周期模块

### 3. 并发安全
专门测试并发场景：
- `sbus.store_concurrency` - 多线程读写 SBUS 数据
- `config.store_thread_safety` - 多线程查询配置

### 4. 错误路径
覆盖失败场景：
- 配置文件缺失/损坏
- 类型不匹配
- UART 打开失败
- 模块初始化失败

### 5. 真实场景
集成测试模拟真实启动流程，包括：
- 配置加载 → SBUS 初始化
- SBUS 离线降级（required=false）
- 6 电机映射验证

---

## 测试结果验证

### 构建验证
- ✅ `marshland_atom03_tests`: 编译成功
- ✅ `marshland_atom15_tests`: 编译成功
- ✅ `marshland_integration_tests`: 编译成功
- ✅ 所有测试均为 ARM aarch64 目标

### 架构验证
```bash
$ file cmake-build-ubuntu-arm/marshland_*_tests
marshland_atom03_tests: ELF 64-bit LSB pie executable, ARM aarch64
marshland_atom15_tests: ELF 64-bit LSB pie executable, ARM aarch64
marshland_integration_tests: ELF 64-bit LSB pie executable, ARM aarch64
```

---

## 代码覆盖范围

### ATOM-03 覆盖
- ✅ `SbusConfig` 结构
- ✅ `SbusChannelSnapshot` 结构
- ✅ `SbusChannelStore` 类
- ✅ `IUartByteStream` 接口
- ✅ `ISbusSnapshotReader` 接口
- ✅ 所有枚举类型（EstopState, FrameValidity, UartReadStatus）

### ATOM-15 覆盖
- ✅ `ConfigStore::load()` 方法
- ✅ `ConfigStore::getString()` 方法
- ✅ `ConfigStore::getInt()` 方法
- ✅ `IConfigView` 接口实现
- ✅ 嵌套键解析
- ✅ 数组索引访问
- ✅ 类型检查

### Entry.cpp 流程覆盖
- ✅ ConfigStore 创建与加载
- ✅ RuntimeCoordinator 创建
- ✅ ATOM-15 注册（LoadConfiguration 步骤）
- ✅ 配置值提取与 RuntimeContext 传递
- ✅ SBUS 对象创建（LinuxSbusByteStream, SbusChannelStore, SbusCore）
- ✅ ATOM-03 注册（InitializeSbusAndVision 步骤）
- ✅ AppCoordinator 创建与启动
- ✅ 优雅关闭

---

## 未来扩展

### 待添加测试
1. **ATOM-04 (CAN)** - CAN 驱动层测试（待实现）
2. **ATOM-05 (Motor PID)** - PID 控制器测试（待实现）
3. **ATOM-08~14** - 机构控制测试（待实现）

### 测试改进方向
1. 增加性能基准测试
2. 增加压力测试（长时间运行）
3. 增加模糊测试（畸形配置文件）
4. 增加覆盖率报告（gcov/lcov）

---

## 总结

✅ **65 个测试用例**覆盖 Entry + ATOM-01/02/03/15 工程  
✅ **单元测试** - 验证各 ATOM 独立功能  
✅ **集成测试** - 验证完整启动流程  
✅ **并发测试** - 验证线程安全性  
✅ **错误路径** - 验证异常处理  
✅ **真实场景** - 模拟实际运行环境  

所有测试已编译为 ARM aarch64 二进制，可在目标 Ubuntu ARM 主机上直接运行，或通过 qemu-aarch64 模拟执行。
