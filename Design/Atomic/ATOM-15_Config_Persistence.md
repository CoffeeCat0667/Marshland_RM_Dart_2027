# ATOM-15：配置与持久化

## 元信息

| 项目 | 内容 |
|---|---|
| Atom ID | ATOM-15 |
| 功能类 | 配置加载、校验、运行时热加载与参数持久化 |
| 程序代号 | Marshland |
| 状态 | 启用 |
| 来源 | PRD 7.1、9.2、16；Interface 2.3、16、20、21；Config 1、13、17 |
| 同步基线 | PRD 0.10、PEF 1.5、Interface 1.5、Config 0.8；2026-10-10；实现状态补充 2026-10-15 |

## 单一职责

读取唯一配置来源，为运行模块提供校验后的参数；支持 Config 规定的运行时热加载；保存每个电机 PID 及额外预留变量。

## 配置文件原子操作

1. 打开 `./Config/dart_control.json`。
2. 将 JSON 文本解析为配置对象。
3. 校验一个字段的类型。
4. 校验一个字段是否属于 Config 已定义的范围。
5. 为一个缺失可选参数按 Config 规则使用内置默认值。
6. 必需硬件/安全参数缺失时返回初始化失败条件。
7. 对损坏文件按 Config 策略报告错误；不得自行发明自动修复或掉电保护。
8. 查询一个配置键值。
9. 对一次运行时参数更新执行来源、模式、类型和值域校验。
10. 更新一个已接受的运行时配置值，使其按 Config 规则立即生效。
11. 将该更新写入 JSON 配置文件，并分别记录运行时生效和持久化结果。
12. 参数变更成功后发布配置热加载/参数变更事件。
13. 读取一个电机 PIDConfig。
14. 保存一个电机 PIDConfig。
15. 读取或更新一个预留持久化变量；总数和结构按 Config。

## PID 参数修改授权

- 控制源仅为 WEB。
- 允许模式仅为手动模式或调试模式。
- 其他控制源或模式下拒绝更新，且不得改变活动 PID 参数。
- 获准更新后立即热加载；并按 Config 要求写入 `./Config/dart_control.json`。
- 若运行时生效和文件写入结果不一致，必须分别报告 `effective` 与 `persisted` 状态；不得谎报全部成功。
- PID 数值、边界和策略从 Config 读取，不由本 Atom 推定。

## 持久化限制

- 每个电机的 PID 参数重启后恢复。
- 额外预留 100 个持久化变量。
- 文件损坏不自动修复；缺失参数按 Config 默认规则。
- 写入为 Config 规定的普通文件写入，不额外承诺掉电原子性或版本迁移。
- 参数越界处理严格按 Config，不自动钳位或覆写。

换弹两个 M3508 的映射、PID 和 Config 5.6 速度/时间参数纳入加载、校验和持久化；缺失必需映射/PID 时禁止动作。换弹人工校准不产生程序校准/回零参数。

## 验收条件

- 必需配置缺失/损坏可阻止执行器正常启动。
- 单字段校验错误能独立报告。
- WEB 在手动/调试模式更新 PID 后运行时立即生效并持久化。
- 其他控制源/模式不能修改 PID。
- 重启后已保存的每电机 PID 参数可恢复。

## 实现状态（2026-10-15，以代码事实为准）

- 实现位于 `Atoms/ATOM-15_Config_Persistence`：`ConfigStore`（`IConfigView`）+ `LifecycleAdapter`（`ILifecycleModule`），Pimpl 隔离第三方类型。
- JSON 解析/序列化：nlohmann/json 3.12.0（vendored 于 `ThirdParty/nlohmann`）；重复对象键经解析回调拒绝；超 INT64_MAX 正整数视为非法。
- 配置校验：`Rule/ConfigSchema.schema.json` 由上层 CMake 编译期嵌入二进制，pboettch json-schema-validator 2.4.0（`ThirdParty/pboettch`）执行完整 Schema 校验；其外仅保留 Schema 无法表达的薄语义层（电机 id/node 去重、`valid_min≤valid_max`、`pulse_max>pulse_min`、区间有序性、PCA9685 prescale 公式）。
- 运行时更新（`updateRuntime`/`writePid`/`writeReserved`）：仅 WEB 来源 + MANUAL/DEBUG 模式；候选快照经同一校验后才生效；`effective` 与 `persisted` 分开报告；热更新受完整 Schema 约束。
- 与"配置文件原子操作"第 5 条的偏差：当前**不执行缺失参数的内置默认值回填**（Schema 中的 `default` 仅作注释性元数据），缺失必需项直接初始化失败；`persistence.missing_param_recovery: USE_BUILTIN_DEFAULT` 的回填行为尚未实现。
- `sbus.frame_bytes==25` 的恒等约束不在 ATOM-15 侧执行（Schema 仅 `minimum:1`），由 ATOM-03 消费侧承担。