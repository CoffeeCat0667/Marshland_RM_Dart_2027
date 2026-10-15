# ATOM-15_Config_Persistence 缺陷清单

- 更新日期：2026-10-15
- 设计基线：PRD 0.10、PEF 1.5、Interface 1.6、Config 0.9
- 适用模块：`Atoms/ATOM-15_Config_Persistence`
- 维护约定：只登记未完成事项；ATOM-03 的串口线程和控制链问题不在本清单重复登记。

## 1. 未修复缺陷

当前无未修复缺陷。

## 2. 已修复缺陷（2026-10-15，Schema 校验化改造）

### ATOM15-BUG-001：配置校验不是完整 Schema 校验 —— 已修复

`validate()` 已改为两段式：编译期嵌入的 `Rule/ConfigSchema.schema.json` 由
pboettch json-schema-validator 2.4.0 完整执行（`additionalProperties:false`、
枚举、const、正则、数组、`contains+allOf`、`patternProperties` 全部生效），
再叠加 Schema 无法表达的薄语义层（电机 id/node 去重、`valid_min≤valid_max`、
`pulse_max>pulse_min`、区间有序性、prescale 公式）。`load()` 只有完整校验
通过后才替换快照。顺带消除了手写白名单缺 `motor_control/motor_encoder/
servo_mechanisms` 导致的合法配置误判。

遗留说明（按"以 Schema 为准"决策，非缺陷）：`sbus.frame_bytes==25` 的手写
恒等约束已随手写校验删除，Schema 仅约束 `minimum:1`；25 字节协议定长约束
自此完全由 ATOM-03 消费侧承担。如需 ATOM-15 侧恢复该校验，应收紧 Schema
而非恢复手写代码。

### ATOM15-BUG-002：JSON 字符串转义不完整 —— 已修复

解析已切换为 nlohmann/json 3.12.0，`\uXXXX`、UTF-16 surrogate pair、
非法 UTF-8 拒绝等均由库完整实现。

### ATOM15-BUG-003：序列化未转义控制字符 —— 已修复

序列化已切换为 `nlohmann::json::dump()`，控制字符转义、非 ASCII 的 UTF-8
原样输出、浮点最短 round-trip 均由库保证。round-trip（load → update →
persist → reload）的运行期测试仍待 ARM 主机/qemu 环境执行。

### ATOM15-BUG-004：运行时更新校验不完整 —— 已修复

`updateRuntime`/`writePid`/`writeReserved` 对候选快照执行与 `load()` 完全
相同的两段式校验（同一 `validate()`，无第二套规则）；校验失败时活动快照、
版本号、回调与文件均不变，`effective`/`persisted`/诊断继续分开返回。

## 3. 与 ATOM-03 的交接边界

- `sbus.frame_bytes` 的固定 25 字节约束自 2026-10-15 起仅由 ATOM-03 消费侧
  校验承担（Schema 仅 `minimum:1`）；当前两侧均未强制到 25。
- `timing.sbus_receive_period_ms` 的键值加载属于 ATOM-15，实际读取调度和
  UART 行为属于 ATOM-03。
- ATOM-15 只提供 `IConfigView`，不负责 SBUS 控制意图、失联上报或串口线程
  生命周期。

## 4. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-13 | 首次登记完整 Schema 校验缺口、JSON 字符串解析/序列化转义缺口和运行时更新值域校验缺口。 |
| 1.1 | 2026-10-15 | BUG-001~004 全部关闭：JSON 切换 nlohmann/json 3.12.0，校验切换嵌入 Schema + pboettch validator + 薄语义层；frame_bytes 约束移交 ATOM-03 侧的记录保留于 §2/§3。 |
