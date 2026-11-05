# P0-ENGINE-AOT-EMPTY-01：零 AOT block 函数的 artifact 诊断

状态：未开始。

## 输入与责任

- 阶段/需求：P0 收尾/ENGINE 诊断；R06；前置 P0-CORE-FIX-01。
- owner/reviewer/QA：compiler / reviewer / qa。
- 输入：`p0/engine-spike` HEAD `8c0fc15`；QA 观察：只含 JIT-only `imul` block 的函数 `aot_blocks=0`，生成 `[0 x %BlockEntry] [zeroinitializer]`，clang `expected type`；基线 `a4ef11c` 同样存在。
- 隔离：新 worktree/分支。

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
