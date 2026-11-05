# P0-CORE-FIX-02：F1–F5 回归补强与最终 HEAD 绑定

状态：未开始。

## 输入与责任

- 阶段/需求：P0 收尾；R01/R06；前置 P0-CORE-FIX-01/QA/REVIEW。
- owner/reviewer/QA：compiler / reviewer / qa。
- 输入：`p0/engine-spike` HEAD `8c0fc15`；reviewer 非阻断列表；QA `REPORT.md`/`checks.tsv`。
- 隔离：新 worktree/分支；不与真实应用/API 扩展并行。

## 目标与范围

- F3 增加 faulting `add/inc/neg/logic [mem]` 的 flags 不提交回归；增加 `aev_aot_run`→`jit_dispatch` 的故障返回路径。
- F1 `translate_fail` 断言失败时无 `.ll/.h`；emitter 在 terminator 入口也检查 LOCK。
- F4 增加 `push imm8/imm32` 成功与 `pushw $imm` 拒绝回归。
- F5 JIT 用例预置 CF/OF，验证无溢出时确实清零。
- one-operand IMUL 诊断区分 8/16/32/64 位。
- QA N/A 计数改为 `PASS + N/A`；在最终 HEAD 上重新绑定 ctest/哈希。
- 不包含：PE loader、GUI、真实应用、Trace、版本失效、原子/TSO 实现。

## 验收

- 新测试实际执行，不放宽原断言；原 5 case + 现有 semantics 仍通过。
- 最终 HEAD 的 ctest、输入/产物哈希、QA 报告三者一致。
- reviewer 确认非阻断项关闭或明确保留。

## 完成报告

- commit、命令/退出码、哈希、逐项结果、未测项。
