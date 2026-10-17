# ATOM-18 Diagnostics & Logging 实现计划（2026-10-16 收敛为唯二接口后）

> 依据：`Design/Atomic/ATOM-18_Diagnostics_Logging.md`（只读基线）。
> 工作规则：仅遵守本目录 `AGENTS.md`；任何修改需用户明确许可。

## 0. 用户裁决记录

### 0.1 第一次裁决（2026-10-16）：删除全部对外/对内接口

| # | 决策点 | 结论 |
|---|--------|------|
| 1 | 调用装配层/其他 ATOM 公开接口 | 全部删除（无 `IConfigView` / `IMonotonicClock` / `ILifecycle` 注入） |
| 2 | 对外提供数据/接口 | 全部删除（`Atoms/Public/` 下 4 个契约头删除并在 `PublicHeaders` 注销） |
| 3 | 内部代码 | 极大简化：无第三方库、无后台线程、无队列、无虚接口 |

### 0.2 第二次裁决（2026-10-16）：恢复唯二对外接口 + 两项输出设计

| # | 决策点 | 结论 |
|---|--------|------|
| 4 | 对外接口 | 全系统日志功能**只有两个**对外函数：`marshland::log::Init(Level)` 与 `marshland::log::Log(Level, string_view)`，声明在 `Atoms/Public/Log.hpp` |
| 5 | 初始化语义 | `Init(Level)` 传入日志等级；初始化后**等级 ≥ 该等级**的日志打印到终端；可重复调用以调整阈值 |
| 6 | 磁盘写入 | 增加日志文件写入磁盘的设计（§1）：**全部等级**都写文件，不受终端阈值影响 |
| 7 | 终端打印 | 增加日志打印到终端的设计（§2）：按阈值过滤、行式文本、立即 flush |
| 8 | 配置来源 | 对外接口不接收配置，原"从 Config 读取"的各项改为模块内编译期常量（§1.4） |

**对外接口的唯一性**：`Atoms/Public/Log.hpp` 只声明 `Level`（参数类型）、`Init`、`Log`；本模块不再有任何其他对外头、虚接口、查询接口或数据导出。`Level` 是这两个函数的参数类型，不是第三个接口。

## 1. 日志文件写入磁盘设计

### 1.1 格式与落点

- 格式：**JSON Lines**（UTF-8，每行一个 JSON 对象，行尾 `\n`）；字段固定为
  `timestamp`、`level`、`message`：

  ```json
  {"timestamp":"2026-10-16T23:40:12.345Z","level":"INFO","message":"state -> MANUAL"}
  ```

- 时间戳：`system_clock` 取 UTC，格式 `YYYY-MM-DDTHH:MM:SS.mmmZ`（定长 24 字符，日历值钳制防溢出）。
- 目录：`./Logs/`（相对程序工作目录，编译期常量）；首次写入时惰性 `create_directories`，`Init` 也会提前创建以尽早暴露"目录不可写"。
- 文件命名：`marshland-<UTC毫秒>-<序号>.jsonl`（序号进程内单调，`trunc` 打开，不覆盖同名历史文件）。
- 写入范围：**所有等级**都写文件（含低于终端阈值的 TRACE/DEBUG），因为文件的用途是事后完整追溯；终端阈值只影响打印。
- 转义：`"` `\` `\n` `\r` `\t` 转义，其余 `<0x20` 控制字符转 `\uXXXX`，保证一行一条合法 JSON。
- 刷新：每条写入后立即 `flush()`（同步落盘），保证进程异常退出时已写日志不丢。

### 1.2 轮转与清理

- 轮转触发：当前文件累计字节数 ≥ `kMaxFileSizeBytes`（10 MiB）时，关闭当前文件并打开新文件。
- 清理时机：每次轮转后执行一次；只处理形如 `marshland-*.jsonl` 的文件。
- 清理策略：按文件修改时间从旧到新，在"文件数 > `kMaxFiles`（10）"或"目录总量 > `kMaxTotalSizeBytes`（100 MiB）"时删除最旧文件；**当前正在写的文件始终保留**，且至少保留 1 个文件。

### 1.3 失败行为

- 目录创建失败或文件打开失败：该条日志的磁盘部分静默跳过（不抛异常、不终止、不影响终端打印），并在下次写入时重试打开。

### 1.4 编译期常量

| 常量 | 值 | 原设计来源 |
|---|---|---|
| `kOutputDirectory` | `"./Logs/"` | Config §16.1 `log.output_directory` |
| `kMaxFileSizeBytes` | 10485760（10 MiB） | Config §16.1 `log.max_file_size_bytes` |
| `kMaxTotalSizeBytes` | 104857600（100 MiB） | Config §16.1 `log.max_total_size_bytes` |
| `kMaxFiles` | 10 | Config §16.1 `log.max_files` |

## 2. 日志打印到终端设计

- 阈值：`Init(Level)` 设定的等级 `T`；仅当 `level >= T` 时打印到终端。
- `Init` 未被调用时：阈值取 `Level::INFO`（保证启动早期日志不静默丢失，且不刷屏 TRACE/DEBUG）。`Init` 可重复调用以在运行期调整阈值。
- 输出设备：`stdout`（全部等级统一走 stdout；不区分 stderr）。
- 行格式（内容原样，不做 JSON 转义，便于人读）：

  ```text
  2026-10-16T23:40:12.345Z [WARN] sbus offline, hold last command
  ```

- 刷新：每条打印后 `fflush(stdout)`，保证终端/日志采集器实时可见。

## 3. 文件清单与构建

```text
Atoms/Public/Log.hpp                            # 唯二对外接口（Init / Log + Level）
Atoms/ATOM-18_Diagnostics_Logging/
├─ CMakeLists.txt                               # STATIC 库 atom_18_diagnostics_logging
├─ PLAN.md                                      # 本文件
└─ Src/log.cpp                                  # 全部实现（匿名命名空间，不导出任何符号）
```

- 目标名不变：`atom_18_diagnostics_logging` / `Marshland::atom_18_diagnostics_logging`。
- 依赖：仅标准库（`<filesystem>` / `<fstream>` / `<mutex>` / `<chrono>`）；无第三方库；包含路径 PRIVATE（`Inc/` 为空 + `../Public`）。
- 并发：两个接口内用同一把 `std::mutex` 串行化，任意线程可调用；单条日志的"文件 + 终端"在同一次加锁内完成，终端行与文件行不会交错。
- 说明：本模块的对外头在 `Atoms/Public/`（用户指定位置），因此 `Inc/` 目前没有私有头；这符合"对外唯一头必须在 Public"的要求。

## 4. 与设计文档的偏差记录（`Design/` 只读，未修改）

| 设计文档要求 | 现状 |
|---|---|
| 原子操作 1：写入时间戳、等级、模块、事件、来源、状态、payload | 现仅写 `timestamp`/`level`/`message` 三个字段（内容由调用方自行拼接） |
| 原子操作 2~7：故障码分配、故障保存/查询/恢复/清除 | 未实现（无故障库） |
| 原子操作 8~11：状态迁移/控制权/发射结果/急停分类写入 | 由调用方通过同一 `Log` 接口写入，无分类通道 |
| 原子操作 12：ERROR/安全事件立即刷新 | 全部等级都立即 flush（更强） |
| 原子操作 13/14：轮转与超限删除最旧文件 | 已实现（§1.2） |
| 原子操作 15：对外提供诊断快照 | 未实现（对外接口仅 Init/Log） |
| 配置约束：等级/格式/路径/轮转/上限**均读取 Config** | 改为编译期常量（§1.4），不读 Config |
| 验收：按故障码/来源/时间查询历史故障 | 不适用（无故障库） |

## 5. 可选项（待用户裁决，本次未做）

1. 若需要"等级/路径/上限可配置"，可在不增加对外接口的前提下改为环境变量或进程启动参数；
2. 若需要恢复结构化字段（module/event/source/state/error_code）与故障库，需先修订设计文档并恢复 `Atoms/Public/` 契约头；
3. 若需要"不阻塞控制线程"，需重新引入异步写线程（当前每条同步 flush）。

## 6. 回退点

- `%TEMP%\atom18_backup_20261016-233259`：收缩前（含 4 个 Public 契约头与旧实现）的完整备份；
- `%TEMP%\atom18_backup_before_logapi_20261016-234014`：新增 `Log.hpp` 之前的模块内实现备份。
