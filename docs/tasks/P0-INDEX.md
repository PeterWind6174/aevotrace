# P0 任务索引

阶段：P0 范围、选型和高风险验证。权威任务基线：`586662a4ff09465d7d3aa453fb44e5aa6e880c79`（P0 任务记录提交；任务开始前的产品基线为 `373becbd`）。每个任务必须在完成报告中记录实际 `git rev-parse HEAD` 与输入/二进制哈希，不把索引基线当成唯一身份。
本索引只登记任务与状态，不把计划当成已实现能力。

| 任务 | 目标 | owner（lead） | 写代码 | 状态 |
| --- | --- | --- | --- | --- |
| P0-ENV-01 | 双机工具链编译/运行/回传 smoke（Windows → Mac） | qa | 否 | 完成 |
| P0-SCOPE-01 | Win32 GUI 测试族、真实应用候选、API/依赖/许可矩阵 | binary（lead）+ platform | 否 | 完成（静态调查；未运行应用） |
| P0-ABI-01 | x64 Windows ABI、TLS、异常、回调、内存模型与依赖许可调查 | binary（lead）+ platform | 否 | 完成（调查；运行时未验证） |
| P0-TRACE-01 | 准备执行、动态发现与记录可移植性方案 | binary（lead）+ compiler | 否 | 待审查（仅方案，未实现/未运行） |
| P0-ENGINE-01 | x64→ARM64 AOT/JIT 最小闭环与执行引擎选型 spike | compiler | 是（隔离 worktree） | 完成（QA 功能 PASS；V-1/P-1 转 P1） |
| P0-UI-01 | `compat` / `native` GUI 回调与窗口最小 spike | platform | 是（隔离 worktree） | 完成（功能 + W-1/W-2 QA PASS；人工待验） |
| P0-QA-01 | 对 ENV/ENGINE/UI 指定提交和产物做独立复跑与反伪证检查 | qa | 否 | 完成（功能 + W-1/W-2 PASS；整体 P0 不通过） |
| P0-ADR-01 | 汇总证据，形成选型 ADR、支持矩阵与 P1 契约草案 | coordinator | 否 | 进行中（草案） |
| P0-REVIEW-01 | 对 ADR、spike 代码与 QA 证据做独立只读审查 | reviewer | 否 | 未开始 |

> 多 owner 任务只有一个 lead，另一角色为 contributor；结论冲突由 lead 汇总，coordinator 裁决。

## 工作区

- `main` 工作目录：`/Users/peterwind/Script/Systems/Aevotrace`。coordinator 独占文档与集成。
- `P0-ENGINE-01`：分支 `p0/engine-spike`，worktree `.worktrees/p0-engine`。
- `P0-UI-01`：分支 `p0/ui-spike`，worktree `.worktrees/p0-ui`。
- `P0-TRACE-01`：分支 `p0/trace-investigation`，worktree `.worktrees/p0-trace`（CLI worker，cwd 为该 worktree）。
- 调查/QA 任务不修改产品源码；报告写入 `.local/results/<TASK-ID>/`（被 `.gitignore` 排除），由 coordinator 汇总。原始日志可留 `.local`，但完成报告必须给出可定位路径、哈希和复跑命令。
- Windows 工作目录：`C:\Users\PeterWind\AevotraceP0\<TASK-ID>`（首轮由 P0-ENV-01 实际创建并记录）。

## 状态口径

- `未开始` / `进行中` / `待审查` / `待人工验证` / `完成` / `失败`。
- 只有命令、退出码、哈希、日志和独立复核证据齐全时才能从 `进行中` 进入 `完成`。
- GUI 自动部分通过但人工观察未完成时，保持 `待人工验证`。
