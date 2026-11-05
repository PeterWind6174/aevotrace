# 给 aevotrace-coordinator 的下一轮提示词

你继续担任 Aevotrace coordinator。本轮执行“最小回调 ABI → 共同执行桥 → 双 GUI 最小集成”，不全面展开 P1。

## 先读与基线

阅读 AGENTS.md，以及：

- docs/plans/P0-CALLBACK-INTEGRATION-01.md
- docs/tasks/P0-CALLBACK-ABI-01.md
- docs/tasks/P0-CORE-BRIDGE-01.md
- docs/tasks/P0-UI-INT-01.md
- docs/p0/INTEGRATION-20261105.md
- .local/results/CODEX-FINISH-REVIEW-20261105/REPORT.md

代码已合入 main：ENGINE 来源 b4052dd，UI 来源 847dad7；原 ENGINE ctest 3/3、533 条断言通过。旧 11 个工作树已移除，历史材料归档在 .local/worktree-archives/20261105_195018/，不要使用旧 .worktrees 路径。

核对当前 main HEAD 和工作区，以派发时的实际提交作为基线，保留本轮计划及其他已有修改。从 main 新建任务 worktree。不要重复 Git 初始化或远端创建，不清理根 .local。

## 执行约束

- 只使用 OpenCode 桌面端和原生会话/子 agent，不运行 opencode run 或启动 CLI worker。
- 使用 OpenCode Go / DeepSeek V4.1 Flash / Max；核对主控和每个实际子会话的 provider、model、variant、parent 和 directory，保存安全元数据证据，不以文字自述替代。
- 不读取/复制密钥或登录记录，不擅改全局账号。普通 Git、编译、SSH 可以使用。
- 并行写代码必须隔离 worktree；无法保证时串行。阶段 A/B/C 按依赖顺序做，不先派两个互不兼容的桥接实现。

## 阶段 A：P0-CALLBACK-ABI-01

先盘点 ENGINE 与 UI 当前接口和指令/装载缺口，由 coordinator 汇总、compiler/platform 会签，reviewer 检查最小契约后再实现。

契约只包含本轮需要的整数/指针参数、返回值、Guest 寄存器/栈/shadow space、受检查内存、HostImport ID/Guest 地址、正常/故障/未覆盖等退出原因、helper 返回、生命周期和重复调用。

先单线程、AppKit 主线程上一层 Host→Guest→Host；不承诺任意嵌套 Guest 重入/线程/模态。未支持行为明确诊断。Host 异常不能跨 C/机器码边界；不能持久化 Host 裸指针。ABI/结构尺寸不匹配要能识别。

不要把本任务扩大为冻结整个 P1 ABI、PE、TSO 或异常系统，也不要陷入“完整 P1 契约依赖 P0 集成、P0 集成又依赖完整 P1”的循环。

## 阶段 B：P0-CORE-BRIDGE-01

在共同契约上提供可复用 ENGINE 薄接口，先做不带 GUI 的 Host→Guest→Host 调用测试。

- 复用现有 AOT/JIT/helper，必要时小范围抽取共享逻辑，不复制第三套 ORC dispatcher，不大型重构。
- Guest 业务必须来自输入 x64 机器码，不能在 Host 重新编译/重写业务来冒充转换。
- 先检查旧 UI DLL 是否适合本轮；若需要大量新增指令或完整 PE 支持，允许新增更小的自编 x64 回调 fixture。
- Windows 参考实际执行同一机器码输入，例如链接同一固定哈希的 COFF 对象，或加载同一测试 DLL；记录最终参考程序与 Guest 输入的对应关系。Mac 转换以二进制为必需输入，源码只用于生成和核验 fixture。
- 允许显式入口/模块/HostImport/重定位元数据或有界 fixture loader，但必须披露，不宣称通用 PE loader。
- 验证正常返回、重复调用、Host helper、未知 helper、非法内存、未支持重入和错误传播，结果来自 Guest 计算。

B 通过后再接 GUI，不能用纯 Host bridge mock 宣布阶段通过。

## 阶段 C：P0-UI-INT-01

让同一 Guest 二进制、同一 ENGINE 执行桥分别驱动 compat/native。

最小业务链：输入框/按钮事件 → Host GUI → ENGINE 的 x64 Guest 回调 → Guest 计算 → Host helper 更新结果控件 → 返回 Host。

- 两种模式至少各有一个窗口、输入框、按钮和结果区域。
- native 用真实 AppKit 控件并在主线程创建/更新；compat 使用既有独立后端，不静默互相回退。
- ENGINE 集成路径不加载/执行 Unicorn。可以保留明确标注的历史 Unicorn 对照模式，但不能在失败时偷偷调用它。
- 至少三组不同输入/操作序列，其中一组独立于准备场景；不能在 Host 写死业务结果。
- 同一实例连续至少 100 次回调，验证状态和生命周期；不把这项 smoke 描述成性能测试。
- 正常链路记录实际 AOT 执行和至少一次真实 JIT 编译/执行，不能只打印计数或符号名称。
- 未支持 API、非法内存和未支持重入明确失败；故障不能提交后续副作用。

无需在本轮迁移旧 GUI 的全部线程/模态/消息泵。自动事件与物理输入/焦点/输入法验收分开，需要 Windows 交互桌面时再向用户给出明确版本、操作和预期。

## 验收、集成与停止

- 原 ENGINE 三组 ctest 与既有断言不回归，不改弱原 fixture/参考输出。
- 新契约/桥/GUI 测试保存输入与产物哈希、实际代码 HEAD、模式、命令、退出码/Guest 退出原因及日志。
- 独立 QA 在最终代码 HEAD 复跑 Windows 参考、无 GUI 桥和两个 GUI 后端，核对业务结果和独立输入。
- reviewer 检查实际 diff、共用核心、无 Unicorn/Host 重写、ABI/故障及能力边界。
- 阶段通过后顺序集成到本轮集成分支；等待独立复核再合入 main。不自动 push，不删除 worktree/旧证据。
- 阶段失败时保留结果与具体缺口，不通过缩小成纯 Host 演示或更换执行器宣布通过。

不包含：通用 PE/Win32、任意重入/多线程、真实 7-Zip、通用 Trace、版本失效、独立 .app、性能优化或产品发布。本轮调用日志不等于 Trace 系统已经完成。

完成这条有限集成链后停止，交付：阶段 A/B/C 结果、实际模型/Max 证据、契约和输入限制、提交与测试证据、独立 QA/review、未测项、分支/工作区/集成状态，以及下一轮 Trace/版本失效建议。不要自行启动后续任务，也不要宣布 P0 或首版整体通过。
