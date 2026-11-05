# OpenCode 协作与交接方案

日期：2026-11-05。目标版本：已在本机隔离目录验证的 OpenCode CLI v2.1.3。

## 1. 人与 agent 的分工

- 用户：决定产品取舍；需要 Windows 交互桌面时协助 GUI 验证。
- 当前规划助手：维护需求、架构、阶段计划，审阅阶段结果并调整下一批任务。
- OpenCode coordinator：读取计划，派发任务，管理依赖和集成，提交可复核的阶段报告。
- OpenCode 实施角色：按任务和公共契约开发。
- QA/reviewer：验证指定提交，报告事实和问题；不能用实施者的自述替代检查。

规划助手与 OpenCode 没有默认共享记忆或自动回调。交接依靠项目文档、任务记录、提交与报告。只有另行实现 CLI/SDK 调度和状态回传后，才具备自动编排；本次没有启动或实现该调度器。

## 2. 现有环境与注意点

2026-11-05 只读检查结果：

| 项目 | 结果 |
| --- | --- |
| Mac | ARM64；macOS 27.0.1；Apple clang 21、CMake 4.4.4、Ninja 1.13.2、Git 2.56.0 |
| CLI | `/opt/homebrew/bin/opencode`，实际输出 `opencode v2.1.3` |
| PATH | `opencode` 先解析到桌面应用内部路径，启动 CLI 时建议使用已核查的绝对路径 |
| Windows | `ssh win` 成功；X64；系统报告版本 10.0.26340 |
| Windows 工具链 | VS 18 Community、C++ x64 工具集、Windows SDK、VS 内 CMake/Ninja 路径存在 |
| SSH PATH | Git/Python 可见；MSVC/CMake/Ninja 未直接命中，构建需初始化 VS 开发环境 |

存在不等于已经编译成功；ENV-01 仍需双机 smoke。没有安装或修改 Windows 工具链。Windows 未在本次 PATH 检查中发现 OpenCode，不要求两台机器都安装 agent：Mac agent 可以通过 SSH 调用 Windows 构建测试脚本。

本机常规 CLI 版本查询最初因沙箱禁止写全局日志而失败；将 XDG 数据/缓存/状态/配置指向临时隔离目录后，版本和 `run --help` 验证成功。未调用模型、读取凭据或验证现有 provider 是否可用。

## 3. 已提供的角色配置

项目内 `.opencode/agents/`：

| Agent ID | 责任 | 主要拥有范围（选型后落实） |
| --- | --- | --- |
| aevotrace-coordinator | 拆任务、接口和集成、阶段报告 | 任务/ADR/公共构建与接口协调 |
| aevotrace-binary | PE、模块、资源、代码发现 | binary frontend、模块身份 |
| aevotrace-compiler | x64 语义、ARM64、AOT/JIT | 执行/编译核心、失效 |
| aevotrace-platform | Windows 语义和双 GUI 后端 | 平台桥接、compat/native |
| aevotrace-qa | 双机测试、差分、性能和产物 | 测试基础与指定版本的证据 |
| aevotrace-reviewer | 独立静态审查 | 只读，无 shell 与文件编辑权限 |

配置不指定模型，使用会话所选模型；不改全局 provider、账号、密钥或桌面设置。worker 不继续派生子 agent，防止任务层级无界扩展。权限与角色约定不等于操作系统沙箱，必须结合工作目录和 worktree 管理。

角色数不等于并发数：先保持主控 + 两个实施任务，QA/审查按需加入。平台模块仍由一个 owner 维护，再按已冻结接口拆 compat/native 子任务。

## 4. 工作目录隔离

OpenCode 的子 agent 会话不能被视为自动拥有独立 Git worktree。写代码的并行任务必须显式分配独立会话、分支和 worktree；同工作目录的 child agent 仅用于不修改源码的调查与审查。

项目已于 2026-11-05 初始化 Git，主分支为 `main`，公开远端为 [PeterWind6174/aevotrace](https://github.com/PeterWind6174/aevotrace)。首次实施时由 coordinator 核对当前基线，再创建任务 worktree，不要重复初始化或创建远端。

建议把 worktree 放在项目内被忽略的 `.worktrees/`，每个使用不同分支、构建目录和缓存。worktree 从同一基线创建；公共接口更新后由主控指定统一版本，不能让各个 agent 自己猜兼容方式。

Windows 使用单独的项目工作目录，按 commit 或源码包哈希同步；必须排除 `.local/`、`.worktrees/`、凭据、构建缓存和不必要的第三方材料。首次选择目录时记录下来，不覆盖机器上的其他项目。

并行 Windows 测试使用任务专属目录和输出。初期可串行执行参考测试，不要求先在 Windows 建立一套完整 agent 集群。

## 5. 每个任务的输入和输出

任务文档采用 [TASK_TEMPLATE.md](tasks/TASK_TEMPLATE.md)。开始前包含目标、输入版本、接口版本、允许修改文件、排除范围、验收步骤及 owner。

完成时提供：

1. 分支、commit 或可核查 diff，以及实际修改范围。
2. 双机使用的代码/二进制身份和配置。
3. 执行过的命令、退出码、结果摘要和原始日志位置。
4. 未覆盖内容、失败项、需要用户 GUI 验证的具体步骤。
5. 接口变更和剩余问题；未完成时不能标成完成。

报告和任务状态保存在项目文档；原始大日志放忽略的结果目录并用 manifest 引用。外部讨论和聊天会话不是唯一证据。

## 6. 集成规则

1. coordinator 先冻结当前阶段的契约 owner 和任务边界。
2. 实施 agent 在分配的 worktree 中修改并运行对应测试。
3. reviewer 检查差异、回归与需求覆盖，QA 在指定提交运行验收。
4. coordinator 顺序集成，处理冲突；不并行修改主分支的 ABI、格式或公共构建入口。
5. 集成提交重新做必要的跨模块验证，再发布阶段报告。

两个实现分别通过单测，不等于组合通过。reviewer 的只读配置无法运行测试，由 QA 提供测试证据。

任务超时或失败后，主控先查看日志、保留已完成结果，再决定重试、缩小任务或调整方案；不要无限重启同样的失败任务。

## 7. Windows 测试与 GUI 协作

远程脚本使用参数文件或 PowerShell `-EncodedCommand`，避免多层 shell 展开破坏脚本。MSVC 从 VS 开发环境脚本初始化，工具版本和构建命令写入报告。

大部分编译、单元测试和业务结果比较可通过 SSH 自动完成。GUI 请求必须提供：应用版本、启动方式、所在会话、操作步骤、预期结果和需要回传的证据。SSH 非交互会话中未看到窗口，不能立即归因于转换器错误。

用户确认之前，GUI 项保持“自动部分已测、人工部分待验收”。需要操作时再通知用户，不把整轮开发都阻塞在人工观察上。

## 8. 启动方式

以下是已核对 CLI 帮助支持的 OpenCode 命令，不是 Aevotrace 构建命令。先确认在项目目录：

```sh
cd /Users/peterwind/Script/Systems/Aevotrace
/opt/homebrew/bin/opencode
```

在桌面端打开同一项目后，可以让当前主 agent 阅读本文件并使用项目角色。桌面端实际连接的 server/version 与角色发现尚未验证，不能仅凭 CLI 版本认定一致。

需要显式选择主控时，可在终端发送首轮任务：

```sh
/opt/homebrew/bin/opencode run --agent aevotrace-coordinator \
  '阅读 docs/需求规格.md、docs/项目构想与技术路线.md、docs/开发计划.md 和 docs/OpenCode协作方案.md。只执行 P0：核对现有 main 基线并建立任务记录，再做范围调查和最小技术验证。实施任务必须显式隔离 worktree；不得以兼容模式替代 native 需求。提交 P0 报告，不提前宣称阶段通过。'
```

不要将临时探测用的 XDG 隔离目录用于正常开发，它没有用户的 provider 配置。若正常启动有权限或 provider 问题，先修复实际环境，不能把 agent 配置文件存在当成集群已启动。

## 8.1 P0 实际执行补充（2026-11-05）

- 两个写代码任务使用独立分支/worktree/子会话：`p0/engine-spike`（`.worktrees/p0-engine`）、`p0/ui-spike`（`.worktrees/p0-ui`）。子会话不等于 worktree，任务提示中明确要求所有写入/提交只发生在其 worktree。
- 另用已验证的 CLI 启动独立调查 worker：cwd 设为 `.worktrees/p0-trace`（分支 `p0/trace-investigation`），命令形式为
  `opencode run --agent aevotrace-binary --auto --title '<TASK>' '<明确任务、cwd、退出条件>' > .local/results/<TASK>/cli.log 2>&1`。
  该 worker 只写 `.local/results/`，完成后退出；未递归启动 coordinator。CLI 日志和 shell 句柄记录在 `.local/results/COORDINATION.md`。
- Windows 默认 SSH shell 是 PowerShell 7.6.9：未包装时远程 `exit 5/7` 会被塌缩成本地 1。后续自动化使用
  `ssh win 'cmd.exe /c <script>; exit $LASTEXITCODE'`，或读取脚本写出的 `*.exit.txt`。超时用本地包装器强制终止并记录退出码。
- Windows 交互桌面在 `console` 会话 1（`PeterWind`）；SSH 落在 services/session 0。GUI 参考运行需要用户在会话 1 执行，或由后续会话内启动工具完成；纯 SSH 看不到窗口不等于程序失败。
- Mac 当前进程已验证 `screencapture` 与 System Events 窗口读取可用；仍需在报告中区分自动截图/日志与人工观感验收。

## 9. 后续自动调度

首轮先验证角色加载、一个子任务交接、指定 worktree 的独立会话、SSH 参考测试和报告返回。稳定后再通过 OpenCode CLI 或当前版本 SDK 建立队列、任务依赖、状态、超时和失败恢复。

桌面端适合查看会话和介入，CLI/SDK 适合可复现调度。不要同时建立两套竞争的任务状态源。Git/任务文件承载工程状态，OpenCode 会话承载执行上下文。

官方依据：[V2 Agents](https://opencode.ai/v2/docs/agents/)、[CLI](https://opencode.ai/v2/docs/cli/)、[CLI Commands](https://opencode.ai/v2/docs/cli/commands/)、[Permissions](https://opencode.ai/v2/docs/permissions/)。这是 2026-11-05 查阅的文档；V1/V2 配置字段不同，后续升级要重新核对。
