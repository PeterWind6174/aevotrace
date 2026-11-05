# P0-ENGINE-AOT-EMPTY-01：零 AOT block 函数的 artifact 诊断

状态：完成（已集成 `p0/engine-spike` `b4052dd`；独立 QA PASS；reviewer 无代码阻断）。

## 输入与责任

- 阶段/需求：P0 收尾/ENGINE 诊断；R06；前置 P0-CORE-FIX-01。
- owner/reviewer/QA：compiler / reviewer / qa。
- 输入：`p0/engine-spike` HEAD `8c0fc15087221e408e84f4b7adf50fc67f2a9abf`；QA 观察：只含 JIT-only `imul` block 的函数 `aot_blocks=0`，生成 `[0 x %BlockEntry] [zeroinitializer]`，clang `expected type`；基线 `a4ef11c` 同样存在。
- 隔离：分支 `p0/core-fix-02`，worktree `.worktrees/p0-core-fix-02`（从 `8c0fc15` 创建）；构建目录 `.local/build-core-fix-02`；本轮原始证据 `.local/results/P0-FINISH-01/`。
- 模型/档位：OpenCode Go / DeepSeek V4.1 Flash / Max；每个会话在 OpenCode 会话库核对 `session_v2.model`，不以文字自述代替。
- 执行方式：OpenCode 桌面端原生子会话；不运行 `opencode run`/CLI worker；worker 不派生 agent。

## 目标与范围

- 修正空 block 表的 LLVM IR 语法，或在 `aot_blocks==0` 时明确失败并给出诊断；不得静默生成无效 artifact。
- 增加“纯 IMUL 函数/零 AOT block”回归；保留 JIT 路径可用性。
- 不包含：把 IMUL 改为 AOT 支持、PE/GUI/真实应用、性能。

## 验收

- `aev-translate` 对零 AOT block 输入要么生成可编译 IR（空表合法），要么非零失败并诊断；不能 `rc=0` 但 clang 失败。
- 原 closed-loop/semantics 测试仍通过；新增回归在 ctest 中。
- 报告输入/产物哈希、命令/退出码、日志。

## 完成报告

- commit、命令/退出码、哈希、未测项。

## 本轮实施要求（2026-11-05）

- 先在 `8c0fc15` 独立复现：仅 `imul eax,ecx,7; ret` 的 COFF，`aev-translate` rc=0 但 `.ll` 含 `[0 x %BlockEntry] [zeroinitializer]`，clang rc=1；保存输入、命令、退出码、日志、哈希到本任务结果目录。
- 优先方案：生成合法空表 `@aev_blocks = constant [0 x %BlockEntry] zeroinitializer`，并实际加载生成的 dylib，通过 `aev_aot_run` → `jit_dispatch` 进入 JIT 执行纯 IMUL block；断言 `aot_blocks=0`、JIT 至少编译/执行一次、结果正确。
- 若现有契约确实不能支持纯 JIT 输入，才可改为 `aev-translate` 非零拒绝并给出准确诊断；必须在报告中写明取舍、限制和为何不选合法空表。不得 rc=0 却产生无法编译的产物。
- 不增加 IMUL 的 AOT 支持，不改动非空 AOT 表、AOT/JIT 交接或原 closed-loop 行为。
- 将零 AOT block 场景纳入 `ctest`，明确测试合法产物或显式拒绝之一。
- 新增测试必须实际编译/加载/执行，不接受只检查 IR 文本。

## 完成报告（2026-11-05）

- 分支 `p0/core-fix-02`，worktree `.worktrees/p0-core-fix-02`；基线 `8c0fc15`；阶段 A 提交 `9ad0632`；最终代码 HEAD `7487e04`；报告/分支头 `b4052dd`。已 fast-forward 集成到 `p0/engine-spike`；未 push、未合并 main。
- 基线独立复现：`imul eax,ecx,7; ret` → `aev-translate` rc=0、`@aev_blocks = constant [0 x %BlockEntry] [zeroinitializer]`、clang rc=1 `expected type`。
- 最终方案：合法空表 `@aev_blocks = constant [0 x %BlockEntry] zeroinitializer`；实际编译/链接 ARM64 dylib、加载、`aev_aot_run` → `jit_dispatch` → ORC 执行；`RAX=0x15`、`aot_table_count=0`、`aot_blocks=0`、`jit_compiles=1`、`jit_execs=1`、`error=0`。
- ctest：新增 `p0_engine_empty_aot`，1 个端到端场景/22 条断言 PASS；非空 AOT `.ll/.o` 哈希保持 `df9aa1d6...`/`5b158e59...`。
- 证据：`.local/results/P0-FINISH-01/repro-empty-aot/`、`final/empty-aot/`；QA `.local/results/P0-FINISH-QA-01/REPORT.md`；reviewer `.local/results/P0-FINISH-REVIEW-01/REPORT.md`。
- 未测/限制：未增加 AOT IMUL；空表回归只覆盖纯 IMUL 输入；PE/GUI/真实应用/性能等不在范围。
