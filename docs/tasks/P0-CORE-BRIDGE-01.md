# P0-CORE-BRIDGE-01：可复用 ENGINE 执行桥

状态：计划已准备，尚未派发。

## 输入与责任

- 阶段/需求：P0 收尾；R01/R06/R08；前置 P0-CALLBACK-ABI-01。
- owner：compiler（lead）+ platform；QA 独立复跑，reviewer 审查。
- 基线：从派发时 main 新建隔离 worktree；不使用已删除的旧 worktree。
- 允许范围：核心必要的薄接口/共享调度抽取、回调 fixture、桥接测试及构建记录。

## 目标

- 导出可由 GUI 使用的最小执行接口，按共同契约调用真实 x64 Guest 代码。
- 提供 Guest 内存受检查读写、HostImport 调用、明确错误返回和重复调用生命周期。
- 共用现有 AOT/JIT/helper，不复制第三套 ORC dispatcher，不手工重写 Guest 业务。
- 在无 GUI 测试中先完成 Host→Guest→Host→返回，证明正常 AOT/JIT 路径与负例。

## 验收

- 原 ENGINE ctest 不回归；新桥测试实际进入生成的 ARM64 代码。
- 至少覆盖输入变化、重复调用、Host helper、未知 helper、非法内存、未支持重入或调用状态。
- Windows 参考与 Mac 使用同一固定哈希的机器码输入；允许元数据/fixture 限制但如实说明。
- 最终提交绑定命令、退出码、输入/产物哈希和独立 QA。
- 不包含 GUI 后端实现、完整 PE/Win32、Trace/失效或性能产品化。
