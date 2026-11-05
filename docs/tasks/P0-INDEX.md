# P0 任务索引

阶段：P0 范围、选型和高风险验证。基线：`373becbdd7a65cafd63a98cb526bb1a9c933f731`（`main` = `origin/main`）。
本索引只登记任务与状态，不把计划当成已实现能力。

| 任务 | 目标 | owner | 写代码 | 状态 |
| --- | --- | --- | --- | --- |
| P0-ENV-01 | 双机工具链编译/运行/回传 smoke（Windows → Mac） | qa | 否 | 进行中 |
| P0-SCOPE-01 | Win32 GUI 测试族、真实应用候选、API/依赖/许可矩阵 | binary + platform | 否 | 进行中 |
| P0-ABI-01 | x64 Windows ABI、TLS、异常、回调与依赖许可调查 | platform + binary | 否 | 进行中 |
| P0-ENGINE-01 | x64→ARM64 AOT/JIT 最小闭环与执行引擎选型 spike | compiler | 是（隔离 worktree） | 进行中 |
| P0-UI-01 | `compat` / `native` GUI 回调与窗口最小 spike | platform | 是（隔离 worktree） | 进行中 |
| P0-ADR-01 | 汇总证据，形成选型 ADR、支持矩阵与 P1 契约草案 | coordinator | 否 | 未开始 |
| P0-REVIEW-01 | 对 ADR 与 P0 证据做独立只读审查 | reviewer | 否 | 未开始 |

## 工作区

- `main` 工作目录：`/Users/peterwind/Script/Systems/Aevotrace`。coordinator 独占文档与集成。
- `P0-ENGINE-01`：分支 `p0/engine-spike`，worktree `.worktrees/p0-engine`。
- `P0-UI-01`：分支 `p0/ui-spike`，worktree `.worktrees/p0-ui`。
- 调查/QA 任务不修改产品源码；报告写入 `.local/results/<TASK-ID>/`（被 `.gitignore` 排除），由 coordinator 汇总。
- Windows 工作目录：`C:\Users\PeterWind\AevotraceP0\<TASK-ID>`（首轮由 P0-ENV-01 实际创建并记录）。

## 状态口径

- `未开始` / `进行中` / `待审查` / `待人工验证` / `完成` / `失败`。
- 只有命令、退出码、哈希、日志和独立复核证据齐全时才能从 `进行中` 进入 `完成`。
- GUI 自动部分通过但人工观察未完成时，保持 `待人工验证`。
