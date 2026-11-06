# P0 任务索引

阶段：P0 范围、选型和高风险验证。权威任务基线：`586662a4ff09465d7d3aa453fb44e5aa6e880c79`（P0 任务记录提交；任务开始前的产品基线为 `373becbd`）。每个任务必须在完成报告中记录实际 `git rev-parse HEAD` 与输入/二进制哈希，不把索引基线当成唯一身份。
本索引只登记任务与状态，不把计划当成已实现能力。

| 任务 | 目标 | owner（lead） | 写代码 | 状态 |
| --- | --- | --- | --- | --- |
| P0-ENV-01 | 双机工具链编译/运行/回传 smoke（Windows → Mac） | qa | 否 | 完成 |
| P0-SCOPE-01 | Win32 GUI 测试族、真实应用候选、API/依赖/许可矩阵 | binary（lead）+ platform | 否 | 完成（静态调查；未运行应用） |
| P0-ABI-01 | x64 Windows ABI、TLS、异常、回调、内存模型与依赖许可调查 | binary（lead）+ platform | 否 | 完成（调查；运行时未验证） |
| P0-TRACE-01 | 准备执行、动态发现与记录可移植性方案 | binary（lead）+ compiler | 否 | 待审查（仅调查/方案）；实际 Guest Trace 由 P0-TRACE-AOT-01 实现，P0-05 未整体关闭 |
| P0-ENGINE-01 | x64→ARM64 AOT/JIT 最小闭环与执行引擎选型 spike | compiler | 是（隔离 worktree） | 完成（QA 功能 PASS；F1–F5 + 零 AOT 修复集成 `b4052dd`） |
| P0-UI-01 | `compat` / `native` GUI 回调与窗口最小 spike | platform | 是（隔离 worktree） | 待人工验证（自动部分完成；W-1/W-2 PASS） |
| P0-QA-01 | 对 ENV/ENGINE/UI 指定提交和产物做独立复跑与反伪证检查 | qa | 否 | 完成（功能 + W-1/W-2 PASS；整体 P0 不通过） |
| P0-UI-INT-01 | ENGINE 核心替换 Unicorn，跑通双后端回调 | platform + compiler | 是（隔离 worktree） | 待人工验证（自动链/外部审查 PASS；base/supplemented 44/44） |
| P0-TRACE-SPIKE-01 | 自建核心 Guest PC/基本块/间接目标记录 | compiler + binary | 是（隔离 worktree） | 进行中（块级记录由 P0-TRACE-AOT-01 实现；独立任务验收未做，非完整指令 Trace） |
| P0-CALLBACK-ABI-01 | 最小 Host↔Guest 回调契约与固定 fixture | coordinator + compiler/platform | 是（隔离 worktree） | 完成（v2 会签；QA/reviewer PASS） |
| P0-CORE-BRIDGE-01 | 共享 JIT/回调 bridge、AOT 身份与无 GUI 测试 | compiler + binary | 是（隔离 worktree） | 完成（4 ctest/77 checks；QA PASS） |
| P0-TRACE-AOT-01 | 实际 Trace → 补充 AOT → 新进程验证 | compiler + binary + platform | 是（隔离 worktree） | 待审查（实现/自动测试在 `8325b64` 通过；独立 QA/reviewer 待做） |
| P0-APP-RUN-01 | 7-Zip 26.05 Windows 参考工作流 | qa | 否 | 未开始 |
| P0-PKG-01 | 最小 ARM64 `.app`/随包依赖/签名探针 | platform + qa | 是（隔离 worktree） | 未开始 |
| P0-BUILD-REPRO-01 | ENGINE/UI 构建可复现口径 | compiler + qa | 是（隔离 worktree） | 未开始 |
| P0-CORE-FIX-01 | 执行核心 F1–F5 正确性修复与回归 | compiler | 是（隔离 worktree） | 完成（已集成 `p0/engine-spike`） |
| P0-CORE-FIX-QA-01 | F1–F5 修复的独立复跑 | qa | 否 | 完成（PASS；1 N/A） |
| P0-CORE-FIX-REVIEW-01 | F1–F5 修复的独立只读审查 | reviewer | 否 | 完成（无代码阻断；建议集成） |
| P0-CORE-FIX-02 | F1–F5 回归补强与最终 HEAD 绑定 | compiler | 是（隔离 worktree） | 完成（已集成 `b4052dd`） |
| P0-ENGINE-AOT-EMPTY-01 | 零 AOT block 函数的 artifact 诊断 | compiler | 是（隔离 worktree） | 完成（已集成 `b4052dd`） |
| P0-FINISH-QA-01 | 零 AOT block 与回归补强的独立复跑 | qa | 否 | 完成（PASS；0 产品 FAIL） |
| P0-FINISH-REVIEW-01 | 零 AOT block 与回归补强的独立只读审查 | reviewer | 否 | 完成（无代码阻断；建议集成） |
| P0-ADR-01 | 汇总证据，形成选型 ADR、支持矩阵与 P1 契约草案 | coordinator | 否 | 完成（ADR；P0 不通过；P1 输入） |
| P0-REVIEW-01 | 对 ADR、spike 代码与 QA 证据做独立只读审查 | reviewer | 否 | 完成（P0 不通过维持；意见已合并） |

> 多 owner 任务只有一个 lead，另一角色为 contributor；结论冲突由 lead 汇总，coordinator 裁决。

## 工作区

- `main` 工作目录：`/Users/peterwind/Script/Systems/Aevotrace`。coordinator 独占文档与集成。
- `P0-ENGINE-01`：分支 `p0/engine-spike`，worktree `.worktrees/p0-engine`；已集成 F1–F5 与零 AOT 表修复，HEAD `b4052dd`。
- `P0-CORE-FIX-01`：分支 `p0/core-fix-01`，worktree `.worktrees/p0-core-fix`；已 fast-forward 集成到 `p0/engine-spike`。
- `P0-CORE-FIX-02` + `P0-ENGINE-AOT-EMPTY-01`：分支 `p0/core-fix-02`，worktree `.worktrees/p0-core-fix-02`（基线 `8c0fc15`，HEAD `b4052dd`）；已 fast-forward 集成到 `p0/engine-spike`。QA worktree `.worktrees/p0-core-fix-02-qa`（detached `b4052dd`）。
- `P0-UI-01`：分支 `p0/ui-spike`，worktree `.worktrees/p0-ui`。
- `P0-TRACE-AOT-01`：分支 `p0/trace-aot-01`，worktree `.worktrees/p0-trace-aot`（基线 `4bb69ab`；契约 `1f1fe00`、实现 `8325b64`）；独立 QA/reviewer 待做，未合入 main、未 push。
- `P0-TRACE-01`：分支 `p0/trace-investigation`，worktree `.worktrees/p0-trace`（CLI worker，cwd 为该 worktree）。
- 调查/QA 任务不修改产品源码；报告写入 `.local/results/<TASK-ID>/`（被 `.gitignore` 排除），由 coordinator 汇总。原始日志可留 `.local`，但完成报告必须给出可定位路径、哈希和复跑命令。
- Windows 工作目录：`C:\Users\PeterWind\AevotraceP0\<TASK-ID>`（首轮由 P0-ENV-01 实际创建并记录）。

## 状态口径

- `未开始` / `进行中` / `待审查` / `待人工验证` / `完成` / `失败`。
- 只有命令、退出码、哈希、日志和独立复核证据齐全时才能从 `进行中` 进入 `完成`。
- GUI 自动部分通过但人工观察未完成时，保持 `待人工验证`。
