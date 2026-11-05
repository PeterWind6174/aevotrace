# P0-CALLBACK-ABI-01：最小 Host↔Guest 回调契约

状态：计划已准备，尚未派发。

## 输入与责任

- 阶段/需求：P0 收尾；R01/R04/R06/R08。
- 基线：main `02309da`；实际派发时重新核对，保留此后的计划文件修改。
- owner：coordinator；compiler/platform 会签，reviewer 审查。
- 输入：`spikes/p0-engine/`、`spikes/p0-ui/`、[本轮计划](../plans/P0-CALLBACK-INTEGRATION-01.md)。

## 范围与完成物

- 固定整数/指针参数、Guest 状态/栈、返回/故障/HostImport、地址与内存、重复调用、单层回调边界。
- 明确 Host 主线程、不可支持重入的诊断、helper 生命周期、ABI/结构尺寸校验。
- 盘点旧 DLL 的需求，选择旧输入或更小的 x64 fixture，并记录为何这样选择。
- 提交契约与验收方案后再实现，不一次性冻结完整 P1 ABI，也不提前扩展 PE/系统 API。

## 验收

- compiler/platform 使用同一契约，reviewer 确认调用与错误边界可验证。
- 写清 Windows 参考如何执行同一机器码，Mac 如何进入 ENGINE，如何证明 Host helper 和结果来自 Guest。
- 本任务无性能/产品兼容性结论；报告未决项，不把契约文档当成实现完成。

## 完成报告（2026-11-05，A 阶段）

- 基线/工作树：main `c91a3f7`；分支 `p0/callback-integration-01`，worktree `.worktrees/p0-callback-integration`。A 阶段只新增契约、fixture 输入和证据，不含 bridge/GUI 实现。
- 契约：[`docs/p0/CALLBACK-ABI-20261105.md`](../p0/CALLBACK-ABI-20261105.md)。内容包括调用/返回、Guest 内存/栈/shadow space、HostImport、退出原因、生命周期、AOT/JIT 共用、Windows/Mac 同一机器码和 B/C 验收方案。
- fixture：`spikes/p0-engine/testdata/callback/aev_callback_fixture.obj`，SHA-256 `4f6929f7c71c82169295b6675517a839ae9533a85941e2632115bf0ef47328dc`；源码 `aev_callback_fixture.c`。MSVC x64 `/c /O2 /GS- /Brepro /W4`，连续两次构建哈希一致；`.aevtext` 86 字节、27 条指令、5 块（4 AOT + JIT-only `0x140001020`），无 `.aevtext` 重定位。Windows 参考驱动链接同一 `.obj`，输出 alpha/beta/hidden 三组 CASE，helper 固定返回 `op`。
- 转换可行性：使用现有 `aev-translate` 对固定对象生成 `4 aot_blocks / 1 jit_blocks`，无 unsupported instruction；`aev_callback_fixture.manifest.json`、反汇编、section/relocation 文本已随 fixture 提交。
- 会签：compiler `SIGN-OFF`（session `ses_e5452516effeU230iYUz7Sqarq`），platform `SIGN-OFF`（session `ses_e5452516cffeTcnkrlZmejylWj`），reviewer `SIGN-OFF`（session `ses_e543f2eb5ffeE1WTA9hZhVBeS1`；非阻断备注进入 B/C）。实际模型/档位以根 `.local/results/P0-CALLBACK-INTEGRATION-01/session-*.json` 的 `session_v2` 元数据为准。
- 未完成/边界：A 不实现 bridge/GUI，不声称回调链已通；通用 PE/Win32、重入/线程、Trace、版本失效、.app、性能均不在本轮。
