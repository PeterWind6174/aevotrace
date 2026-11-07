# 给 aevotrace-coordinator：P0-PE-SESSION-FIX-01

你是 Aevotrace coordinator。执行一次有界的 PE session 正确性修复，完成实施、独立 QA 与只读审查；通过后停在集成分支供外部复核。

## 1. 先读与基线

项目根目录 `/Users/peterwind/Script/Systems/Aevotrace`。读取：

- `AGENTS.md`、`docs/需求规格.md`、`docs/项目构想与技术路线.md`、`docs/开发计划.md`、`docs/OpenCode协作方案.md`、`docs/OpenCode弹性调度.md`。
- 基线工作树 `.worktrees/p0-pe-gui-01` 的 `docs/tasks/P0-PE-GUI-01.md`、`docs/p0/PE-SESSION-CONTRACT-20261107.md`、`docs/p0/P0-PE-GUI-01-REPORT-20261107.md`。
- 外部复核：`/Users/peterwind/Script/Systems/Aevotrace/.local/results/CODEX-PE-GUI-REVIEW-20261107/REPORT.md`、两个 probe 的源码/日志、`review_manifest.json`。不要只读实施报告结论。

基线 HEAD：`5e79398902eedf4ee2e369136fa9ee1a460eea10`，最终产品源码 `c5adf4fc26c3f360c9931d3833aa45d96eedcd7a`；两者之间只有文档变化。先核对实际 HEAD/clean 状态。根目录 main 仍是旧版本，不从 main 开始实现，也不合并根目录未提交材料。保留全部原 worktree 和证据。

当前主链路已由外部独立复跑：7/7 ctest、session 233、PE 149、语义 791、base GUI 560/561、supplemented 596/597；固定 DLL 真正进入共享核心，100 次按钮同一实例；补充后各 2534 AOT 块、零 JIT。但是 F1 有悬空回调风险，整轮不能标为无阻断完成。

## 2. 执行方式与分工

所有开发、测试、审查会话使用桌面端 **OpenCode Go / DeepSeek V4.1 Flash / Max**，核对实际会话元数据，不能靠提示文字或默认继承推定。不能确认时报告配置问题。不用 `opencode run`，不启动其他 CLI 模型 worker，不修改全局账号或密钥。Git、构建、SSH 可正常用。

从上述基线创建新集成分支与 worktree（可用 `codex/p0-pe-session-fix-01`）。按 `docs/tasks/TASK_TEMPLATE.md` 建 `P0-PE-SESSION-FIX-01` 任务卡，先冻结错误分类和所有权契约，再写实现。

本轮最多两个独立写入会话已足够：

- platform：拥有 Python wrapper 和 wrapper 专项测试，负责 F1。
- compiler：拥有 PE session core 及对应 C ABI/runner 专项测试，负责 F2。
- coordinator：公共 CMake、契约、集成、报告；出现共享文件先协调，顺序集成。
- QA：独立构建集成后的确切源码提交；reviewer 只读审查。可以按需要增加独立 QA 原生会话，无需再开第三个实施者。

每名写入者独立 worktree/分支/build/cache，worker 不派生下一层。桌面无法确认绑定隔离时串行实施，不用 CLI 绕过。

## 3. F1（P1）：observer 失败路径的 Python 生命周期

位置：`spikes/p0-ui/harness/pe_session.py` 的 `set_observer` / `clear_observer`。

已复现：注册旧 observer 后，在非 owner 线程 clear，C ABI 返回 WRONG_THREAD=13 并保留旧注册，wrapper 却已清空 `_observer_callbacks`、替换 `_observer`；gc 后旧 CFUNCTYPE 弱引用死亡。C 还持有函数指针，后续执行存在 UAF/崩溃风险。旧测试直接调 C ABI，没有覆盖这个 wrapper 失败路径。

实现要求：

- 新的 observer/callback/context 的 Python 资源先作为局部候选构建；旧绑定保持强引用，直到 C ABI 明确成功替换、清除，或成功销毁。
- C ABI 返回错误、候选构造异常、handle 校验异常时，旧 wrapper 绑定与 C 注册一致；不要在失败分支把旧绑定清掉。
- 不吞掉 WRONG_THREAD/REENTRY，不更改 C 层正确的 owner/reentry 策略，不通过禁用 observer 解决。
- 成功替换/清除/destroy 后可释放不再使用的旧资源，不形成无限增长的保活列表。

必须使用真正的 `harness.pe_session.PeSession` + 实际 dylib 测试：

1. 非 owner 的替换、清除均返回 13；原回调仍存活，回 owner 后实际 Guest call 仍进入原回调，新回调未生效。
2. 在 `on_call_end` 与 `on_host_import` 内尝试替换/清除均返回 10；原绑定保持，后续 Guest 调用正常。
3. 成功替换后仅新回调运行；成功清除后无回调；成功 destroy、重复创建/清除、gc 压力下无保活泄漏或悬空指针。
4. 覆盖只有 call_end、只有 host_import、两个都有的配置；异常路径不改变业务结果或计数。

测试不能始终用额外变量保留旧 callback，从而掩盖 wrapper 本身丢失引用。旧版本上用弱引用/存活断言先可靠失败；不要为了证明问题主动解引用悬空地址使整个测试进程崩溃。将修复对应测试纳入常规 headless 回归，不能只放 `.local` 手工 probe。

## 4. F2（P2）：HostImport 实现与 registry arity 必须一致

位置：`spikes/p0-engine/src/pe_session_core.cpp` 的 HostImport 绑定循环。目前只检查 id=1，随后直接安装固定 args=2 的实现。

已复现：同一固定 DLL，无 AOT、load base `0x180000000`，registry 为 `aevhost.dll|AevUpdate|1|1|win64-u64` 时，create/call 都成功，并调用两参数实现。不能把不一致声明当作正确绑定。

实现要求：

- 固定支持签名由明确的实现描述定义，绑定时验证 id/实际支持参数个数/ABI 与声明一致；不支持时 fail closed，诊断包含期望/实际签名。
- 契约明确：解析合法但无匹配实现的签名返回 session UNKNOWN_IMPORT=5；runner 仍按既有 create/load 规则映射 LOAD=3。先更新契约再落实现。
- 不支持的参数声明不得靠忽略参数、补默认值、假返回值或重写 DLL 业务掩盖。
- 正常 registry、固定 DLL 与原始 DLL/helper 二进制完全保持；不扩大 HostImport API 支持集。

测试至少包括：args=0/1/3/4 被拒绝、args=2 正常；unknown ID 仍拒绝；失败不泄漏 slot/半 session，随后有效 session 可正常创建；runner 映射符合原契约。使用同一真实 PE DLL，不能只单测字符串比较。

注意外部复核已纠正原 reviewer 的笼统说法：registry parser 已拒绝非 `win64-u64`，该负例当前返回 LOAD=3，不要误报成 ABI 未校验。保留该负例证明已有防护；不无故改动错误优先级或现有 ABI。

## 5. 回归和证据

- F1/F2 对应测试须在原源码上失败、在修复后通过，记录确切版本、命令、退出码。F1 测试必须经过真实 Python wrapper。
- 保留原 7 个 ctest 和原断言；session 原 233、PE 原 149、语义 791、Trace/AOT 78 均不得缩减。必要时增加 wrapper 专项 ctest，不拿新增计数掩盖旧断言丢失。
- 在同一最终源码提交，独立 QA 复跑 compat/native 的 base → 实际 GUI Trace → 离线 supplement → 新进程 supplemented。load_count=1、100 次按钮状态、错误后恢复、2534 AOT/零 JIT 等按实际结果核对；若计数变化解释原因。
- 原六组 COFF GUI 做相关回归，特别确认 wrapper/core 改动没有改变旧入口；forced native failure 无 compat 回退。
- 继续真实机器码执行、共享 PE core、C ABI 薄层、两种 GUI 后端；不增加 runner 子进程桥、Unicorn 或 Host 业务副本。
- 若仅 wrapper/Host 绑定变更且固定 DLL/helper 未变，可复用已绑定哈希的 Windows reference，明确“本轮未重跑”；如果 Guest 输入/预期业务或 Windows 参考代码变化，须通过 `ssh win` 新跑并绑定哈希。
- 全部会话使用指定模型/档位；记录会话配置证据与能确认的层级，不声称检验了不可见的 provider 请求。
- 实施一份证据，最终 QA 一份证据；复用自己的 build 增量构建。不得删除、覆盖、压缩旧轮证据或清理已有 worktree；不恢复昨晚全部归档。不无故保存多份相同 100 次循环的大 stderr。

## 6. 收口

提交代码、可重复运行的针对性测试、更新后的契约/任务卡及 `docs/p0/P0-PE-SESSION-FIX-01-REPORT-20261107.md`。报告明确 F1/F2 的旧版本失败/修复后结果、最终源码提交、DLL/dylib/manifest/trace 哈希、独立 QA/reviewer 结论和未测项。

同步修正上一轮证据索引/完成状态：原“无阻断”是原评审结论，外部复核新发现 F1，不能继续把旧版本视作最终验收通过；保留历史，不抹除。`parse_exports` 已排序，未提供反例前不要为原报告的 trace 排序猜测做多余修补。长导出名、少见错误优先级和 runner 不可达分支保留单独 backlog，不借本轮扩大重构。

本轮不开展通用 EXE、Win32 窗口 API、`.app` 打包、真实第三方应用或性能优化，不改 ABI 编号/布局。完成后停在新集成分支，报告待外部复核；不自行合 main、push、清理旧 worktree 或启动下一轮。
