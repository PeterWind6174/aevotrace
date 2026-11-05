# P0-TRACE-SPIKE-01：自建核心的 Guest 记录最小验证

状态：未开始。

## 输入与责任

- 阶段/需求：P0 收尾/P0-05；R03/R06；前置 P0-TRACE-01 方案、P0-ENGINE-01 `a4ef11c`。
- owner/reviewer/QA：compiler（lead）+ binary（contributor）/ reviewer / qa。
- 基线：ENGINE `a4ef11c`；worktree/分支由 coordinator 派发时指定。
- 契约：P0-TRACE-01 字段草案；P1 冻结前不宣称稳定格式。

## 目标与范围

- 在 ENGINE runner 上记录 Guest PC/基本块、间接目标、输入模块/内容哈希、映射代次、代码版本；不持久化 Host 裸指针。
- 用准备集外输入触发新间接目标/分支，证明可重组执行，不是固定回放；记录 AOT/JIT 命中。
- 不支持输入/未实现指令显式诊断；区分采集开销与正常执行。
- 不包含：完整 PE loader、真实应用、性能产品结论。

## 验收

- 固定输入哈希、记录文件/日志、Guest PC→AOT/JIT 路径、独立输入结果。
- 记录能回溯到实际输入字节；QA 可复跑。
- 记录格式草案交给 P1-CONTRACT-01；D6/P0-05 关闭条件写入报告。

## 完成报告

- 提交/diff、命令/退出码、哈希、日志、未测项、接口变更。
