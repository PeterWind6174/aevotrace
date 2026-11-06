# 给 aevotrace-coordinator：最小 PE32+ DLL 装载与共享核心执行

执行 P0-PE-DLL-01：把当前固定 COFF 函数原型推进到“真实 Windows x64 DLL 输入 → 有界 PE 装载/重定位/导入导出 → 共享 AOT/JIT 核心实际执行”。完成本轮契约、实现、独立 QA/reviewer 和报告后停止，供外部复核。

## 1. 基线与先读

- 最新已接受分支：`p0/promotion-fix-01` / `p0/promotion-fix-integration`，HEAD `aed3b46435bff4347966be03b33cf7013d1aa2d8`。最近行为实现为 `17ec718`、`fc39482`；QA 指定版本 `66a63fb`；后续为文档更新。
- 外部复核：`/Users/peterwind/Script/Systems/Aevotrace/.local/results/CODEX-PROMOTION-REVIEW-20261106/REPORT.md`，F1 已关闭。main 仍 `c91a3f7`，缺少最近 callback/Trace/AOT/provenance 集成，不能从旧 main 丢失成果。
- 开始前核对 refs、worktree 和未提交修改。从上述接受版本新建本轮独立分支/worktree，保留既有工作树和证据。根目录新文件 `docs/prompts/P0-PE-DLL-01.md` 是本次用户交接，可能未提交；根目录四项既存调度/提示词修改已在接受分支中包含对应版本，先比对，不重复覆盖或带入无关修改。
- 先读 AGENTS.md、需求规格、技术路线、开发计划、OpenCode协作方案、OpenCode弹性调度，以及当前 callback/Trace/artifact/provenance 契约、P1-INDEX、支持矩阵。本轮是有界前置实验，不宣布 P0 全部通过，也不以完整 P1 契约草案阻塞最小接口。
- 用 TASK_TEMPLATE 建立计划/任务；先确认基线、owner、文件范围、契约版本、支持/拒绝清单、验收，再写实现。更新陈旧的“仍未集成/本轮未派发”状态，保留历史版本结论。

## 2. A 阶段：模块、地址、内存与 artifact 最小契约

coordinator 协调 binary（模块身份/PE owner）、compiler（执行/内存 owner）、platform（导入桥接口）会签，reviewer 审查后实施：

- 区分文件偏移、RVA、preferred ImageBase、实际 Guest load base、Guest VA、Host 存储地址、Host ARM64 函数地址。定义原始 DLL SHA-256、模块/映射实例、export RVA、块字节身份、加载配置和代次；不持久化 Host 裸指针。
- 先审计现有 `aev_load/aev_store` 单段 identity-mapped 内存和 bridge 假设。PE image、数据、栈与 HostImport 的 Guest 地址必须经一致的受检查访问模型到 Host backing storage，不能直接把任意 Guest VA 转 Host 指针，也不能用 MAP_FIXED 覆盖 Host 已有映射来凑地址。
- 定义 image/stack 的范围、零填充、读写/执行权限与跨边界访问语义。至少拒绝未映射访问、只读段写入、非执行段取指和越界/溢出；故障位置与后续副作用沿用现有正确性要求。
- 定义 relocation 和 IAT 绑定发生的时机、持久化前后的字节身份以及 AOT/JIT 消费视图；原始 DLL 保留。不能以修改输入文件让哈希看起来匹配。
- 定义 AOT/Trace/provenance 对 module SHA、RVA、Guest base/配置与 ABI 的绑定。首版实验可以每个 Guest base 单独生成绑定的 AOT；若暂不支持同一产物重新绑定，必须明确拒绝基址错配并重建，不能冒称跨基址复用。
- 如果 Guest/helper/bridge 语义需要变化，先协调版本与全部 producer/consumer/test 的更新，旧不兼容产物明确拒绝/重建；不要沿用同一版本掩盖变化。保留原 COFF 回归行为。

## 3. B 阶段：有界 PE frontend 与装载

- 输入只需最终 PE32+ AMD64 DLL 及声明的依赖/配置；解析 DOS/PE/optional/section headers、SizeOfImage/headers、RVA→section/file 范围、导出表、普通命名导入与 IAT、base relocation。每次整数运算和目录/字符串读取均检查长度、溢出、截断、重叠与范围；拒绝错误架构/格式。
- image 布局按 PE 语义映射，处理原始数据与虚拟零填充，不把文件偏移当 RVA。对本轮支持的 x64 DIR64 relocation 实际应用 load-base delta；未知类型、缺必需 relocation、RELOCS_STRIPPED 且无法按首选基址装载均明确诊断。
- 至少按名称解析导出；按 ordinal、forwarder、delay import、API set 等可先不实现，但必须检测并说明受控边界，不静默跳过。
- 从 PE 字节和可用 PE 元数据定位入口/函数并进行有界代码发现，复用共享 Program/decoder/emitter。不能要求原 COFF/PDB/C 源码才能转换；不能硬编码 fixture 的文件偏移/指令边界，不能抽取预置机器码替代 DLL 的实际字节。
- 普通导入通过显式 capability/import registry 绑定到 Guest 可调用入口，再经已有 HostImport 桥调用 Host；不把 Host 原生函数指针直接写入 IAT 交 Guest 执行。未知模块/导入明确失败，不返回假零。
- 生命周期可限定 create/load → resolve export → call → destroy/unload，单线程、调用边界；释放后旧模块 handle/入口拒绝继续调用，失败加载释放本次资源。暂不要求运行中重装、并发失效或跨模块自动加载闭包。
- TLS callbacks、DllMain/CRT 初始化、SEH/unwind、COM、线程等先不实现；选择无这些运行需求的 fixture，对需要它们的输入给出支持报告/拒绝。区分存在 `.pdata/.xdata` 元数据与执行异常展开，不能把正常元数据存在误说成 unwind 已实现。

## 4. C 阶段：同一真实 DLL 的执行闭环

先在 Windows 通过 ssh win 构建固定自编 x64 DLL：无 CRT/无入口初始化需求，声明导出，至少一个可观察的全局可写状态、一个需要 base relocation 的绝对指针，以及一个真实普通导入。导入可限定自编 `aevhost.dll!AevUpdate` 等最小测试 ABI，不将它宣称为 Windows API 支持；Windows 提供同名自编 helper DLL，Mac registry 只做该声明 helper 的适配，业务计算仍在 Guest DLL。

- DLL 包含分支/循环或整数业务、IAT helper 调用、全局状态读写；导出返回/查询结果。不要只用从不访问 image data 的算术函数证明完整装载。
- Windows reference 通过系统 loader 加载并调用**同一固定 DLL**；Mac 转换输入是该 DLL 的实际机器码/数据。源码仅供 fixture 构建和 oracle 调查，不能在 Mac 重编业务源码替代转换。
- 支持优先验证一个主 DLL + 声明的 HostImport 模块；自动加载任意 Guest DLL 依赖与完整 EXE 启动均不在本轮。报告明确这是有界 PE/DLL 入口，不是一般应用已可运行。
- 接入共享执行核心：真实 DLL 导出经 AOT 执行，未覆盖合法块真实 JIT，再返回 AOT；使用准备集外输入验证行为。继续使用输入哈希/真实执行记录/实际 base artifact 验证，不能开新引擎或用 Unicorn/Rosetta 顶替。
- 在本 DLL 上至少完成一次实际 Trace → 离线补充 → 新进程 AOT 验证；确认被提升块由 JIT 转为 AOT，promoted/base/new 集合来自实际 artifact。若地址/模块契约导致现有工具不适用，更新共用链路而不是复制第三套补充器。

## 5. 验收与范围

1. 固定 DLL/helper/reference 版本、来源、SHA-256、PE headers/sections/imports/exports/relocation 盘点；证明 fixture 存在并实际使用全局数据、IAT、需修补的指针。
2. 至少首选 Guest base 与一个非首选 Guest base 成功运行；核对 relocation 修补值、导出地址、helper 参数、全局状态和结果。每种配置均在新进程加载对应 AOT；不支持 artifact rebind 时提供错配拒绝。Host ASLR 不算 PE base relocation 的替代证据。
3. 至少三组输入，含独立输入和溢出/空输入等适用边界；同一模块实例至少100次导出调用及重复 load/destroy 的状态隔离检查。这是稳定性验证，不作性能结论。
4. 错架构/截断或越界目录、无效 export、未知 import、非法 relocation、基址/模块/ABI 错配、未映射和权限访问错误，明确失败且不产生故障后的业务副作用，不破坏原 DLL/旧产物。
5. 独立 QA 在最终源码提交使用新构建和自己的验证输入/损坏变体；核对 Windows 同 DLL 参考与 Mac 结果、实际 AOT/JIT 路径、原始字节/重定位和模块状态，不只相信实现者报告或其 parser。
6. 保留原5个ctest、provenance反例和 base/首次补充/空新增再补充×compat/native六组44项回归。ABI更新确需改断言时列明等价替代，不删弱测试。此轮不强制把新 PE DLL 连接全部 GUI；原双模式要求及现有 GUI 链保持，PE 驱动 GUI 留后续明确任务。

排除：通用 EXE 启动、完整 Win32/消息泵、真实第三方应用移植、通用依赖图/TLS/CRT/异常运行时、并发自修改失效、持久化 JIT 缓存产品化、app 分发、性能优化。新增依赖须记录固定版本、许可证与集成方式；不提交无权分发的 Windows 系统 DLL/软件。

## 6. 调度、证据与停止

- 仅 OpenCode 桌面端/原生会话，实际核对每个会话 Go / DeepSeek V4.1 Flash / Max，不使用 opencode run/CLI 模型 worker，不改账号/密钥。
- 建议 coordinator + binary + compiler 两个实施 owner；platform 会签，必要时接入导入适配；契约明确后才按不重叠文件/独立 worktree/build/cache 并行，核心内存/公共构建保持单 owner。主控可按需要增开 QA-artifact、QA-behavior/Windows 等独立会话；常态最多2个实施，资源与隔离允许才扩到4个，worker不递归派生。无需仅为此任务新增角色模板。
- 不确定接口先完成契约与最小反例再实现；若遇到超出范围的必要改造，记录具体触发、差距和最小调整，不用占位/改写验收宣称完成。
- 结束提交最终代码/契约版本、真实DLL与依赖SHA、Guest映射/重定位配置、Trace/AOT/provenance、实际命令/退出码/原始日志、Windows与Mac结果、独立QA/reviewer结论和未测项。停止在本轮集成分支；不自行合入main/push/删除或归档工作树，不自动开始下一轮。

PE 字段、RVA/VA、导入导出与重定位以 [Microsoft PE Format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) 为依据；执行时核查所用规范和工具版本。本文规定本轮验收边界，不声称这些能力已实现。
