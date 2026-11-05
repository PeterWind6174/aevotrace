# P0-FINISH-REVIEW-01：零 AOT block 与回归补强的独立只读审查

状态：完成（无代码级阻断；建议 fast-forward 到 `p0/engine-spike`；非阻断项保留）。

## 输入与责任

- 阶段/需求：P0 收尾；R01/R06；前置 `P0-ENGINE-AOT-EMPTY-01`、`P0-CORE-FIX-02`、`P0-FINISH-QA-01`。
- owner/reviewer/QA：reviewer / reviewer / qa。
- 审查对象：`p0/core-fix-02` 最终 HEAD；基线 `8c0fc15087221e408e84f4b7adf50fc67f2a9abf`。
- 输入：最终 diff、任务文档、实施报告、独立 QA 报告与原始日志。
- 方式：只读；不运行 shell/测试、不编辑文件、不派生 agent；测试事实以 QA 为准，但审查代码与证据是否支持结论。
- 模型/档位：OpenCode Go / DeepSeek V4.1 Flash / Max；coordinator 在会话库核对。

## 目标与范围

- 零 AOT block：最终方案是否消除“rc=0 但产物不可编译”；合法空表是否真的可加载并进入 JIT；显式拒绝方案是否有准确诊断；是否有回归覆盖。
- F3：fault guard 是否覆盖新增 arithmetic/logic 路径；flags 提交顺序；完整 dispatcher 返回路径；精确 PC 与无副作用。
- F1：失败无产物断言是否有效；terminator LOCK 防御是否覆盖所有发射入口；正常翻译入口仍提前拒绝。
- F4：push imm 符号扩展与 RSP 行为；16 位立即数 push 拒绝；原 pop rsp 不回归。
- F5：JIT 预置 flags 测试是否真正证明清零；helper 矩阵与 4/2/1 位回归；未使用未定义 flags。
- 测试削弱与证据绑定：是否删除/放宽原测试；最终 HEAD 与 ctest/哈希/报告是否一致；场景数与断言数是否区分。
- 明确阻断/非阻断问题、合并建议、未审查范围。

## 完成报告

- 结论、逐项判断、阻断/非阻断问题、建议验证、未审查范围、是否建议集成到 `p0/engine-spike`。

## 完成报告（2026-11-05）

- 审查对象 `b4052dd`（代码 `7487e04`）相对基线 `8c0fc15`；未发现代码级阻断；建议 fast-forward 到 `p0/engine-spike`，不直接合并 main。
- 零 AOT 表、F1 terminator 防御、F2 诊断、F4 push immediate、F5 预置 CF/OF 均判 PASS；原测试未见削弱，计数口径基本一致。
- 非阻断：F3 完整故障路径使用 test-only `jit_dispatch` 而非产品 `runner.cpp`；`rflags=0x10` 依赖 IMUL 未定义位；probe 进程退出码固定 0；`emit_aot_module` 对 `jit_only` block 先跳过；空表脚本未直接断言 `@aev_block_count`；二进制哈希为 build-local；reviewer 未独立核对模型/档位。
- 完整意见：`.local/results/P0-FINISH-REVIEW-01/REPORT.md`。
- 未审查范围：PE/DLL/重定位/TLS/异常/回调/版本失效/GUI/真实应用/Trace/性能/打包/原子/TSO/AOT IMUL 等。
