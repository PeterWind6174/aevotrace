# P0-FINISH-QA-01：零 AOT block 与回归补强的独立复跑

状态：完成（PASS，0 个产品 FAIL；两处 QA harness 调用笔误已记录，不影响产品结论）。

## 输入与责任

- 阶段/需求：P0 收尾；R01/R06；前置 `P0-ENGINE-AOT-EMPTY-01`、`P0-CORE-FIX-02`。
- owner/reviewer/QA：qa / reviewer / qa。
- 被验版本：最终代码 HEAD（派发时写入完整 40 位 commit）；基线 `8c0fc15087221e408e84f4b7adf50fc67f2a9abf`。
- worktree：`.worktrees/p0-core-fix-02-qa`，detached checkout 最终 commit；不自建分支、不修改产品源码、不 commit、不派生 agent。
- 构建：`.local/qa-build-core-fix-02`；结果：`.local/results/P0-FINISH-QA-01/`。
- 模型/档位：OpenCode Go / DeepSeek V4.1 Flash / Max；派发时显式指定，并在 OpenCode 会话库核对 `session_v2.model` 与 `directory`。

## 目标与范围

- 独立 configure/build，运行 `ctest --output-on-failure`；原 `p0_engine_closed_loop` 和新增回归都必须通过。
- 零 AOT block：独立复现基线错误；验证最终方案（合法空表 + JIT 回退，或明确非零拒绝）实际成立；若为合法空表，必须实际加载 dylib 并走 `aev_aot_run` → `jit_dispatch` 执行 IMUL，不能只检查 IR 文本。
- F3：独立执行 faulting `add/inc/neg/logic [mem]`，检查 flags/内存/寄存器/RSP/RIP 无不应有副作用、`error_pc` 精确；至少一个用例走完整 `aev_aot_run` → `jit_dispatch` → 非零返回。
- F1：独立确认失败翻译不产生 `.ll/.h`/manifest；terminator LOCK 防御；正常入口仍在 IR 前拒绝。
- F4：独立执行 `push imm8/imm32`（含负立即数符号扩展）与 `pushw $imm` 拒绝。
- F5：独立确认 JIT 预置 CF/OF 后，无溢出清零、溢出保持；4/2/1 位回归；不以 ZF/SF/PF 判错。
- 审计测试是否被削弱：删除反例、改期望、屏蔽断言、假返回值、只检查文本不执行。若有则 FAIL。
- 统计口径：分别报告独立场景数与断言数；PASS/FAIL/N/A 分列，N/A 不计入 PASS。
- 记录实际命令、退出码、输入/产物 SHA-256、日志；绑定最终 HEAD。

## 验收

- 对未修复基线 `8c0fc15` 的复现与最终 HEAD 行为对照，证明问题确实变化。
- 原 5 个 closed-loop case 仍匹配 Windows 参考；不重新生成参考来适配错误结果。
- 输出 PASS/FAIL/N/A、未测项、对最终提交的结论；失败不修，保留证据。
- 检查 main、旧 worktree、旧审查证据未被本轮修改。

## 完成报告

- 被验 commit、命令/退出码、哈希、逐项结果、场景/断言计数、未测项、结论。

## 完成报告（2026-11-05）

- 被验 HEAD `b4052dd`（代码 HEAD `7487e04`，两者只差报告文件）；基线 `8c0fc15`；QA worktree `.worktrees/p0-core-fix-02-qa`；构建 `.local/qa-build-core-fix-02`。
- `ctest` 3/3 PASS；原 closed-loop 5 case 22 PASS、`reference.diff` 0 字节；最终 semantics 489 PASS/0 FAIL；empty AOT 22 PASS/0 FAIL；基线 semantics 364 PASS（对照）。
- 零 AOT：基线独立复现 rc=0 但非法 IR/clang 失败；最终合法空表实际编译/链接/加载，经 `aev_aot_run` → `jit_dispatch` → ORC 执行，`RAX=0x15`、`aot_table_count=0`、`aot_blocks=0`、`jit_compiles=1`、`jit_execs=1`。
- F1：17 个拒绝目录无 `.ll/.h/.json`；terminator LOCK 防御通过。F3：8 个新场景（含完整 AOT→JIT 故障返回）；F4：6 个 push immediate 场景；F5：19 个 JIT + 54 个 helper 检查；均 PASS。
- 测试削弱审计：未发现删除/放宽/屏蔽；原 364 条语义断言保留，新增 125 条/19 场景；N/A 未计入 PASS。Windows MSVC 参考独立编译/运行，输出与提交参考一致。
- QA harness 笔误：空 AOT 输入文件名、AOT→JIT 进程退出码判据；已在 `QA-HARNESS-NOTES.md` 记录并更正，不构成产品失败。
- 结论：范围内 15 PASS / 0 FAIL；N/A 包括 AOT IMUL、原子/TSO、PE/GUI/真实应用/Trace/版本失效/性能/打包等。报告 `.local/results/P0-FINISH-QA-01/REPORT.md`。
