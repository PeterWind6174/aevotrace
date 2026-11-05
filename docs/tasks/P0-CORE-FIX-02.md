# P0-CORE-FIX-02：F1–F5 回归补强与最终 HEAD 绑定

状态：完成（已集成 `p0/engine-spike` `b4052dd`；独立 QA PASS；reviewer 无代码阻断，非阻断项保留）。

## 输入与责任

- 阶段/需求：P0 收尾；R01/R06；前置 P0-CORE-FIX-01/QA/REVIEW。
- owner/reviewer/QA：compiler / reviewer / qa。
- 输入：`p0/engine-spike` HEAD `8c0fc15087221e408e84f4b7adf50fc67f2a9abf`；reviewer 非阻断列表；QA `REPORT.md`/`checks.tsv`；独立复核 `.local/results/CODEX-CORE-FIX-REVIEW-20261105/REPORT.md`。
- 隔离：分支 `p0/core-fix-02`，worktree `.worktrees/p0-core-fix-02`（从 `8c0fc15` 创建）；构建目录 `.local/build-core-fix-02`；本轮证据 `.local/results/P0-FINISH-01/`。
- 模型/档位：OpenCode Go / DeepSeek V4.1 Flash / Max；每个会话在 OpenCode 会话库核对 `session_v2.model`，不以文字自述代替。
- 执行方式：OpenCode 桌面端原生子会话；不运行 `opencode run`/CLI worker；worker 不派生 agent。

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

## 本轮实施要求（2026-11-05）

- **F3**：新增 faulting `add/inc/neg/and/or/xor [mem]` 回归，断言故障时 RFLAGS 不被提交、目标内存/寄存器/RSP/RIP 无不应有副作用、`error_pc` 精确。至少一个用例走完整 `aev_aot_run` → `jit_dispatch` → 非零返回路径（可以是含 IMUL 的 JIT-only block 后跟故障内存操作，但不得为测试而把 IMUL 改成 AOT 支持）。
- **F1**：`translate_fail` 使用本次运行独立的干净输出目录，断言失败时没有生成 `.ll`/`.h`/manifest；不删除旧证据。emitter 在 terminator 发射入口也检查 `lock_prefix`（逐条检查，不能只依赖 `build_program`）；正常翻译入口仍须在 IR 前拒绝全部 LOCK。若可低成本构造手工 `Program`，增加一个 terminator LOCK 防御负例；否则报告说明为何不可行。
- **F4**：新增 `push imm8`、`push imm32` 成功回归（含负立即数符号扩展到 64 位，`pop` 后检查值/RSP）；新增 `pushw $imm` 明确拒绝负例。若发现真实缺陷，做最小修复。
- **F5**：JIT 用例在乘法前将 CF/OF 置 1，再验证无溢出用例确实清零、溢出用例保持 1；不以 ZF/SF/PF 判错。host/AOT helper 矩阵保持。
- 修正 one-operand IMUL 诊断，按实际位宽报告，不把所有位宽都写成 8-bit；保留 `one-operand imul` 可检索前缀。
- 报告必须区分 PASS/FAIL/N/A，并区分“断言数”和“独立场景数”；不得把同一场景的多条断言算成多个场景。
- 在最终代码 HEAD 上绑定 ctest、输入/产物哈希、命令/退出码和报告；原 closed-loop 5 case、现有 364 项语义断言不得削弱或改期望。
- 不包含：PE loader、GUI、真实应用、Trace、版本失效、原子/TSO 实现、指令集扩展或执行核心重构。

## 完成报告（2026-11-05）

- 分支 `p0/core-fix-02`；阶段 B 提交/最终代码 HEAD `7487e04`；报告/分支头 `b4052dd`；已 fast-forward 集成到 `p0/engine-spike`；未 push、未合并 main。
- **F1**：17 个拒绝场景使用独立 `mktemp -d` 输出目录，断言 `.ll/.h/.json` 均不存在；`emit_terminator` 入口新增 LOCK 检查；`emitter_lock_probe` 手工构造 LOCK RET/JMP 均被拒绝，普通 RET 通过。正常 `build_program` 仍在 IR 前拒绝全部 LOCK。
- **F2**：发现并修正 one-operand IMUL 诊断把 16/32/64 位都写成 8-bit 的缺陷；按实际位宽报告，保留 `one-operand imul` 前缀，8/16/32/64 位输入均拒绝。
- **F3**：新增 6 个 faulting `add/inc/neg/and/or/xor [mem]` JIT 场景、1 个 AOT 场景、1 个 `fault_imul_add` 完整 `aev_aot_run` → `jit_dispatch` 故障返回场景；flags/内存/寄存器/RSP/RIP 无不应有副作用，`error_pc` 精确（第二条指令 `0x140001005`，AOT→JIT 为 `0x140001003`）。
- **F4**：新增 `push imm8 -1 → 0xffffffffffffffff`、`push imm32 -2^31 → 0xffffffff80000000` 的 JIT/AOT 成功回归及 `pushw $imm` 双路径拒绝；未发现产品代码缺陷。
- **F5**：19 个 JIT IMUL 用例预置 `rflags=0x801`，无溢出清零、溢出保持；host/AOT helper 各 27 例；只以 CF/OF 判错。
- 测试：`ctest` 3/3；原 5 case 22 PASS、`reference.diff` 0 字节；semantics 489 PASS/0 FAIL（基线 364 + 新增 125）；新增 19 个独立场景/125 条断言；empty AOT 22 条断言/1 场景。未发现测试削弱。
- QA：`P0-FINISH-QA-01` PASS（15 项 PASS/0 FAIL；两处 QA harness 调用笔误已记录，不影响产品结论）。Reviewer：无代码级阻断，建议集成；非阻断项见 `P0-FINISH-REVIEW-01`。
- 未测/限制：F3 完整故障路径使用 test-only `jit_dispatch`（未走产品 `runner.cpp` dispatcher 的故障分支）；`rflags=0x10` 依赖 IMUL 未定义位；`aev-aot-jit-probe` 进程退出码固定 0；`emit_aot_module` 对 `jit_only` block 先跳过；空表脚本未直接断言 `@aev_block_count`；二进制哈希为 build-local。PE/GUI/真实应用/Trace/版本失效/性能不在范围。
