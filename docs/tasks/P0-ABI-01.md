# P0-ABI-01：Windows x64 ABI、TLS、异常、回调与依赖许可调查

状态：完成（调查与静态/交叉编译证据；未在 Windows/macOS 运行 ABI 探针，运行时项标未验证）。

## 输入与责任

- 阶段、需求 ID、前置任务：P0；R01/R02/R06/R08；可与 SCOPE 并行。
- owner / reviewer / QA：binary（lead）+ platform（contributor） / reviewer / qa（按需）。
- 基线 commit 或源码包哈希：`373becbdd7a65cafd63a98cb526bb1a9c933f731`。
- 分支 / worktree / Windows 工作目录：调查任务，不改产品源码；报告在 `.local/results/P0-ABI-01/`。
- 公共 ABI / artifact / 平台接口版本：草案，供 P0-ADR-01 冻结，不直接实现。

## 目标与范围

- 可观察的完成结果：一份可执行的 ABI/依赖调查，覆盖：
  1. Windows x64 调用约定：整数/浮点/向量参数、影子空间、对齐、变参、返回值、非易失寄存器、栈展开。
  2. Guest 状态：GPR、RIP、RFLAGS、MXCSR/x87/SSE 所需子集、TEB/PEB、TLS、错误状态（LastError）、线程与栈。
  3. 回调与重入：Host→Guest、Guest→Host、窗口过程、同步消息、异常/长跳转边界。
  4. 内存模型：x86-64 TSO 与 ARM64 顺序差异、原子/锁前缀、未对齐访问、跨页访问、权限失败、映射代次；至少给出 P0/P1 的验证项，未实测必须标“未验证”。
  5. PE 装载相关：重定位、导入/延迟导入、导出、资源、TLS 回调、DLL 初始化、异常目录。
  6. 候选依赖：CPU 引擎/解码器/JIT/AOT、兼容运行库、GUI 适配；记录固定版本、许可证、目标架构、可嵌入性和组合发布风险。
- 允许修改的文件或组件：仅 `.local/results/P0-ABI-01/`；不写产品代码。
- 明确不包含的内容：不实现 ABI 桥接、不冻结最终结构体布局、不把 Wine/LLVM 等候选说成已集成。
- 需要的依赖及许可记录：只做资料核查；引用官方文档/源码时记录 URL、commit/tag 和许可文件位置。

## 验收

- 测试程序、输入、准备集和独立验证集：不要求运行；至少给出两个可被 P0-ENGINE-01/P0-UI-01 直接使用的 ABI 检查项（例如参数传递/回调/栈对齐）。
- Windows 参考执行步骤：如做小实验，记录编译命令、退出码、反汇编/寄存器证据；未做则明确写“未运行”。
- macOS 目标执行步骤：不适用或仅列出后续验证方法。
- 预期业务输出、状态或副作用：P0-ADR 能据此选择桥接方式和候选依赖，不需要各 agent 自行猜测。
- GUI 自动/人工部分：不适用。
- 必须证明真实生效的路径：无；区分官方文档、作者报告和本项目实测。

## 完成报告

- 提交或 diff：无产品源码变更；报告文件列表。
- 命令、退出码、环境及配置：
- 二进制哈希、日志和报告位置：
- 正确性/性能结果：不适用。
- 审查问题及修复：
- 未测项、已知限制、需要用户操作的具体步骤：
- 后续任务与接口变更：建议冻结的 v0 契约条目和 P1 验证任务。

## 完成报告（2026-11-05，调查阶段）

- 提交或 diff：无产品源码变更；报告 `.local/results/P0-ABI-01/binary-abi.md`（53,507 B），原始探针在 `.local/results/P0-ABI-01/spike/`。
- 实际证据：`clang --target=x86_64-pc-windows-msvc` 交叉编译 ABI 探针（exit 0）；`llvm-objdump` 反汇编证明 RCX/RDX/R8/R9、32 字节 shadow、第 5/6 参数 `[rsp+0x28/0x30]`、调用点 `rsp%16==0`、`GS:0x58` TLS 路径；`llvm-readobj --unwind` 证明非叶函数 `.pdata/.xdata`；`ssh win` 只读 `dumpbin` 确认 `DefWindowProcW` forwarder。
- 关键结论：GuestState 需 GPR/RIP/RFLAGS/FS_BASE/GS_BASE/XMM0–15/MXCSR/x87/TEB/LastError/线程栈；TEB/PEB 必须版本化，不能把完整偏移当稳定 ABI（`GS:0x58/0x60` 实测，`GS:0x68` 未实测）；`SendMessage` 同线程直接重入、跨线程阻塞并泵消息，`native` 不能异步化；Guest SEH/C++ EH 不能穿过 Host 帧；PE 需处理 forwarder/API set/delay import/TLS/`.pdata`/load config/资源；x86 TSO 与 ARM64 弱序未验证，P1 必须先声明策略。
- 候选依赖许可：LLVM Apache-2.0 WITH LLVM-exception；Capstone BSD-3-Clause；Remill Apache-2.0（未构建）；QEMU GPLv2；FEX/Box64 MIT；Wine LGPL-2.1+；Unicorn GPLv2（仅 P0）。未集成、未做法律结论。
- 未验证：未在 Windows 运行 ABI 探针；未在 macOS 执行 Guest/桥接；内存序/原子/未对齐/跨页/权限/映射代次未运行；TLS 回调与 DllMain 顺序、C++ EH/CFG 未运行；Authenticode 信任未验证；`lld-link` 缺失，未链接完整 PE 探针。
- 需要裁决：TEB 版本化模型、同步消息语义、`native` 自绘内容边界、7-Zip 的 `__CxxFrameHandler` 首版级别、TSO 保证级别；`tls_callbacks` 统一为“目录存在/非空回调数量”。
- 后续：P1 冻结 GuestState/桥接/异常/PE 能力表/映射代次/诊断码；P1-ABI-01 与 P1-MEM-01 在 Windows+Mac 双机实测。
