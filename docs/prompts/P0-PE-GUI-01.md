# 给 aevotrace-coordinator：持久 PE session 与双 GUI 集成

日期：2026-11-07。执行 P0-PE-GUI-01：抽取共享、持续存活的 PE 调用会话，让同一真实 Windows x64 DLL 的业务经 compat/native GUI 事件进入 AOT/JIT 执行。完成契约、实现、独立 QA/reviewer 和报告后停止，供外部复核。

## 1. 版本与先读

- 最新接受 HEAD：`de67e2be2ea2252bb0e2b522ba52aa5d208e5d21`，refs `p0/pe-core-fix-01` / `p0/pe-core-fix-integration`。产品修复 `987692b`、测试加强 `ac84ae3`；后续文档。开始前核对实际 refs/diff/worktree 状态，从此接受版本新建任务分支/worktree。
- main 仍为旧 `c91a3f7`，不含最近 callback/Trace/AOT/provenance/PE 集成。不能从旧 main 开工丢失成果；不自行 merge/push/清理 worktree。
- 先读 AGENTS.md、四份项目必读文档、OpenCode弹性调度、PE-DLL v3、Guest/helper/bridge/artifact/provenance 契约。
- 交接：`/Users/peterwind/Script/Systems/Aevotrace/docs/交接/2026-11-06-收尾.md`；最新外部复核 `.local/results/CODEX-PE-CORE-FIX-REVIEW-20261106/REPORT.md`，F1/F2已关闭。本提示词和根目录交接可能尚未提交，按范围带入本轮分支；不要把根目录陈旧的未提交配置覆盖接受分支的新版本。
- 昨日20个历史根构建已归档，恢复索引 `.local/cleanup-audits/20261106-closeout/README.md`。最新两套PE构建、results、venv及全部未合并worktree仍在；不批量恢复历史构建或清理既有证据。
- 用 TASK_TEMPLATE 建立本轮计划/任务，明确owner、文件范围、基线、契约、验收、排除项，再实施。阶段名称不代表P0整体或首版已经完成。

## 2. 本轮验收链

```text
compat / native 窗口的真实控件事件
  → 共用 GUI host 薄适配
  → 同一进程内、持续存活的共享 PE session
  → 实际 DLL 导出机器码经 AOT / 未覆盖块 JIT
  → 既有受控 IAT / HostImport
  → 返回状态与 Guest 结果；通过 DLL 导出查询持久业务状态
  → 成功时更新结果控件，失败时显示明确诊断并保留上次有效结果
```

继续使用已固定 `aevguest.dll` / `aevhost.dll` / registry 和导出 ABI，优先保持 DLL 字节不变。GUI可由Host harness创建，不宣称自动转换任意Win32窗口创建代码或完整Windows消息泵。

## 3. A：先协调 PE session 最小契约

coordinator协调 compiler（PE执行会话owner）、binary（模块身份/装载会签）、platform（C ABI与GUI调用会签），reviewer审查。API的具体命名/布局在此阶段决定，先满足以下行为：

- 版本化的C ABI/薄共享库，供CLI与Python GUI等调用；定义create/load、导出解析/调用、身份/selected/计数查询、Trace开关、销毁/错误返回。参数限制沿用支持的整数/指针子集，结构尺寸/ABI不匹配明确拒绝。
- 输入仍为实际DLL、registry、Guest load base、可选绑定AOT与主入口；actual module identity / AOT表 / base-new provenance沿用统一校验，不能根据GUI模式放宽校验。
- 一个GUI窗口对应一个存活session/模块实例，导出调用之间保留Guest全局数据、HostImport状态、JIT cache、Trace调用编号。Reset只由明确用户动作/场景初始化触发，不在每次点击偷偷reload/reset。
- Guest VA/Host backing/Host函数地址仍分离。GUI只经受检查接口访问Guest内存，不直接解引用Guest数值。明确32位有符号业务参数/返回与u64寄存器位模式的转换；UI显示不能将0xFFF...误作不同业务结果。
- session固定owner线程，GUI路径为AppKit主线程。未支持跨线程、重入/调用中销毁明确拒绝；Host异常不得穿过C ABI。定义关闭、重复关闭、部分初始化失败的资源释放。
- stale handle必须可诊断；若使用已释放裸指针，就不能承诺可靠的“销毁后调用拒绝”。优先复用现有代次/handle管理，避免UAF；销毁一个session不得影响其他实例。
- HostImport继续使用声明且Windows对照一致的受控实现。若增加注册/观察回调，明确参数与生命周期/异常边界，不把它扩成任意Windows API。
- 明确首次失败保留、per-call与累计计数、trace sink清零/关闭等语义；保留runner的既有JSON/退出码/`--expect-error`行为。已有ABI不因目录重构无意义改版；确需变化时统一升级全部消费者并拒绝不兼容产物。

## 4. B：共享实现先通过无GUI验证

- 从 `pe_runner.cpp` 抽取实际模块/Program/JitRuntime/AOT校验/dispatcher/HostImport/Trace/生命周期到共享组件。runner变成薄CLI消费者，GUI使用同一组件。
- 不复制新的PE parser、dispatcher、JIT缓存、Trace writer或artifact校验器来适配GUI；不另建不同语义的GUI版执行器。保持共享内存helper与权限/SAR修复。
- 不用每次按钮启动`aev-pe-runner`进程、不用解析runner JSON当执行桥，不在Host重写DLL业务，也不使用Unicorn/Rosetta兜底。测试脚本可调用CLI，产品GUI调用必须进程内共享session。
- headless测试先验证同一session多次Compute/State、Reset、至少两个session的状态隔离、部分加载失败、close/stale handle、错误线程/重入、内存故障恢复策略。明确哪些Guest副作用在故障前已发生，不承诺任意事务回滚；UI不提交失败结果与Guest内存回滚是两件事。

## 5. C：同一 DLL 接入 compat/native

- platform使用现有后端。最小GUI包含业务参数输入（如a/b/n、必要seed）、Compute、明确Reset以及结果/Guest State/调用次数显示；以已冻结DLL接口决定控件，不为追求界面增加Guest指令/API范围。
- 只有GUI输入解析、调度、格式化和展示放在Host。业务计算/全局状态修改来自实际DLL机器码；State/Calls等通过DLL导出查询，不以Host影子计数代替。
- 两后端使用同一DLL SHA、registry、Guest load配置、AOT、session库与操作序列。native真实AppKit，compat独立自绘语义，无静默切换。
- 成功后把Guest返回与查询状态提交到实际控件。非法数字/越界输入先明确诊断、不进入Guest；PE调用失败保留上次有效业务显示，错误单独可见。闭窗销毁session、释放模块/Trace；重开新实例状态按契约初始化。
- 至少一条错误经真实按钮/控件事件进入PE session验证，例如测试场景选择已支持的ReadAt非法地址或无效导出；使用实际控件文本和Guest状态检查，而不只检查Host缓存字符串。测试专用错误参数要在日志明确，不能混作正常业务输入。

## 6. 验收与证据

1. **同一持久实例**：三组以上输入/操作序列（含准备集外输入），明确重置与连续调用语义；同一session连续100次Compute按钮动作，DLL Calls/State、HostImport行为、返回值与独立oracle/Windows同序列对照一致，加载次数不随点击增加。100次只作稳定性，不宣称提速。
2. **AOT/JIT两种产物**：base coverage下，新业务分支实际JIT且返回AOT；Trace来自该PE session真实执行。按现有离线工具生成补充产物，再用新GUI进程/session复跑相同输入，promoted块实际AOT、无对应JIT，状态/结果一致。base/new/promoted/demoted、SHA、实际块表逐项绑定；不靠表存在代替执行证据。
3. **实例与错误**：两个session隔离、销毁/重开状态、stale handle、未支持重入/错误线程、部分加载失败、权限故障、错误DLL/base/registry/ABI/artifact错配明确诊断。至少一项事件驱动PE故障后UI结果不提交，随后正常操作按声明策略恢复；不要求撤销故障前Guest已完成的副作用。
4. **两个GUI后端**：相同DLL/产物/操作，结果与状态一致；截图/对象类/主线程证明；native强制失败无compat回退；无Unicorn/Host业务替代。
5. **双机**：Windows用同一固定DLL与helper执行相同有状态操作序列；可扩展参考driver读取场景参数，不改变转换输入源码依赖。固定DLL未变时无需无意义重编它，但参考结果必须覆盖本轮实际序列。需要Windows可见GUI或物理操作时提供步骤，未完成标待人工。
6. **回归**：原6个ctest、PE147项、语义791项、Trace/AOT78项、provenance与六组既有COFF GUI各44项保留。合理重构/改断言时列出等价关系，不删弱失败/权限/CF测试。新PE GUI检查有自己的场景与oracle，不把旧COFF检查改名冒充PE集成。
7. **独立QA**：在最终源码提交、新build/results目录核验。QA-behavior/Windows与QA-artifact/lifecycle可分会话；至少一个使用自己的新输入、逐项检查真实DLL状态与真实控件，不只信实施者自述。

## 7. 调度、范围与结束

- 常态两个实施owner：compiler负责共享PE session/CLI，platform负责GUI。契约先稳定；二者独立worktree/build/cache，公共ABI/CMake/dispatcher单owner顺序整合。binary按需会签或调查，主控可复用QA/reviewer角色增开任务会话；worker不递归派生。
- 所有主/新/子会话实际核对OpenCode Go / DeepSeek V4.1 Flash / Max；仅桌面端原生会话，不启动opencode run/CLI模型worker，不擅改账号/密钥。隔离/模型/资源条件不满足时串行，不换模型绕过。
- 控制磁盘副本：优先一套实施构建、一套最终独立QA构建，反例用小目录；新循环日志使用计数或有界诊断，必要原始记录可以压缩并保留原/压缩SHA与恢复说明。不能省掉验收、删历史证据或自动清理未合并工作树来节省空间。
- 排除通用EXE/Win32窗口自动转换、完整消息泵、任意嵌套/线程/模态、TLS/CRT/SEH、通用依赖图、真实第三方应用、运行中代码失效、独立app分发、公证与性能优化。边界外行为明确拒绝；仍不宣布P0整体/首版完成。
- 结束提交最终代码与最小契约、实际DLL/依赖/库/AOT/Trace/provenance哈希、持久session和加载次数证据、两后端结果/截图、Windows同序列对照、命令/退出码/日志、独立QA/reviewer最终版本结论及未测项。停在本轮集成分支供外部复核；不自行合main/push/删除或归档工作树，不自动开启下一轮。
