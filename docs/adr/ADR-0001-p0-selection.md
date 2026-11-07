# ADR-0001：P0 选型历史与 2026-11-07 状态更新

状态：本批实现与外部复核已统一到 main；P0 总体验收仍未关闭，本 ADR 不是 P1 全量契约。
更新：2026-11-07；保留 2026-11-05 审查与候选原文。
决策者：coordinator；审查：reviewer；证据责任人：compiler / platform / binary / qa。

## 当前决策状态（2026-11-07）

当前能力以 [支持矩阵](../p0/SUPPORT-MATRIX.md)、[当前状态](../CURRENT_STATUS.md) 和 [本批集成记录](../p0/INTEGRATION-20261107.md) 为准。下方历史原文中“UI 尚未接核心”“Trace 只有方案”“PE 重定位未验证”等已被后续有界实验推进，不能作为当前结论。

| 决策 | 现状 |
| --- | --- |
| D1 执行核心 | 自建有限 x64 frontend + LLVM ARM64 AOT/ORC JIT 已有运行证据；不等于完整指令覆盖 |
| D2 artifact/交接 | 有界 COFF/PE DLL、独立 AOT、空表 JIT、身份校验/重定位/实际 base 表和补充产物验证已通过；运行中失效、跨 base 复用和持久缓存尚缺 |
| D3 compat | 仍未选定最终运行库；当前自绘语义 harness 不是产品兼容层 |
| D4 native | AppKit 已接共享 COFF/PE 核心；Guest EXE 的 Win32 窗口/消息/回调链尚缺 |
| D5 工程与系统 | 当前实际 CMake 为 C++17/C11，平台 harness 为 Python/PyObjC；最终语言边界、最低系统、独立包/签名/JIT 发布条件尚未冻结 |
| D6 采集 | 目标端实际块级 Trace→离线补充→新进程 AOT 已验证；有限实验路线成立，完整采集/失效/性能契约尚未冻结 |
| D7 真实应用 | 7-Zip 等仅静态候选，无转换后的双模式工作流验收 |

本批没有重新开始任何下一阶段任务。未完成项转入 [待办清单](../BACKLOG.md)；P0 总体验收、人工 GUI、独立 `.app` 和真实应用/性能门槛继续保留。

## 2026-11-05 历史审查原文

以下章节记录当时的结果、判断和提案，保留原口径以便追溯；不作为 2026-11-07 的当前能力声明，也不自动派发旧任务。

### P0 结果摘要

- 已成立（独立 QA 复跑）：ENGINE `a4ef11c` 的 AOT 独立进程加载、ORC JIT 真实编译并返回 AOT、ASLR、5 用例与 Windows 参考一致、未知目标/未支持指令显式失败；UI `847dad7` 的 compat/native 各 25 checks、真实 AppKit 控件/主线程、自绘 compat、无跨后端回退、截图、Windows 参考 15/15。
- 已关闭：UI W-1（构建脚本 `cmd.exe /c` 下 `Get-FileHash` 失败）与 W-2（DLL 固定哈希不可复现）；`/Brepro` 后新固定 DLL `c5b89e83...`，连续构建一致。
- 未通过/未完成：P0-UI 仍是 Unicorn 临时桥接 + 自建语义 compat，未与 ENGINE 核心集成；D3 最终 compat 运行库未选定；P0-TRACE 只有方案，无实际记录，D6 未决；V-1 代码版本/失效未实现；P-1/R05 `.app`/随包依赖/签名/无开发工具运行未满足；真实 7-Zip 未在 Windows 运行；同核心 JIT-only/持久化缓存 JIT/AOT Hybrid 性能未测；GUI 可见窗口/物理输入/输入法/焦点/外观待人工。
- 因此本 ADR 只能作为 **P1 规划输入**，不能宣布 P0 阶段完成或首版能力。

### 独立审查结论（P0-REVIEW-01）

- ENGINE 证据可排除 Host 重写/固定回放/只跑 JIT；UI 证据可排除 Host 改状态/只跑一侧/静默回退；W-1/W-2 关闭可信。
- P0 整体不通过维持：P0-03（选定核心+兼容实现集成）与 P0-05（实际 Trace）未闭合；R05/P-1、V-1、真实应用、性能三类对照、人工 GUI 未完成。
- 审查指出的支持矩阵缺失、UI 状态口径、WinMerge 备选冲突、TLS 字段混用、QA 旧报告、ENGINE “unsigned”措辞等已在 coordinator 文档中处理；支持矩阵见 `docs/p0/SUPPORT-MATRIX.md`。
- 非阻断但需 P1 处理：UI `DefWindowProcW/TranslateMessage/DispatchMessageW` 占位返回、ENGINE `aev_load` 失败返回 0、UI 证据 `git_head` 旧值、ENGINE README/REPORT 的签名措辞。

## 背景

首版要求 Windows x86-64 用户态 EXE/DLL/资源 → macOS ARM64 独立 GUI 应用；AOT 优先、允许 JIT 回退；同时提供 `compat` 与 `native` 两种输出。AOT 产物必须实际执行转换后的 Guest 机器代码，不能用 Host 重写程序或包装现有运行器冒充。`native` 必须真实使用 macOS 原生 GUI/系统接口，不能静默回退 `compat`。

## 平台约束（本机已实测/官方资料）

- macOS 27.0.1、arm64、Xcode 27.1、Apple clang 21、CMake 4.4.4、Ninja 1.13.2。
- `mmap(PROT_READ|PROT_WRITE|PROT_EXEC, MAP_JIT)` + `pthread_jit_write_protect_np` 在 linker-signed/ad-hoc 可执行文件上可写入并调用 ARM64 `ret`；Hardened Runtime 需要 `com.apple.security.cs.allow-jit`，无 entitlement 时 `mmap MAP_JIT` 返回 errno 22。
- arm64 Mach-O dylib 移除签名后 `dlopen` 失败；链接器 ad-hoc 签名或 `codesign --sign -` 后可加载。AOT artifact 必须包含签名步骤。
- Apple 官方支持页：Rosetta 在 macOS 27 及更早可用，从 macOS 28 起仅对特定旧游戏保留有限功能。产品 Host 组件不能依赖 Rosetta。

### 来源与核查边界

| 事实 | 来源 | 状态 |
| --- | --- | --- |
| Rosetta 生命周期 | https://support.apple.com/en-us/102527（2026-09-21 发布） | 官方页面已读；未在 macOS 28 实机验证 |
| JIT/MAP_JIT 与 entitlement | Apple 官方 JIT 文档（REFERENCES R3）+ 本机探针 | 本机探针实测；分发签名未验证 |
| AOT dylib 签名 | 本机 `codesign`/`dlopen` 探针 | 本机实测；公证/分发未验证 |
| FEX/Box64 | https://github.com/FEX-Emu/FEX、https://github.com/ptitSeb/box64 | README 已读；未构建 |
| QEMU TCG | https://www.qemu.org/docs/master/devel/tcg.html | 文档已读；未构建 |
| Remill | https://github.com/lifting-bits/remill | README/许可已读；未构建 |
| LLVM 23.1.4 | 本机 `/opt/homebrew/opt/llvm` | 本机版本已核对；未做许可文件全文审查 |
| Capstone 5.0.9 | 本机 `/opt/homebrew/opt/capstone` | 本机版本已核对；P0-ABI 已记录 BSD-3-Clause |

## 决策驱动

1. 正确性：x64 语义、内存序、异常、回调、DLL/PE 装载和失效。
2. AOT 能力：可导出、可重定位、可在新进程加载、可版本失效。
3. JIT 交接：未覆盖代码真实编译并返回，不掩盖缺失。
4. 目标平台：macOS ARM64、Hardened Runtime/MAP_JIT、签名、独立 `.app`。
5. 嵌入与许可：能否作为库嵌入、许可证与组合发布义务。
6. 工程成本：首版可落地的最小闭环和可测试性。
7. GUI 双后端：Win32 语义兼容与 AppKit 原生控件各自真实生效。

## 候选（待证据填充）

### 执行/编译核心

| 候选 | 已知事实 | 待验证 | 初步风险 |
| --- | --- | --- | --- |
| 自建 x64 frontend + LLVM AArch64 | LLVM 23.1.4 可用，Apache-2.0 with LLVM exceptions；P0-ENGINE-01 spike 已验证 AOT/JIT 闭环 | 完整 x64 语义覆盖、PE 重定位、异常/失效 | 自建 lifter 工作量大；异常/自修改代码后置 |
| Remill（指令语义库）+ 自建发现/运行时 + LLVM | Apache-2.0；README 声明支持 x86-64/AVX/AVX512 语义、macOS、LLVM 15+；未在本项目构建 | 固定版本构建、Windows PE 集成、生成的 IR 与 AOT/JIT 交接、体积/性能 | 依赖重、LLVM 版本耦合；不能直接提供 PE 装载/运行时 |
| QEMU TCG | JIT；GPLv2；未见 AOT 导出接口 | 是否能导出可重定位代码、嵌入成本 | 许可与 AOT 目标不符 |
| FEX-Emu | MIT；面向 ARM64 Linux；需 x86-64 RootFS；code cache 是运行期缓存 | macOS 可用性、AOT 导出 | 非 macOS、非 PE |
| Box64 | MIT；Linux userspace；DynaCache 运行期缓存 | AOT 导出、macOS | 非 macOS、非 PE |
| Rosetta 2 | macOS 27 及更早可用；macOS 28 起仅特定旧游戏 | 是否可作为开发期参考 | 产品路径不能依赖；不可嵌入 |

已排除/不作为首版直接依赖的线索：BinRec（README 面向 Ubuntu/Debian + S2E + LLVM 14，非 macOS/PE 直接可用）；McSema（AGPL-3.0、已归档、控制流恢复依赖 IDA Pro）。它们只保留方法参考，不能直接复制代码或当作已选依赖。

### 平台兼容 / GUI

| 候选 | 已知事实 | 待验证 | 初步风险 |
| --- | --- | --- | --- |
| Wine x86-64 + Rosetta | macOS 常见路径；Rosetta 生命周期受限 | 无 Rosetta 的 ARM64 Host 方案 | 违反“不依赖 Rosetta” |
| Wine ARM64 + 翻译核心 | Wine 可在 ARM64 跑 ARM64 PE；x86-64 PE 需翻译 | 与自建核心的 PE/ABI 集成 | 集成复杂、许可/组合义务 |
| 自建 Win32 兼容层 | P0-UI-01 验证最小子集 | API 覆盖、消息/重入/模态 | 工作量大，但边界可控 |
| AppKit native 适配 | P0-UI-01 验证 NSWindow/NSButton/NSTextField | 控件映射、主线程、回调 | 不能覆盖自绘/复杂控件 |

### 语言与构建

- 建议：C++20 + C ABI 核心，Objective-C++ 平台适配，Python 编排；CMake + Ninja；macOS 最低版本待定。
- 工具链已在 P0-ENGINE-01/P0-UI-01 验证；D5 仍待用户确认最低 macOS 版本后冻结。

### 准备执行与记录

- 建议：Windows 原生执行负责业务参考输出/文件/事件；目标端自建执行核心负责 Guest PC/基本块/间接目标/代码版本记录。SSH 不是 Trace 通道。
- 待 P0-TRACE-01 验证记录格式、开销和失效规则。

## ABI 结论（P0-ABI-01，调查）

- Windows x64 调用约定已交叉编译/反汇编验证：RCX/RDX/R8/R9、XMM0–3、32 字节 shadow、第 5/6 参数 `[rsp+0x28/0x30]`、调用点 16 字节对齐；非叶函数需要 `.pdata/.xdata`。
- GuestState 不能只用 GPR+少量 XMM：至少 GPR/RIP/RFLAGS/FS_BASE/GS_BASE/XMM0–15/MXCSR/x87/TEB/LastError/线程栈；AVX/AVX-512 未支持时显式诊断。
- TEB/PEB 是版本化、非稳定 ABI：`GS:0x58/0x60` 已由编译器实测，`GS:0x68` 未实测；禁止把完整 TEB 偏移写成无版本常量。
- `SendMessage` 同线程直接重入、跨线程阻塞并泵消息；`native` 不能异步化后仍宣称 Windows 兼容。模态/重入/线程退出必须按 Guest 语义。
- Guest SEH/C++ EH 不能穿过 Host 帧；7-Zip 有 212 个函数使用 `msvcrt!__CxxFrameHandler`，首版是否支持 C++ EH 必须显式决定，不能静默。
- PE 装载需覆盖 forwarder/API set/delay import/TLS 回调/`.pdata`/load config/CFG/资源；`RELOCS_STRIPPED` + 基址冲突必须诊断。
- x86-64 TSO 与 ARM64 弱序差异、`LOCK`/原子/未对齐/跨页/权限失败/映射代次全部未实测；P1 必须先声明保证级别，不能默认“同地址映射等价”。

## 决策（reviewer 已审；P0 整体不通过，D3/D6 未决）

- D1 执行核心：优先自建 x64 前端 + LLVM AArch64 后端；ENGINE spike 已验证最小 AOT/JIT 闭环。Remill（Apache-2.0）作为 P1 指令语义覆盖候选，需先做固定版本构建/许可/集成验证；QEMU/FEX/Box64/Rosetta 不作为首版直接执行核心。
- D2 AOT/JIT 交接与 artifact：AOT 模块优先独立签名 dylib + 模块描述符/调度表；JIT 回退用 LLVM ORC 或等价真实编译；spike 已证明 dlopen/ASLR/JIT return；PE 重定位/TLS/代码失效仍待 P1。
- D3 `compat` 平台路线：**未决**。自建 Win32 兼容层是候选；Wine x86-64 + Rosetta 违反无 Rosetta 要求；Wine ARM64 复用需先证明 PE/ABI/许可可行；P0 UI 的 compat 只是语义 harness，不是最终运行库。
- D4 `native` GUI 路线：AppKit 原生窗口/控件适配；P0 UI 已验证 NSWindow/NSTextField/NSButton/NSMenu/NSAlert、主线程和回调；产品化仍缺 Guest 消息泵/引擎集成。
- D5 构建/语言/最低系统：C++20 + C ABI 核心、Objective-C++ 平台适配、Python 编排、CMake/Ninja；最低 macOS 版本待定；UI `/Brepro` 与构建脚本已修复。
- D6 采集与准备执行：Windows 原生执行负责业务参考输出；目标端自建执行核心负责 Guest PC/基本块/间接目标/代码版本记录；SSH 不是 Trace 通道。P0-TRACE-01 只有调查/方案，**D6 未决**，P0-05 不能关闭。
- D7 首版真实应用与支持矩阵：主选 7-Zip 26.05（LGPL-2.1+，含 unRAR/BSD 组件；`7zFM.exe` + `7z.dll`）。备选不按 WinMerge 排序：PE/API 证据显示 WinMerge 2.16.60 的 762 个导入、静态 MFC/ATL、GDI+、COM/OLEACC、CFG/TLS 2 回调与可选 WebView2 组件明显更难；PuTTY 0.85（MIT，但网络/加密/终端自绘）或 Notepad++ 8.9.8.1（GPL-3.0，Scintilla/D2D/插件）仅在其范围被纳入时作条件式备选。首轮 7-Zip 工作流限定为“打开归档→列出→解压→校验→退出”，Shell/拖放/UNC/密码/多卷/`7zG.exe` IPC 单列；最终由用户确认，P0 未运行该应用。输入/指令/PE/GUI/真实应用 consolidated 矩阵见 `docs/p0/SUPPORT-MATRIX.md`。

## P1 契约候选（待证据确认，不冻结）

- Guest 状态：GPR（含 RSP/RBP/RIP）、RFLAGS、MXCSR、x87 控制/状态、所需 XMM/YMM、TEB/PEB、LastError、线程 ID、栈边界、退出原因。
- 内存：Guest 地址空间、权限、映射代次、Host 映射生命周期、跨页/原子访问失败分类。
- 模块身份：输入内容哈希、PE 时间戳/大小、节/导入/导出/重定位/TLS/资源摘要、Guest 地址与字节依赖。
- AOT/JIT：块边界、helper ABI、Host↔Guest 调用/回调、JIT 回退原因、代码版本、失效事件、未支持指令诊断。
- Artifact manifest：格式版本、输入/模块身份、转换器/运行时/后端/helper ABI 版本、目标特性、重定位假设、配置、构建 ID。
- 平台后端：`compat`/`native` 能力表、API 支持状态、未支持时的错误、禁止静默回退。
- 测试结果：场景 ID、输入哈希、环境、命令、退出码、stdout/stderr/文件副作用、事件序列、AOT/JIT 统计、后端 ID。

## Artifact 方案候选（P1 冻结前）

- 构建目录按应用/模式隔离；`.app` 内含 ARM64 主程序、执行运行时、模式后端和签名的 AOT 模块。
- AOT 模块优先采用独立签名 dylib + 导出模块描述符/调度表；`dlopen` 前校验签名/架构/ABI/输入哈希。若 P0 只能静态链接，必须在报告中标“动态加载/重定位未验证”。
- Guest 原始 EXE/DLL 只作为输入/资源/调试材料，不能被当作执行入口；主执行链必须进入 AOT/JIT 生成的 ARM64 代码。
- 运行时新增 JIT 缓存/Trace 写入 `~/Library/Application Support/Aevotrace/` 或 Caches，不修改已签名包。
- manifest 记录输入/模块哈希、代码版本、转换器/运行时/helper/后端 ABI、目标特性、模式、构建 ID、重定位假设。
- 所有可执行依赖检查 `lipo -archs`/`file`，主执行链不得要求 Rosetta。
- 本地开发签名可 ad-hoc；分发签名/公证/JIT entitlement 作为独立交付检查，不在 P0 宣称完成。

## P1 第一批建议（不自动等于已派发）

1. **P1-CONTRACT-01**（coordinator 主导，binary/compiler/platform 会签，reviewer 审）：冻结 GuestState/内存/线程/退出、AOT/JIT helper、模块身份/失效、artifact manifest、平台后端能力表、测试结果格式 v0。
2. **P0 收尾任务**（可与 P1 契约并行但必须先于 GUI 产品化）：
   - `P0-UI-INT-01`：用 ENGINE AOT/JIT 核心替换 UI 的 Unicorn 桥，至少跑通一条 Host→Guest→Host + compat/native 路径；否则 P0-03 仍不完整。
   - `P0-TRACE-SPIKE-01`：在 ENGINE runner 上实现 Guest PC/基本块/间接目标记录，独立输入重组验证；关闭 D6/P0-05。
   - `P0-APP-RUN-01`：在 Windows 参考机运行 7-Zip 26.05 固定工作流，记录窗口/进程树/输出哈希；不要求已转换。
   - `P0-PKG-01`：最小 ARM64 `.app`/随包 dylib/`@rpath`/本地签名/JIT entitlement 探针；关闭 R05 的 P0 部分。
3. **P1 实施**：P1-PE-01（binary）、P1-CORE-01（compiler，含 V-1 版本/失效）、P1-PLATFORM-01（platform）、P1-TEST-01（qa）、P1-ABI-01/P1-MEM-01（binary+compiler+qa）、P1-TRACE-01（binary+compiler）。
4. **P1 GUI**：P1-GUI-COMPAT-01/P1-GUI-NATIVE-01 仅在 CONTRACT + CORE + PLATFORM 稳定后并行，仍限两个写代码任务。

## 后果

- 正面：
- 负面/成本：
- 需要用户确认：

## 未决问题

- 目标 macOS 最低版本与签名/公证范围。
- JIT entitlement 与独立 `.app` 的发布方式。
- 首版 x64 指令/SSE/AVX 子集；真实候选含 AVX/AVX2/FMA/SHA 路径，CPUID 必须与执行能力一致。
- 兼容层是自建还是基于 Wine 代码的可许可复用；`compat` 最终实现未选定。
- 真实应用的最终选择与工作流；7-Zip 启动时 COM/Shell/拖放是否无条件调用未验证。
- TEB/PEB 版本化布局与 `WindowsBuildId` 支持范围；`GS:0x68` LastError 未实测。
- 首版 C++ EH/SEH 级别；7-Zip 的 `__CxxFrameHandler` 是否首版必须。
- R07 性能 gate：同核心 JIT-only / 持久化缓存 JIT / AOT Hybrid 三类对照在 P1/P6 的冻结阈值，P0 只有原始混合路径数据。
- x86-64 TSO 保证级别：保守全屏障、分级同步还是首版只保证单线程/显式同步。
- `native` 对自绘 list/tree/进度/终端的容忍边界；不能宣称全部原生。
- 动态代码、异常/SEH、TLS、多线程的 P1 优先级。

## 明确未验证的风险（不得在 P0 报告中写成已解决）

- 独立 `.app` 的 ARM64 入口、包内动态依赖、无开发工具路径、可写缓存目录、签名/公证未在 P0 验证。
- `native` 与最终执行核心的集成未验证；P0-UI-01 的临时桥接只证明 GUI 语义。
- `compat` 最终兼容实现未选定；P0-UI-01 的语义 harness 不是产品运行库。
- x86-64 内存序、原子、未对齐/跨页访问、异常/SEH、TLS、多线程未在 P0 完整验证。
- AOT 重定位/ASLR/动态加载与代码版本失效仅在 spike 范围内验证，未达到产品 artifact 契约。
- 许可组合（尤其 GPL/QEMU/Wine/Unicorn）未做法律结论；只记录工程风险。
