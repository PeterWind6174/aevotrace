# P0-CORE-FIX-QA-01：F1–F5 修复的独立复跑

状态：完成（F1–F5 独立执行 PASS；测试未削弱审计 PASS；范围外零 AOT block 缺陷另立任务）。

## 输入与责任

- 阶段/需求：P0 收尾；R01/R06；前置 P0-CORE-FIX-01。
- owner/reviewer/QA：qa / reviewer / qa。
- 输入：ENGINE 修复 commit（派发时写入）；基线 `a4ef11c`；旧审查材料只读。
- worktree：detached `.worktrees/p0-core-fix-qa`，checkout 修复 commit；构建 `.local/qa-build-core-fix`；结果 `.local/results/P0-CORE-FIX-QA-01/`。
- 模型/档位：OpenCode Go / DeepSeek V4.1 Flash / Max；派发时显式指定并在 OpenCode 会话库核对，不能只凭文字。

## 目标与范围

- 不修改产品源码、不 commit、不派生 agent；独立复跑修复后的 ENGINE。
- 原 `ctest`/closed-loop 5 case 必须通过；F1–F5 回归必须实际执行，不能只读测试代码。
- F1：LOCK ADD/INC/AND/SUB/OR/XOR/DEC 等形式显式拒绝；不能只测 XADD。
- F2：二/三操作数 IMUL、dst≠src、正负、8/16/32/64 位；实际 JIT 执行 ARM64 代码。
- F3：load/store/push/pop fault；RAX/RBX/RSP/目标内存无不应有的副作用；`error_pc` 精确；dispatcher 非零错误。
- F4：`pop rsp` 正确；16 位 push/pop 显式拒绝。
- F5：64 位 -1×1、min/max/溢出边界；4/2/1 位回归；不以 ZF/SF/PF 判错。
- 检查测试是否被削弱：删除反例、改期望值、屏蔽断言、假返回值；若有则 FAIL。
- 检查修复 commit 只改允许文件，main/其他 worktree 未被修改。

## 验收

- 每个反例保存输入 `.s/.obj`、IR/产物、命令、退出码、日志、哈希。
- 对照未修复 `a4ef11c` 的复现结果，确认修复后行为改变；不能只测修复后。
- 原 5 个 case 输出仍等于 Windows 参考。
- 输出 PASS/FAIL/未测；失败不修，保留证据。

## 完成报告

- commit、命令/退出码、输入/产物哈希、日志、逐项 F1–F5、未测项、对修复的结论。

## 完成报告（2026-11-05）

- 被验：`c2e49cd`（代码 `9f21649`）；最终分支头 `8c0fc15` 与 `c2e49cd` 仅差报告一行，代码树相同（QA 已核实 `git diff`）。coordinator 已在 QA worktree checkout `8c0fc15`，复跑 `ctest` 2/2 通过，日志 `.local/results/P0-CORE-FIX-QA-01/ctest-final-head.log`。
- 报告：`.local/results/P0-CORE-FIX-QA-01/REPORT.md`；独立驱动 `checks.tsv` 124/124 记录，其中 1 项为 N/A（IMUL AOT by design），实质 PASS 123 + 1 N/A。
- 构建/ctest：configure 0、build 0、ctest 2/2 通过；原 5 case 22 PASS/0 FAIL、`reference.diff` 0 字节，并在 `ssh win` 重新编译/运行参考驱动，输出一致（仅 CRLF/LF）；新 semantics 364 PASS/0 FAIL。
- F1–F5：PASS（LOCK 10 形式翻译/JIT 拒绝；IMUL 2/3 操作数、16/32/64 位 ORC 执行；F3 load/store/push/pop/ret/call 精确 PC/无副作用，JIT+AOT；F4 pop rsp/16 位拒绝；F5 25 helper + 22 JIT 例，4/2/1 位回归）。
- 测试削弱审计：PASS；原 `run_tests.sh`、fixture、反例未改；新增测试未删除/改期望/屏蔽断言。
- 范围外：零 AOT block 函数生成 `[0 x %BlockEntry]` 非法 IR，基线 `a4ef11c` 同样存在，非本次回归；建议另立任务。
- 未测：Windows 参考重跑、8 位 one-operand IMUL、AOT 生成 IMUL、PE/异常/线程/失效/GUI/性能。
- 模型/档位：coordinator 已从 OpenCode 会话库核对本 QA 会话为 `opencode-go/deepseek-v4.1-flash` variant `max`。
