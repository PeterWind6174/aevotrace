# P0-REVIEW-01：P0 证据、ADR 与 spike 的独立只读审查

状态：完成（审查意见已合并；P0 整体不通过维持）。

## 输入与责任

- 阶段、需求 ID、前置任务：P0；R01–R09；前置 P0-QA-01、P0-ADR-01。
- owner / reviewer / QA：reviewer / reviewer / qa（QA 提供复跑证据）。
- 基线 commit 或源码包哈希：由 coordinator 在派发时写入 ENGINE/UI 实际 commit、ADR 文件哈希、QA 报告哈希。
- 分支 / worktree / Windows 工作目录：只读；不运行 shell、不编辑文件、不派生 agent。
- 公共 ABI / artifact / 平台接口版本：审查 ADR 草案的 v0 契约建议；不冻结。

## 目标与范围

- 可观察的完成结果：独立判断 P0 是否满足开发计划的门槛；指出未证据化、被夸大、互相矛盾或违反需求的内容。
- 允许修改的文件或组件：无；输出审查意见给 coordinator。
- 明确不包含的内容：不修代码、不重跑测试、不替代 QA、不做法律意见。
- 需要的依赖及许可记录：审查各任务报告中的固定版本、许可证、目标架构和组合风险。

## 验收

- 测试程序、输入、准备集和独立验证集：核对 QA 的输入/产物哈希和独立复跑，不采信实现者自述。
- Windows 参考执行步骤：核对 ENV/QA 实际命令、退出码和回传；GUI 人工项是否标待验收。
- macOS 目标执行步骤：核对 ENGINE AOT/JIT 与 UI compat/native 的实际运行证据；是否只是 Host 重写/临时桥接。
- 预期业务输出、状态或副作用：ADR 的每个“采用”决定有证据或明确未决；失败路线有替代方案。
- GUI 自动/人工部分：区分自动证据与人工观察；未完成不得写通过。
- 必须证明真实生效的路径：AOT 独立进程、JIT 真实回退、compat/native 各自真实路径、无 Rosetta、无静默回退。

## 完成报告

- 提交或 diff：无。
- 命令、退出码、环境及配置：不适用；审查基于指定证据。
- 二进制哈希、日志和报告位置：引用。
- 正确性/性能结果：只判断证据是否支持结论；不重算性能。
- 审查问题及修复：按阻断/非阻断列文件/章节、触发条件、影响、建议验证。
- 未测项、已知限制、需要用户操作的具体步骤：
- 后续任务与接口变更：P0 通过/不通过建议、P1 前置条件。

## 完成报告（2026-11-05，终审）

- 审查范围：任务/ADR/报告、ENGINE/UI 报告与 patch、ENV/SCOPE/ABI/TRACE/QA 报告；只读，未运行 shell/未重算哈希。
- 结论：
  - ENGINE 证据可信：实际 COFF `.text` 解码、AOT 独立 dylib、ORC JIT 真实编译并返回 AOT、ASLR、独立输入、显式负例；不是 Host 重写/固定回放/只跑 JIT。限制：COFF fragment/线性内存，无 PE/异常/线程/失效。
  - UI 证据可信：Guest x64 实际执行、真实 AppKit、compat 独立自绘、25 checks、无跨后端回退、`MessageBeep` 负例；不是 Host 改状态/只跑一侧。限制：Unicorn 临时桥、Host-owned dispatcher、`DefWindowProcW/TranslateMessage/DispatchMessageW` 仍有占位返回。
  - W-1/W-2 关闭可信；25 checks、`/Brepro`、1165 字节差异有 QA 证据。
  - P0 整体**不通过**：P0-03/P0-05、R05/P-1、V-1、真实应用、性能三类对照、人工 GUI 未闭合。
  - ADR 只能作为 P1 规划输入；支持矩阵缺失、旧状态表述、WinMerge 备选冲突、UI 状态口径、TLS 字段混用、QA REPORT 旧结论、ENGINE“unsigned”措辞等需修正——已在 coordinator 文档中收口（`docs/p0/SUPPORT-MATRIX.md`、任务状态、证据索引、SUPERSEDED 说明）。
- 阻断项（保留到 P0 收尾/P1）：P0-UI-INT-01、P0-TRACE-SPIKE-01、P0-APP-RUN-01、P0-PKG-01、P0-BUILD-REPRO-01；P1-PE scope/ABI lead/license gate 已在 P1-INDEX 修正。
- 未审查：原始 QA 日志/截图/Windows 交互桌面；不声称独立跑过测试。
