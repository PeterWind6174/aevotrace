# P0 任务索引

阶段：P0 范围、选型和高风险验证。权威任务基线：`586662a4ff09465d7d3aa453fb44e5aa6e880c79`（P0 任务记录提交；任务开始前的产品基线为 `373becbd`）。每个任务必须在完成报告中记录实际 `git rev-parse HEAD` 与输入/二进制哈希，不把索引基线当成唯一身份。
本索引只登记任务与状态，不把计划当成已实现能力。

2026-11-07 收口：本批所有已实施分支已统一到 main（集成 `ce9c11b`，产品源码 `47b2b7a`），独立外部复核通过。旧分支/worktree 为历史执行位置，归档和恢复见 [收尾记录](../p0/INTEGRATION-20261107.md)；未开始与人工未验收项目转入 [待办清单](../BACKLOG.md)，下一阶段未启动。

| 任务 | 目标 | owner（lead） | 写代码 | 状态 |
| --- | --- | --- | --- | --- |
| P0-ENV-01 | 双机工具链编译/运行/回传 smoke（Windows → Mac） | qa | 否 | 完成 |
| P0-SCOPE-01 | Win32 GUI 测试族、真实应用候选、API/依赖/许可矩阵 | binary（lead）+ platform | 否 | 完成（静态调查；未运行应用） |
| P0-ABI-01 | x64 Windows ABI、TLS、异常、回调、内存模型与依赖许可调查 | binary（lead）+ platform | 否 | 完成（调查；运行时未验证） |
| P0-TRACE-01 | 准备执行、动态发现与记录可移植性方案 | binary（lead）+ compiler | 否 | 调查归档；有界块级记录由 P0-TRACE-AOT-01 验证，完整失效/采集契约转待办，P0-05 未整体关闭 |
| P0-ENGINE-01 | x64→ARM64 AOT/JIT 最小闭环与执行引擎选型 spike | compiler | 是（隔离 worktree） | 完成（QA 功能 PASS；F1–F5 + 零 AOT 修复集成 `b4052dd`） |
| P0-UI-01 | `compat` / `native` GUI 回调与窗口最小 spike | platform | 是（隔离 worktree） | 待人工验证（自动部分完成；W-1/W-2 PASS） |
| P0-QA-01 | 对 ENV/ENGINE/UI 指定提交和产物做独立复跑与反伪证检查 | qa | 否 | 完成（功能 + W-1/W-2 PASS；整体 P0 不通过） |
| P0-UI-INT-01 | ENGINE 核心替换 Unicorn，跑通双后端回调 | platform + compiler | 是（隔离 worktree） | 待人工验证（自动链/外部审查 PASS；base/supplemented 44/44） |
| P0-TRACE-SPIKE-01 | 自建核心 Guest PC/基本块/间接目标记录 | compiler + binary | 是（隔离 worktree） | 完成（块级记录/新进程验证由 P0-TRACE-AOT-01 实现并经 QA/reviewer 复核；非完整指令 Trace） |
| P0-CALLBACK-ABI-01 | 最小 Host↔Guest 回调契约与固定 fixture | coordinator + compiler/platform | 是（隔离 worktree） | 完成（v2 会签；QA/reviewer PASS） |
| P0-CORE-BRIDGE-01 | 共享 JIT/回调 bridge、AOT 身份与无 GUI 测试 | compiler + binary | 是（隔离 worktree） | 完成（4 ctest/77 checks；QA PASS） |
| P0-TRACE-AOT-01 | 实际 Trace → 补充 AOT → 新进程验证 | compiler + binary + platform | 是（隔离 worktree） | 完成（原 `adf9a95` QA/reviewer PASS 为历史；F1 已由 `66a63fb` 修复并通过独立 QA/reviewer；物理 GUI/IME 待人工验收） |
| P0-PROMOTION-FIX-01 | 修正 promoted_blocks 来源为实际 base artifact | compiler + binary + platform | 是（隔离 worktree） | 完成（source `66a63fb`；QA PASS/reviewer accepted；物理 GUI/IME 待人工验收） |
| P0-PE-DLL-01 | 固定 PE32+ DLL 装载、导入导出与共享核心执行 | binary + compiler | 是（隔离 worktree） | 完成并合 main；权限/SAR 缺陷由 PE-CORE-FIX 修复并经外部复核 |
| P0-PE-CORE-FIX-01 | 无权限节与 SAR CF 聚焦修复 | compiler + qa | 是（隔离 worktree） | 完成并合 main（`de67e2b`），外部复核通过 |
| P0-PE-GUI-01 | 持久 PE session 与双 GUI、实际 Trace 补充 | compiler + platform + binary | 是（隔离 worktree） | 自动链完成并合 main；observer/arity 由 SESSION-FIX 修复；人工 GUI 待验收 |
| P0-PE-SESSION-FIX-01 | observer 生命周期与 HostImport arity | platform + compiler | 是（隔离 worktree） | 完成并合 main（源码 `47b2b7a`），QA/外部复核通过，8/8 ctest |
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

- 当前唯一开发入口为根目录 main；本批源码、契约和跟踪报告已集中在该工作树。
- 旧 worktree/分支→提交→归档的完整对应表见本地 `.local/cleanup-audits/20261107-closeout/worktrees-before.json`、`branches-before.txt` 和 [收尾记录](../p0/INTEGRATION-20261107.md)。旧相对路径在归档内保持；可按原提交恢复，不能把它当成当前工作区。
- 本地构建使用 `.local/build-main`，GUI 环境 `.local/venv-callback`；原始证据保留在 `.local/results/`。
- Windows 历史目录为 `C:\Users\PeterWind\AevotraceP0\<TASK-ID>`；本次收尾不修改远程机器。
- 下一阶段没有派发任务；新任务必须基于当时最新 main 创建隔离工作树。

## 状态口径

- `未开始` / `进行中` / `待审查` / `待人工验证` / `完成` / `失败`。
- 只有命令、退出码、哈希、日志和独立复核证据齐全时才能从 `进行中` 进入 `完成`。
- GUI 自动部分通过但人工观察未完成时，保持 `待人工验证`。
