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
