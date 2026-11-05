# P0-UI-INT-01：ENGINE 核心与双 GUI 后端的最小集成

状态：未开始。

## 输入与责任

- 阶段/需求：P0 收尾；R04/R06/R08；前置 P0-ENGINE-01 `a4ef11c`、P0-UI-01 `847dad7`。
- owner/reviewer/QA：platform（lead）+ compiler（contributor）/ reviewer / qa。
- 基线：`a4ef11c` + `847dad7`；worktree/分支由 coordinator 派发时指定。
- 契约：P1-CONTRACT-01 v0 草案；不得自定 ABI。

## 目标与范围

- 用 ENGINE AOT/JIT 核心替换 UI 的 Unicorn Guest 执行桥，至少跑通一条 Host→Guest→Host 回调 + compat/native 窗口/控件路径。
- 证明 Guest x64 机器码由 ENGINE 实际执行，不是 Host 重写/固定回放；native 真实 AppKit，compat 独立路径，无静默回退。
- 记录 AOT 块/JIT 回退、消息序列、业务状态、线程/模态；未支持 API 明确失败。
- 不包含：完整 PE loader、全部 Win32 API、真实应用、`.app` 打包、性能结论。

## 验收

- Windows 参考与 Mac 目标用同一 Guest 二进制哈希；独立输入/按钮序列变异。
- `native`/`compat` 各至少一条完整回调链；ENGINE trace 显示 AOT/JIT 实际执行。
- 命令、退出码、哈希、日志、截图；人工观感待验收。

## 完成报告

- 提交/diff、命令/退出码、哈希、日志、未测项、接口变更。
