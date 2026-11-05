# P0-FINISH-01 报告：零 AOT block 与回归补强收尾

日期：2026-11-05。状态：**范围内完成并已顺序集成到 `p0/engine-spike`；P0 整体仍未通过**。本报告不宣称首版完成、真实应用支持、GUI/PE 完成或性能提升。

## 1. 基线与提交

- 基线：`p0/engine-spike` `8c0fc15087221e408e84f4b7adf50fc67f2a9abf`。
- 分支/worktree：`p0/core-fix-02` / `.worktrees/p0-core-fix-02`。
- 阶段 A（零 AOT 表）：`9ad06322563b2c5eb953953c65d8b9601901993f`。
- 阶段 B（F1/F2/F3/F4/F5 补强）：`7487e04ed2ff8cc393dba14d3a17ec931c9175af`（最终代码 HEAD）。
- 报告提交/分支头：`b4052dd16c14ec3f529c78032dc9b0e6bd372fbc`（仅新增 `REPORT-FINISH-01.md`）。
- 集成：`p0/engine-spike` 已 fast-forward 到 `b4052dd`；未 push、未合并 main、未删除旧 worktree/报告/证据。
- 主工作树保留上一轮八个未提交协作配置修改（`AGENTS.md`、六份 `.opencode/agents/*.md`、`docs/OpenCode协作方案.md`），未覆盖。

## 2. 模型/档位/工作目录核对

来源为 OpenCode 会话库 `session_v2` 只读查询，不是文字自述；未读取凭据。主会话、实施、QA、reviewer 四个实际执行会话均记录：

```text
providerID=opencode-go  id=deepseek-v4.1-flash  variant=max
```

- 实施：`ses_e54a8e2dbffekfjJ1sbRZqGdZB`，目录 `.worktrees/p0-core-fix-02`。
- QA：`ses_e549c0c22ffeXL47U13vxkPhrd`，目录 `.worktrees/p0-core-fix-02-qa`。
- reviewer：`ses_e54e9ac6affePCElxC3n4yTWfX`，项目根只读。
- 证据：`.local/results/P0-FINISH-01/model-verification.json`、`MODEL-VERIFICATION.md`。
- 未运行 `opencode run` 或 CLI worker；未递归启动 coordinator。该证据证明会话存储的模型选择与目录，不是逐请求服务端审计。

## 3. 零 AOT block（P0-ENGINE-AOT-EMPTY-01）

- 基线独立复现：仅 `imul eax,ecx,7; ret` 的 COFF，`aev-translate` rc=0，生成 `@aev_blocks = constant [0 x %BlockEntry] [zeroinitializer]`；clang rc=1 `expected type`。
- 修复：空表改为合法 `@aev_blocks = constant [0 x %BlockEntry] zeroinitializer`；非空 AOT 分支未改。未增加 IMUL 的 AOT 支持，AOT/JIT 交接不变。
- 实际验证：翻译 → clang 编译 ARM64 object → 链接 `aot_runtime.o` + `aev_mem_aot.o` 为 dylib → 新进程加载 → `aev_aot_run` 空表 miss → `jit_dispatch` → ORC 执行纯 IMUL block。结果 `RAX=0x15`、`aot_table_count=0`、`aot_blocks=0`、`jit_compiles=1`、`jit_execs=1`、`error=0`。
- ctest 新增 `p0_engine_empty_aot`：1 个端到端场景、22 条断言 PASS。
- 非空 fixture 未回归：`guest_aot.ll` `df9aa1d6...`、`guest_aot.o` `5b158e59...` 与上一轮一致。

## 4. 回归补强（P0-CORE-FIX-02）

| 项 | 结果 | 说明 |
| --- | --- | --- |
| F1 LOCK 失败无产物 + terminator 防御 | PASS | 17 个拒绝场景使用独立 `mktemp -d`，`.ll/.h/.json` 均不存在；LOCK RET/JMP 被 emitter 拒绝，普通 RET 通过；正常入口仍提前拒绝。 |
| F2 one-operand IMUL 诊断 | PASS | 修正为 8/16/32/64-bit 实际位宽；保留 `one-operand imul` 前缀；四种位宽均拒绝。 |
| F3 faulting arithmetic flags | PASS | add/inc/neg/and/or/xor 内存目标故障；`rflags` 不提交、目标内存/RAX/RBX/RDX/RSP/RIP 无不应有副作用；精确 PC。 |
| F3 完整 AOT→JIT 故障返回 | PASS | `fault_imul_add` 走 `aev_aot_run` → `jit_dispatch` → ORC；`rc=1`、`error_pc=0x140001003`、IMUL 已提交、故障 add 及后续未提交。 |
| F4 push imm8/imm32 | PASS | `-1 → 0xffffffffffffffff`、`-2^31 → 0xffffffff80000000`；JIT/AOT 通过；POP 恢复 RSP；`pushw $imm` 翻译/JIT 拒绝。 |
| F5 预置 CF/OF | PASS | 19 个 JIT 用例预置 `rflags=0x801`；无溢出清零、溢出保持；host/AOT helper 各 27 例；只以 CF/OF 判错。 |
| 原测试/参考 | PASS | 原 5 case 22 PASS、`reference.diff` 0 字节；原 364 语义断言保留，未发现删除/改期望/屏蔽。 |

新增 19 个独立场景、125 条断言（F1 1、F2 4、F3 8、F4 6）。最终 semantics 489 条断言 = 基线 364 + 125；这是断言数，不是 489 个独立工作负载。

## 5. 测试命令、结果与证据

- `cmake` configure rc=0；build rc=0；`ctest --output-on-failure` **3/3 PASS**（closed loop、core-fix semantics、empty AOT）。
- 原 `scripts/run_tests.sh`：22 PASS / 0 FAIL。
- `scripts/run_semantics_tests.sh`：489 PASS / 0 FAIL。
- `scripts/run_empty_aot_test.sh`：22 PASS / 0 FAIL。
- 合计 533 条断言 PASS、0 FAIL。
- 独立 QA：15 项 PASS / 0 产品 FAIL；Windows MSVC 参考独立编译/运行，5 行输出与提交参考（去 CRLF）逐字节一致；旧 6417 个证据文件未变（排除 `.DS_Store`）。
- QA harness 两处笔误（空 AOT probe 文件名、AOT→JIT 进程码判据）已在 `.local/results/P0-FINISH-QA-01/QA-HARNESS-NOTES.md` 记录并更正，不构成产品失败。
- 关键哈希：
  - `guest_x64.obj` `a12fa7ff...`
  - `reference_stdout.txt` `7d623c9a...`
  - 空 AOT 输入 `5088c4ea...`；修复后 `.ll` `a2a4e8d7...`；`.o` `304261c3...`
  - 非空 `guest_aot.ll` `df9aa1d6...`；`guest_aot.o` `5b158e59...`
  - 完整列表：`.local/results/P0-FINISH-01/final/MANIFEST.sha256`、`.local/results/P0-FINISH-QA-01/MANIFEST.sha256`。
- 证据目录：实施 `.local/results/P0-FINISH-01/`；QA `.local/results/P0-FINISH-QA-01/`；reviewer `.local/results/P0-FINISH-REVIEW-01/REPORT.md`；diff `.local/results/P0-FINISH-01/finish.patch`。

## 6. 独立 QA 与 review

- **QA**：`.local/results/P0-FINISH-QA-01/REPORT.md`。结论：范围内 PASS，0 FAIL；基线 `8c0fc15` 空表错误独立复现；最终 HEAD 合法空表实际 JIT 回退；F1/F3/F4/F5 回归实际执行；未发现测试削弱。N/A 包括 AOT IMUL、原子/TSO、PE/GUI/真实应用/Trace/版本失效/性能/打包等。
- **Reviewer**：`.local/results/P0-FINISH-REVIEW-01/REPORT.md`。未发现代码级阻断，建议 fast-forward 到 `p0/engine-spike`，不建议直接进 main。
- **Reviewer 非阻断项**：
  1. F3 完整故障路径使用 test-only `jit_dispatch`，未走产品 `runner.cpp` 的故障分支（代码审查语义一致）。
  2. F3 `rflags=0x10` 依赖 IMUL 未定义位，测试对实现细节较敏感。
  3. `aev-aot-jit-probe` 进程退出码固定 0；调用方需解析 stdout `rc`。
  4. `emit_aot_module` 对 `jit_only` block 先跳过，手工 `jit_only` LOCK terminator 不会触发 AOT 发射拒绝；正常路径无影响。
  5. 空表脚本未直接断言 `@aev_block_count`。
  6. 可执行文件/空表 dylib 哈希为 build-local；源码/IR/object 哈希可复现。
  7. reviewer 未独立核对模型/档位；已由 coordinator 会话库核对。

## 7. 集成与当前状态

- `p0/engine-spike`：`b4052dd`，clean；已在 `.worktrees/p0-engine/.local/build-integration` 重新 configure/build/ctest，3/3 PASS，日志 `.local/results/P0-FINISH-01/integration/`。
- `p0/core-fix-02`：`b4052dd`，保留；QA worktree `.worktrees/p0-core-fix-02-qa` detached `b4052dd`。
- `main`：本轮文档提交；未合并 main、未 push；`origin/main` 仍为 `373becb`。主工作树保留八个未提交协作配置修改。
- **P0 整体仍未通过**：共同回调 ABI、ENGINE 与双 GUI 最小集成、Trace 实际记录、版本失效、PE/真实应用/性能/打包/人工 GUI 等仍未完成。

## 8. 下一轮最小任务建议（本轮未启动）

1. 共同回调 ABI（Host↔Guest 边界、错误/栈/寄存器契约）。
2. ENGINE 与 `compat`/`native` 双 GUI 的最小集成。
3. 自建核心 Guest PC/基本块/间接目标 Trace。
4. 输入身份/代码版本校验与失效。

这些任务应在上游契约冻结后按依赖顺序推进；不得因为 P0-FINISH-01 完成而宣布 P0 或首版整体通过。
