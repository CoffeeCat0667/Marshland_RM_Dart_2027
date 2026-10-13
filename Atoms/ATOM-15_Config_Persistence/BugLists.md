# ATOM-15_Config_Persistence 缺陷清单

- 更新日期：2026-10-13
- 设计基线：PRD 0.10、PEF 1.5、Interface 1.6、Config 0.9
- 适用模块：`Atoms/ATOM-15_Config_Persistence`
- 维护约定：只登记未完成事项；ATOM-03 的串口线程和控制链问题不在本清单重复登记。

## 1. 未修复缺陷

| 编号 | 标题 | 严重度 | 状态 |
|---|---|---|---|
| ATOM15-BUG-001 | 手写校验未覆盖完整 Config Schema，非法配置可能加载成功 | P1 | 未修复 |
| ATOM15-BUG-002 | JSON 字符串不支持 `\\uXXXX` 等完整标准转义 | P2 | 未修复 |
| ATOM15-BUG-003 | JSON 序列化未转义控制字符，热加载/持久化可能生成非法 JSON | P1 | 未修复 |
| ATOM15-BUG-004 | 运行时更新对大部分配置键缺少完整值域/枚举校验 | P1 | 未修复 |

## 2. 缺陷详述

### ATOM15-BUG-001：配置校验不是完整 Schema 校验

**位置**：`Src/config_store.cpp:72-83`，对照 `Rule/ConfigSchema.schema.json`。

当前实现仅检查部分顶层结构、电机 PID 基本字段、SBUS 部分字段、两个 timing 字段和 persistence 数量。没有完整校验：

- 所有 Config 域的必需字段；
- 枚举值和常量值；
- 数组元素结构和数量；
- `additionalProperties`；
- GPIO、Servo、CAN、Vision、HTTP、WebSocket、Fault、Thread、Log 等大量配置域；
- 字段之间的语义关系。

`Rule/ConfigSchema.schema.json` 定义了多个 `additionalProperties: false` 和更完整的字段约束，但 ATOM-15 没有加载或执行该 Schema。

**风险**：非法配置可能得到 `success=true`，随后由下游模块使用未校验值驱动硬件或服务。

**修复要求**：

1. 接入批准的 JSON Schema 2020-12 校验器，或逐项补齐与 Schema 等价的校验；
2. 明确未知键策略：如果采用 Schema 的 `additionalProperties:false`，未知键必须失败；如果产品要求向前兼容，则同步修改 Schema 和规范，统一为警告；
3. 每个失败项保留键名、原因和字段路径；
4. `ConfigStore::load()` 只有完整校验成功后才能替换现有快照。

### ATOM15-BUG-002：JSON 字符串转义不完整

**位置**：`Src/config_store.cpp:47-50`。

当前解析器支持 `\\"`、`\\\\`、`\\/`、`\\b`、`\\f`、`\\n`、`\\r`、`\\t`，但不支持标准 JSON 的 `\\uXXXX`，也没有处理 UTF-16 surrogate pair。

**风险**：合法 JSON 配置字符串可能被错误拒绝，尤其是包含 Unicode 转义的路径、描述或扩展字段。

**修复要求**：

- 支持 `\\uXXXX`；
- 正确合并 surrogate pair；
- 对非法 Unicode 转义返回带路径的解析诊断；
- 增加 ASCII、中文、控制字符和 Unicode 转义的解析测试。

### ATOM15-BUG-003：序列化未转义控制字符

**位置**：`Src/config_store.cpp:85-86`。

序列化字符串只处理双引号和反斜杠，没有将换行、制表、回车、退格、换页及其他小于 `0x20` 的控制字符编码为 JSON 转义。

例如输入 JSON 中的 `"line1\\nline2"` 被解析为包含真实换行的字符串，后续 `updateRuntime()` 或 PID/预留变量持久化时，序列化可能写出裸换行，从而生成非法 JSON。

**修复要求**：

- 序列化时完整转义 JSON 控制字符；
- 对非 ASCII 字符明确采用 UTF-8 原样输出或 `\\uXXXX` 输出策略；
- 增加 load → update → persist → reload 的 round-trip 测试；
- 持久化失败时不能留下不可解析的半文件，至少要明确记录文件损坏风险。

### ATOM15-BUG-004：运行时更新校验不完整

**位置**：`Src/config_store.cpp:71` 和 `updateRuntime()`。

`validUpdate()` 只对少数键执行额外范围检查：

- `sbus.channel_scale_x`；
- 两个 SBUS timing 键；
- `persistence.reserved_variable_count`。

其他已知键通常只检查当前 JSON 值与新值的基础类型兼容，不重新执行 Config/Schema 的范围、枚举、常量和语义约束。

**风险**：运行时可能写入负周期、非法端口、超限 PID、非法枚举或其他不符合部署约束的值。

**修复要求**：

1. 运行时更新复用同一字段校验器，不维护第二套范围规则；
2. 校验类型、范围、枚举、const、数组元素和跨字段关系；
3. 校验通过后再更新内存快照和持久化文件；
4. `effective`、`persisted` 和诊断结果继续分开返回；
5. 为每个可写键增加拒绝测试，确认失败不会改变活动快照。

## 3. 与 ATOM-03 的交接边界

- `sbus.frame_bytes` 的固定 25 字节约束由 ATOM-15 Schema/加载校验和 ATOM-03 消费校验共同保证；当前两侧均未强制到 25。
- `timing.sbus_receive_period_ms` 的键值加载属于 ATOM-15，实际读取调度和 UART 行为属于 ATOM-03。
- ATOM-15 只提供 `IConfigView`，不负责 SBUS 控制意图、失联上报或串口线程生命周期。

## 4. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-13 | 首次登记完整 Schema 校验缺口、JSON 字符串解析/序列化转义缺口和运行时更新值域校验缺口。 |
