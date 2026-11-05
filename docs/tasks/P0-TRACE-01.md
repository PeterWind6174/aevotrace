# P0-TRACE-01：准备执行、动态发现与记录可移植性

状态：待审查（仅完成调查/方案；未实现、未运行 Trace，验收未通过）。

## 输入与责任

- 阶段、需求 ID、前置任务：P0；R03/R06；依赖 P0-ENGINE-01/P0-ENV-01。
- owner / reviewer / QA：binary（lead）+ compiler（contributor） / reviewer / qa。
- 基线 commit 或源码包哈希：`586662a4ff09465d7d3aa453fb44e5aa6e880c79` 起。
- 分支 / worktree / Windows 工作目录：调查与后续 spike；报告在 `.local/results/P0-TRACE-01/`。
- 公共 ABI / artifact / 平台接口版本：不冻结；只提出记录格式草案。

## 目标与范围

- 可观察的完成结果：明确准备执行发生在哪里、采集什么、如何绑定输入身份，以及哪些数据不能从 Windows 原生执行直接获得。
- 允许修改的文件或组件：调查/报告；后续 spike 在 coordinator 指定的隔离 worktree。
- 明确不包含的内容：不实现完整动态二进制插桩、不把一次 Trace 固化成回放、不把 Windows 原生执行说成指令级 Trace。
- 需要的依赖及许可记录：ETW/Intel PT、DynamoRIO、Frida Stalker、QEMU TCG plugins、Pin、自建引擎日志等候选的 Windows 支持、架构、许可和干扰成本；未验证的标为未验证。

## 验收

- 测试程序、输入、准备集和独立验证集：至少一个间接调用/动态目标样例；准备集与独立验证集分开。
- Windows 参考执行步骤：原生运行产生业务输出/文件/事件；不声称获得指令 Trace。
- macOS 目标执行步骤：在自建执行核心/临时桥接中记录 Guest PC、基本块、间接目标、代码版本；验证新输入能重新组合执行。
- 预期业务输出、状态或副作用：记录可绑定输入模块、内容哈希、场景和构建；不持久化 Host 裸指针。
- GUI 自动/人工部分：不适用（除非后续采集 GUI 事件）。
- 必须证明真实生效的路径：记录确实来自实际 Guest 执行；不是 Host 手工编写的轨迹。

## 完成报告

- 提交或 diff：
- 命令、退出码、环境及配置：
- 二进制哈希、日志和报告位置：
- 正确性/性能结果：区分采集开销与正常执行。
- 审查问题及修复：
- 未测项、已知限制、需要用户操作的具体步骤：
- 后续任务与接口变更：建议的记录格式、失效规则和 P1 任务。

## 完成报告（2026-11-05，调查阶段）

- 提交或 diff：无产品源码变更；worktree `p0/trace-investigation` HEAD `808216c`，`git status` 为空。
- 报告：`.local/results/P0-TRACE-01/REPORT.md`，SHA-256 `7f840aab400e8f14878dffe8eae978bb13a6bc21eae13aab011ff1967a1b484e`；CLI 日志 `cli.log`，CLI 退出 0。
- 实际执行：`ssh win` 只读环境检查（Windows 11 10.0.26340 x64、Intel i7-9700F、WPR 10.0.26100、ETW 工具存在，DynamoRIO/Frida/Pin/QEMU 未安装）；未启动 Trace、未安装工具、未修改 Windows。
- 结论：Windows 原生执行只能作行为 oracle；SSH 不是 Trace 通道；ETW/PT/DynamoRIO/Frida/Pin/QEMU 均不能直接产出 Aevotrace Guest 级记录；权威采集应由自建执行核心日志完成。报告给出记录字段草案（输入/模块哈希、Guest PC/基本块/间接目标/代码版本、映射代次、字节依赖、不持久化 Host 裸指针）和准备集/独立验证集五维隔离方案。
- 未完成/未验证：无实际 Trace、无间接目标样例运行、无记录格式实现；Frida/Intel Pin/QEMU 插件许可文本未逐字复核；Windows Intel PT 采集接口未验证；开销数据为文献量级，非本项目实测。**因此不满足 P0-05 的“实际记录验证”部分，P0-ADR 的 D6 必须标未决。**
- 后续：P1-TRACE-01 需在自建核心上实现记录并用独立验证集复核；在此之前不得宣称具备指令 Trace 或 AOT 种子采集能力。
