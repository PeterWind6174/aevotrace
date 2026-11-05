# P0-UI-INT-01：ENGINE 核心与双 GUI 后端的最小集成

状态：计划已更新，尚未派发；本轮共同 ABI → 执行桥 → 双 GUI 的第三阶段。

## 输入与责任

- 阶段/需求：P0 收尾；R04/R06/R08；前置 P0-CALLBACK-ABI-01、P0-CORE-BRIDGE-01。
- owner/reviewer/QA：platform（lead）+ compiler（contributor）/ reviewer / qa。
- 基线：main `02309da` 已含 ENGINE `b4052dd`、UI `847dad7`；实际派发时核对最新 main，再创建新 worktree。
- 契约：P0-CALLBACK-ABI-01 的最小试验契约；不要求先冻结完整 P1-CONTRACT-01，不得另建互不兼容的 ABI。
- 详细范围与验收：[下一轮计划](../plans/P0-CALLBACK-INTEGRATION-01.md)。

## 目标与范围

- 新增使用共同 ENGINE 执行桥的集成模式，至少跑通一条 Host→Guest→Host 回调 + compat/native 窗口/控件路径。集成路径不加载/执行 Unicorn，历史 Unicorn 对照路径可保留并明确区分。
- 证明 Guest x64 机器码由 ENGINE 实际执行，不是 Host 重写/固定回放；native 真实 AppKit，compat 独立路径，无静默回退。
- 记录 AOT 块/JIT 回退、调用顺序、业务状态与错误；先单线程主线程上一层回调，不要求本轮迁移全部线程/模态场景，未支持 API/重入明确失败。
- 不包含：完整 PE loader、全部 Win32 API、真实应用、`.app` 打包、性能结论。

## 验收

- Windows 参考与 Mac 目标用同一 Guest 二进制哈希；独立输入/按钮序列变异。
- 旧 DLL 若需要范围外 PE/指令支持，可使用声明过限制的新小型 x64 fixture，不得把 Host 重写业务当作集成；两个后端和 Windows 参考仍使用同一输入机器码。
- `native`/`compat` 各至少一条完整回调链；ENGINE trace 显示 AOT/JIT 实际执行。
- 至少三组输入/操作序列（含独立验证场景），同一实例连续至少 100 次调用；检查状态、错误与生命周期。
- 命令、退出码、哈希、日志、截图；人工观感待验收。

## 完成报告

- 提交/diff、命令/退出码、哈希、日志、未测项、接口变更。
