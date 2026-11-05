# P0-FINISH-QA-01：零 AOT block 与回归补强的独立复跑

状态：未开始（等待 `p0/core-fix-02` 最终提交）。

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
