# 给 aevotrace-coordinator：PE 段权限与 SAR 标志位聚焦修复

执行 P0-PE-CORE-FIX-01。上一轮 PE/DLL 正常主链和既有回归已通过外部复跑，但发现 F1/F2 两项正确性问题。本轮只修这两项、补充有意义的回归、独立 QA/reviewer 和报告，然后停止供外部复核。

## 基线与先读

- 从 `p0/pe-dll-01` / `p0/pe-dll-integration` 的 `00ee8d3a20fc68507adcfe5d7fc1e1007a1b2fe2` 新建本轮独立分支/worktree；最终源码 `f864d97aa1b3bd58c678744f17f1ffb4e254fedb`，后续仅文档变化。先核对 refs/diff/未提交状态。main 仍旧 `c91a3f7`，不能从 main 丢失最近成果。
- 外部审查：`/Users/peterwind/Script/Systems/Aevotrace/.local/results/CODEX-PE-DLL-REVIEW-20261106/REPORT.md`。完整原始反例、输入二进制、Windows 原生参考、工具输出均在同目录；先复现实际错误。
- 按 AGENTS.md 阅读四份必读文档、最新调度约定、PE-DLL v3 契约及 Guest/helper/artifact ABI。使用 TASK_TEMPLATE 建任务，明确两项owner/文件范围/基线/验收，并将前轮最新状态更新为“外部审查F1/F2待修复”；保留原QA/reviewer历史版本结论。
- 本提示词可能仅在根目录未提交，按范围带入任务分支；不要把根目录陈旧的未提交文件覆盖已接受分支中的新版本。保留所有前轮工作树、输入和证据。

## F1：无 R/W/X 段不能隐式变为可读

位置 `spikes/p0-engine/src/pe.cpp:189-191`。当前 readable 包含 `!(characteristics & MEM_WRITE)`，R/W/X全空的section也被授予READ。

外部复现：固定DLL的`.data` characteristics从`0xC0000040`仅清除R/W/X为`0x40`；Windows系统loader装载后VirtualQuery为PAGE_NOACCESS；当前PE runner在JIT-only及绑定该修改DLL的AOT下执行`AevDllReadAt(0x180003000)`都成功返回4660。

修复要求：

- binary协调最小权限策略，compiler核对区域访问；R/W/X全空时应不可访问，或在本轮不支持时装载前明确拒绝，不能隐式提升READ。
- 检查现有readable/writable/executable推导与region表，按支持的Windows/PE语义处理；对于无法实现的组合明确拒绝。不得按section名字、固定哈希或反例路径硬编码。
- 回归至少包含零权限、只读、读写、可执行代码与现有W+X拒绝；JIT与AOT都验证禁止的读/写/取指不成功、首个故障位置正确、故障后无额外副作用。若零权限选择load-time拒绝，明确记录并保留其余runtime权限负例。
- Windows参考用同一修改DLL核对系统页属性/装载结果；不需要在原生进程故意非法解引用来制造崩溃。原始DLL保持不变。

## F2：SAR 小位宽大计数的 CF

位置 `spikes/p0-engine/src/aev_mem.c:268-270`。`SAR`在masked count大于operand bits时固定CF=0，负数情形与原生Windows不同。

真实反例：

- `EAX=0x80; ECX=9; sar al,cl`：AOT/JIT AL=0xFF、CF=0；Windows同指令CF=1。
- `EAX=0x8000; ECX=17; sar ax,cl`：AOT/JIT AX=0xFFFF、CF=0；WindowsCF=1。

修复要求：

- compiler修正共享helper的SAR CF语义，保留已有5/6-bit count mask、count=0 flags保持、32/64位移位回归；不把SHL/SHR未定义位与SAR有定义的情况混为一谈。
- 覆盖8/16位正负输入，masked count为0、1、位宽-1、位宽、位宽+1、31以及原始count掩码为0的场景；对32/64位保留本轮已有边界。只对架构有定义的结果/标志位断言，不硬比AF/多位移位OF等未定义位。
- AOT与JIT都用真实COFF机器码通过共享执行链验证；至少一条真实条件分支消费CF的用例证明修复影响执行结果，而非只调用helper自测。
- 对照Windows x64原生指令；可复用外部`windows-sar-reference.py`的独立过程与源输入，记录实际字节、命令、退出码和输出。不要在Mac重编业务C代码冒充转换。

## 验证、分工与停止

- 允许修改上述实现与必要的共有契约、测试、构建调用和报告。旧6个ctest必须通过：包括PE 110项、语义501项、Trace/AOT78项及provenance；新增回归不可删弱既有断言。
- 六组既有GUI（base/首次补充/空新增再补充 × compat/native）各44项保持；Guest/bridge ABI未变则不无意义改版；确需变更先统一契约再改全部消费者。
- 一个binary实施owner负责F1，一个compiler实施owner负责F2；不同worktree/build/cache、文件ownership不交叉，可并行，公共测试/构建由coordinator顺序集成。QA可分权限/PE与指令语义两项，reviewer只读最终指定版本。主控按最新弹性调度授权增开必要会话，worker不递归派生。
- 所有实际会话核对OpenCode Go / DeepSeek V4.1 Flash / Max；仅桌面原生会话，不启动opencode run/CLI模型worker，不改全局账号/密钥。资源或隔离不足时串行，不换模型或CLI绕过。
- QA在最终源码提交、新构建/结果目录验证，不能只接受实施者自述。保留红/绿证据、输入与artifact哈希、Windows对照、AOT/JIT真实执行、权限/故障和回归输出。
- 本轮不扩PE驱动GUI、Win32 API、真实应用、打包、性能或运行中失效，不借修复无限扩大新需求。完成提交最终代码/契约版本、命令/退出码/哈希与日志、独立QA/reviewer结论、未测项；停止在本轮集成分支供外部复核，不自动合入main、push、清理工作树或开始下一轮。
