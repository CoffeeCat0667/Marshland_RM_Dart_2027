# AGENTS.md — ATOM-12_Vision_YAW 工作目录规则

本文件是本工作目录的唯一工作规则来源。以本工作目录为工作目录的 Agent 必须遵守以下全部约束。

## 1. 阅读范围限制

1. 只允许阅读本工作目录下的本 `AGENTS.md` 这一份 AGENTS.md。
2. 禁止阅读其他子目录、父目录以及任意上层或下层目录中的 `AGENTS.md` 文件。
3. 禁止读取或修改任何上层目录中的文件。

## 2. 允许的例外（仅限以下三项）

1. 上层 Public 文件夹：`D:\Project\C++\RM2027\Cocoon-Dart2027\NewVersion\Atoms\Public` —— **允许读取与写入**。
2. 对应的原子功能规划文件：`D:\Project\C++\RM2027\Cocoon-Dart2027\NewVersion\Design\Atomic\ATOM-12_Vision_YAW.md` —— **允许只读，禁止修改**。
3. 上层 CMake 工程文件：`D:\Project\C++\RM2027\Cocoon-Dart2027\NewVersion\CMakeLists.txt`（上层 CMakelist） —— **允许只读，禁止修改**。

除上述三项例外外，不得读取或修改任何上层目录文件。

## 3. 原子功能实现约束

1. 不允许出现 `main` 入口函数。
2. 头文件必须放在 `Inc` 文件夹内。
3. 源码必须放在 `Src` 文件夹内。
4. 如果是 header-only 实现，则不需要创建 `Inc` 或 `Src` 文件夹，直接在本目录存放头文件即可。

## 4. 目录结构初始化与 CMake 工程

1. 初始化本目录结构时，必须创建 `CMakeLists.txt`，使其构成一个完整、可被上层调用的 CMake 工程。
2. 本模块代码不需要独立编译；编译链、目标平台与编译选项全部由上层 CMake 工程 `D:\Project\C++\RM2027\Cocoon-Dart2027\NewVersion\CMakeLists.txt` 决定。
3. 本目录的 `CMakeLists.txt` 仅用于模块化组织，以及管理本模块的依赖与配置。
4. 其他要求与上层 `CMakeLists.txt` 保持一致。

## 5. 修改授权与信息不足处理

1. Agent 进行任何修改前，必须获得用户的明确许可；未经明确许可，禁止修改任何文件。
2. 在任何信息不满足的情况下，都不得直接开始修改文件。
3. 信息不满足时，必须向用户询问所需信息，或请求用户授权搜索上层目录内的文件。
