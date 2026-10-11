# ATOM-01_Runtime 待修改事项

- 更新日期：2026-10-10。
- 设计基线：PRD 0.10、PEF 1.5、Interface 1.5、Config 0.8。
- 本文件按本轮用户要求重新创建，只登记未完成事项；不恢复已删除的历史清单。
- 本工程 Agent 只修改文档与网关/装配代码，以下 ATOM-01 内部修改由对应负责人实施。

## 1. 未完成事项

当前无未完成事项。

---

## 2. 本轮已完成（供追溯，非未完成清单）

**ATOM01-BUG-011：缺少启动前真实注册数量查询 —— 已实施（本轮，2026-10-10）。**

- `Inc/RuntimeCoordinator.hpp`：显式包含 `<cstddef>`，新增
  `std::size_t registeredModuleCount() const noexcept;`
- `Src/RuntimeCoordinator.cpp`：实现为 `return modules_.size();` —— 单一真相，不维护第二份计数、
  不返回启动结果条数、不创建假模块。
- 契约对齐：网关 `Atoms/Public/AppCoordinator/Inc/RuntimeRegistrationQuery.hpp` 的
  `if constexpr (requires { { runtime.registeredModuleCount() } noexcept -> std::same_as<std::size_t>; })`
  检测将自动命中，`AppCoordinator::Start()` 不再返回 `kRegistrationInfoUnavailable`；网关无需改动。
- 验收对照：Created 且无模块为 0，每次成功注册 +1，同一步骤多适配器分别计数；失败注册（非法状态 /
  空指针）在 `modules_.insert` 之前返回，不改变数量；查询为 `const noexcept`，不改状态、不调用
  `initialize`、无分配、无日志、无硬件访问。
- 遗留（不在本模块文件内）：`NewVersion/PLAN.md` 的 R1 状态行与 §2.1/§2.2 仍写"该接口当前不存在"，
  需由该文件职责方同步；`Tests/AppCoordinatorTests.cpp` 的相关用例可解除 PENDING。
