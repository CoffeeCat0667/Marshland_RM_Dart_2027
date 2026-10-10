# ATOM-01_Runtime 缺陷清单（BugLists）

## 0. 文档定位

| 项目 | 内容 |
|---|---|
| 对象模块 | `Atoms/ATOM-01_Runtime` |
| 权威来源 | `Design/Atomic/ATOM-01_Runtime.md`；`Design/Interface.md` §4.2、§4.3、§5.1、§6.2、§7；`Design/Config.md` §3、§13、§15；`Design/PEF.md` §7、§8 |
| 维护约定 | **只登记未修复缺陷。** 已修复缺陷的描述一律删除，仅在 §3 留一行标题供追溯与防止重复报告 |
| 更新日期 | 2026-09-27 |

---

## 1. 未修复缺陷

当前无未修复缺陷。

---

## 2. 缺陷详述

当前无未修复缺陷，故无详述条目。

---

## 3. 已修复项索引（描述已按维护约定删除）

| 编号 | 标题 | 状态 |
|---|---|---|
| ATOM01-BUG-001 | 本地 `RuntimeState` 与权威 `SystemMode` 双轨并存 | 已裁决不修复（分层为现行设计） |
| ATOM01-BUG-002 | `canExecuteActuatorCommands()` 会锁死手动/调试/校准模式 | 已修复（方法按授权移除） |
| ATOM01-BUG-003 | `start()` 允许 `Stopped -> Created` 运行中重启 | 已修复（按 2026-09-27 裁决移除重启路径） |
| ATOM01-BUG-004 | `start()` 异常保护不覆盖内存分配与结果写回 | 已修复（`noexcept` + 全流程保护） |
| ATOM01-BUG-005 | `ModuleRegistration::required` 无默认值 | 已修复 |
| ATOM01-BUG-006 | `RuntimeContext` 以字符串字典占位，无配置模型 | 已裁决不实施（归属 ATOM-15 + `Public` 契约决策） |
| ATOM01-BUG-007 | 共享契约未落入 `Atoms/Public` | 已修复（上提 `ILifecycle.hpp`） |
| ATOM01-BUG-008 | 无结构化步骤事件 | 契约已定义（`Design/LifecycleEvents.md`），代码未实施 |
| ATOM01-BUG-009 | 每步骤仅允许注册一个模块 | 已修复（1 步骤 : N 模块 + FIFO） |
| ATOM01-BUG-010 | 启动协调器无法通知"初始化开始" | 已裁决（启动前迁移归装配层，写入 `PLAN.md`） |

## 4. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.7 | 2026-09-27 | 按维护约定精简：删除全部已修复缺陷的详述、过时修复方案与历史修复记录；仅保留 ATOM01-BUG-003（重启路径与裁决冲突）；§3 仅留已修复项标题索引。 |
| 0.8 | 2026-09-27 | ATOM01-BUG-003 已修复：按"不支持运行中重启"裁决（ATOM02-BUG-015）移除 `start()` 中 `Stopped -> Created` 的重启路径，同步更新 `Inc/RuntimeCoordinator.hpp` 头注释、`PLAN.md` 与 `Design/LifecycleEvents.md` 的事件不变量。§1/§2 现无未修复缺陷。 |
