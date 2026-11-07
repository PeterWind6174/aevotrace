# Aevotrace 工程协作约定

## 先读

读取 `docs/需求规格.md`、`docs/项目构想与技术路线.md`、`docs/开发计划.md`、`docs/OpenCode协作方案.md` 和 `docs/CURRENT_STATUS.md`。产品需求以用户最新指示和需求规格为准；旧 AArch64/Switch 首版设定已经被替换。

## 当前目标

Windows x86-64 EXE/DLL → macOS ARM64 桌面 GUI 应用；AOT 优先且允许 JIT；同时保留 `compat` 与 `native` 两种可选输出。main 已连通固定 PE DLL、共享 AOT/JIT 核心、持久 session、双 GUI 与 Trace→补充 AOT，并合入 P0-PE-SESSION-FIX-01。窗口仍由 Host 测试框架建立；不是通用 EXE/Win32 GUI 转换器，不得写成已支持真实应用、独立 `.app` 或已提速。本批已收口，下一阶段尚未启动；已归档工作树路径不能当作现有工作区，按最新 main 建新任务。

## 开发与协作

- 先完成当前分配任务与前置条件，使用 `docs/tasks/TASK_TEMPLATE.md` 记录范围、基线和验收。
- 写代码的并行会话使用独立分支/worktree、构建目录和缓存。同目录子 agent 只做不修改源码的调查或审查，不假设其自动隔离。
- ABI、artifact 格式、公共构建系统和平台接口变更由 coordinator 协调，先更新契约再分派实现。
- worker 不自行增加子 agent 层级，不同时改动其他任务拥有的文件。
- coordinator 顺序集成；实施者提供证据，QA/reviewer 独立验证指定版本。
- OpenCode 开发、测试和审查使用用户的 **OpenCode Go / DeepSeek V4.1 Flash / Max**。主会话、新会话和子 agent 都需要核对实际模型与档位；不能以未知默认值或别的模型代替。
- 后续通过 OpenCode 桌面端及原生会话/子 agent 工作，不自行执行 `opencode run` 或启动其他 CLI worker。Git、编译和 `ssh win` 等普通工程命令仍可使用。若桌面端不能明确隔离并行任务的 worktree，就串行实施，不用 CLI 绕过该约定。
- 不猜测 Go 的 provider/model ID，不写入密钥或擅改全局账号。仅有文字提示不能证明模型/Max 生效；无法确认时先报告配置问题，不用其他默认模型继续。

## 正确性与证据

- 转换路径必须实际使用输入机器代码，不用 Host 手工重写程序替代转换结果。
- JIT 回退必须真实可用；未实现指令/API 不得用假返回值掩盖。
- `native` 必须实际使用已支持的原生系统/GUI 后端，不静默回退 `compat`。
- 比较准备集以外的输入，验证 DLL/回调、代码版本、重定位和失效。
- 报告绑定代码版本、输入/二进制哈希、环境、配置、命令、退出码与日志。
- Windows 参考机通过 `ssh win` 使用；GUI 需要交互桌面时给用户具体步骤，并保留待人工验收状态。
- 性能测量区分冷 JIT、持久化缓存 JIT 和 AOT Hybrid，不将跨硬件耗时直接作为转换开销。

## 输入与依赖

引入第三方实现前记录来源、固定版本、许可证和集成方式。现有非商业许可不赋予第三方代码的改许可权。不要提交凭据、私人 Trace、无权分发的应用或系统文件；本地材料置于忽略目录。
