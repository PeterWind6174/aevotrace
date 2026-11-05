# P0-CORE-FIX-REVIEW-01：F1–F5 修复的独立只读审查

状态：完成（无代码级阻断；建议合并到 `p0/engine-spike`）。

## 输入与责任

- 阶段/需求：P0 收尾；R01/R06；前置 P0-CORE-FIX-01 / P0-CORE-FIX-QA-01。
- owner/reviewer/QA：reviewer / reviewer / qa。
- 审查对象：`p0/core-fix-01` HEAD `8c0fc15087221e408e84f4b7adf50fc67f2a9abf`（代码 `9f21649`），基线 `a4ef11c`。
- 输入：`docs/tasks/P0-CORE-FIX-01.md`、`P0-CORE-FIX-QA-01.md`、`spikes/p0-engine/REPORT-CORE-FIX-01.md`、`.local/results/P0-CORE-FIX-QA-01/REPORT.md`、`fix.patch`。
- 模型/档位：coordinator 会话库核对 reviewer 会话为 `opencode-go/deepseek-v4.1-flash` variant `max`。

## 目标与范围

- 只读审查 F1–F5 实际 diff、语义、测试覆盖与证据；不运行测试、不修改文件。
- 重点：LOCK 拒绝、IMUL 源操作数、故障副作用/精确 PC、POP RSP、64 位 IMUL flags、测试是否被削弱。
- 不包含：P0 其他未完成能力、PE/GUI/真实应用、性能、法律意见。

## 验收

- 给出阻断/非阻断问题、合并建议、未审查范围。
- 测试事实以独立 QA 为准；reviewer 不声称独立跑过。

## 完成报告

- 结论：无 F1–F5 代码级阻断；建议合并到 `p0/engine-spike`，不直接合并 main。
- F5 AOT 生成 IMUL：按 spike JIT-only 设计为已披露限制；AOT dylib 导出的 helper 已测，AOT 不生成 IMUL block。
- 非阻断：F3 faulting arithmetic/flags 与 JIT dispatcher 回归、F1 无 `.ll/.h` 断言/terminator 第二道检查、F4 push imm、F5 JIT 预置 CF/OF、QA N/A 计数、最终 HEAD 绑定、零 AOT block 非法 IR、one-operand 诊断文案、报告文件白名单。
- 完整意见：`.local/results/P0-CORE-FIX-01/REVIEW.md`。
