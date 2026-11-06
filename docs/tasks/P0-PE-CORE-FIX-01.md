# P0-PE-CORE-FIX-01：PE 段权限与 SAR 标志位聚焦修复

状态：完成，独立 QA PASS，reviewer ACCEPT WITH NON-BLOCKING NOTES，待外部复核。基线：`p0/pe-dll-01` / `p0/pe-dll-integration` HEAD `00ee8d3a20fc68507adcfe5d7fc1e1007a1b2fe2`（上一轮源码 `f864d97aa1b3bd58c678744f17f1ffb4e254fedb`）。本轮分支 `p0/pe-core-fix-01`，工作树 `.worktrees/p0-pe-core-fix`；本轮代码 `987692be19fdc1057f8bdd4be3ada87d2047e280`。main 仍 `c91a3f7`。

## 输入与责任

- 阶段 / 需求：P0 有界 PE32+ DLL 正确性修复；上一轮报告 `docs/p0/P0-PE-DLL-01-REPORT-20261106.md`。
- 外部审查：`.local/results/CODEX-PE-DLL-REVIEW-20261106/REPORT.md`；F1/F2 原始输入、Windows 参考和日志在同目录。
- owner（计划/建议）：binary F1（PE 段权限/region）；compiler F2（共享 SAR helper/测试）。本轮实际写代码采用单实施者串行：桌面端子会话不能由主控工具明确绑定独立 worktree，按 `docs/OpenCode弹性调度.md` 在无法保证隔离时串行；binary/compiler 以只读复现/审查会签，QA/reviewer 独立验证。公共测试/构建由 coordinator 顺序修改。
- 契约：[PE-DLL-CONTRACT-20261106](../p0/PE-DLL-CONTRACT-20261106.md) v3 + 本轮权限/SAR 修订。
- Guest ABI 2 / callback ABI 3 / artifact ABI 2 / translator ABI 2；本轮不改变 ABI，除非实现发现必须（当前无计划）。

## 目标与范围

可观察结果：

1. F1：R/W/X 全空的 PE section 不再隐式获得 READ。按 Windows/PE 语义映射；零权限区域不可读/写/取指，或装载前明确拒绝。不得按段名/哈希/反例路径硬编码。
2. F2：8/16 位 SAR 在 masked count 超过操作数位宽时 CF 与 Windows x64 一致；保留 5/6-bit count mask、count=0 flags 保持、32/64 位边界；增加真正消费 CF 的条件分支用例。

允许修改：

- `spikes/p0-engine/src/pe.cpp`（F1 权限推导/region 拒绝策略）
- `spikes/p0-engine/src/aev_mem.c`（F2 SAR CF）
- `spikes/p0-engine/scripts/pe_dll_test.py`、`run_semantics_tests.sh`、`testdata/semantics/*`（回归）
- 必要的契约/任务/报告文档
- 不改 Guest/bridge/artifact ABI、公共构建入口，除非出现已记录的必要变更

明确不包含：PE 驱动 GUI、Win32 API、真实应用、打包/签名/性能、运行中失效、TLS/CRT/SEH/COM/线程、32 位/其他架构。

## 验收

- 先复现 F1/F2 原错误并保存红证据；修复后同一输入/独立输入变绿。
- F1：零权限、只读、读写、可执行代码、W+X 拒绝；JIT 与 AOT 下禁止的读/写/取指都明确失败，首个故障位置与错误类型正确，故障后无额外副作用；零权限若选择 load-time 拒绝需记录并保留其余 runtime 权限负例。Windows 用同一修改 DLL 核对系统页属性（不制造原生崩溃）。
- F2：8/16 位正负输入；masked count 0、1、位宽-1、位宽、位宽+1、31、原始 count 掩码为 0；AOT/JIT 用真实 COFF 机器码；只比较有定义的结果/标志位；至少一条 `JC/JNC` 分支消费 CF 的用例改变执行结果；Windows x64 原生对照。
- 原 6 个 ctest（含 PE 110、语义 501、Trace/AOT 78、provenance）全部通过；六组既有 GUI 各 44 项保持；不删弱断言。
- 独立 QA 分 F1/权限与 F2/语义两个会话（或明确串行）；reviewer 只读最终源码提交。原始 DLL/旧产物不变。

## 完成报告

- 报告：[P0-PE-CORE-FIX-01-REPORT-20261106](../p0/P0-PE-CORE-FIX-01-REPORT-20261106.md)。
- 独立 QA：[qa-perm](../../.local/results/P0-PE-CORE-FIX-01/qa-perm/REPORT.md)、[qa-sar](../../.local/results/P0-PE-CORE-FIX-01/qa-sar/REPORT.md)。
- reviewer：ACCEPT WITH NON-BLOCKING NOTES（无阻断）。

- 最终代码/集成分支、契约版本、输入/产物 SHA-256。
- 命令、退出码、日志路径、Windows/Mac 双机输出。
- F1/F2 红/绿证据、AOT/JIT 实际路径、故障位置/副作用。
- 独立 QA/reviewer 结论与未测项。
- 停在 `p0/pe-core-fix-01` / 集成分支；不合 main、不 push、不清理工作树、不开启下一轮。
