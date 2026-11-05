# ADR-0001：P0 执行引擎、平台路线与双 GUI 后端选型（草案）

状态：草案，等待 P0 实验证据；不得据此宣称已实现。
日期：2026-11-05。
决策者：coordinator；审查：reviewer；证据责任人：compiler / platform / binary / qa。

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
| Capstone 5.0.9 | 本机 `/opt/homebrew/opt/capstone` | 本机版本已核对；许可证待 P0-ABI 清单确认 |

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
| 自建 x64 frontend + LLVM AArch64 | LLVM 23.1.4 可用，Apache-2.0 with LLVM exceptions；P0-ENGINE-01 正在验证 | x64 语义覆盖、AOT 重定位、ORC JIT 交接 | 自建 lifter 工作量大；异常/自修改代码后置 |
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
- 待 P0-ENGINE-01/P0-UI-01 证明工具链可用后冻结。

### 准备执行与记录

- 建议：Windows 原生执行负责业务参考输出/文件/事件；目标端自建执行核心负责 Guest PC/基本块/间接目标/代码版本记录。SSH 不是 Trace 通道。
- 待 P0-TRACE-01 验证记录格式、开销和失效规则。

## 决策（proposed，待 P0-QA-01/P0-REVIEW-01 证据确认）

- D1 执行核心：优先自建 x64 前端 + LLVM AArch64 后端；P0-ENGINE-01 先证明最小 AOT/JIT 闭环。Remill（Apache-2.0）作为 P1 指令语义覆盖候选，需先做固定版本构建/许可/集成验证；QEMU/FEX/Box64/Rosetta 不作为首版直接执行核心。
- D2 AOT/JIT 交接与 artifact：AOT 模块优先独立签名 dylib + 模块描述符/调度表；JIT 回退用 LLVM ORC 或等价真实编译；保留静态链接作为 spike 降级，但必须标注重定位/动态加载未验证。
- D3 `compat` 平台路线：自建 Win32 兼容层为候选；Wine x86-64 + Rosetta 违反无 Rosetta 要求；Wine ARM64 复用需先证明 PE/ABI/许可可行，P0 不选定。
- D4 `native` GUI 路线：AppKit 原生窗口/控件适配；P0-UI-01 只证明最小回调/控件路线，不等于产品完成。
- D5 构建/语言/最低系统：C++20 + C ABI 核心、Objective-C++ 平台适配、Python 编排、CMake/Ninja；最低 macOS 版本待定。
- D6 采集与准备执行：Windows 原生执行负责业务参考输出；目标端自建执行核心负责 Guest PC/基本块/间接目标/代码版本记录；SSH 不是 Trace 通道。P0-TRACE-01 只有调查/方案时必须标未决。
- D7 首版真实应用与支持矩阵：候选 7-Zip 26.05（LGPL + unRAR 限制）优先，Notepad++ 8.9.8.1（GPL-2.0）备选；最终由用户确认，P0 不宣称已运行。

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

## 后果

- 正面：
- 负面/成本：
- 需要用户确认：

## 未决问题

- 目标 macOS 最低版本与签名/公证范围。
- JIT entitlement 与独立 `.app` 的发布方式。
- 首版 x64 指令/SSE/AVX 子集。
- 兼容层是自建还是基于 Wine 代码的可许可复用。
- 真实应用的最终选择与工作流。
- 动态代码、异常/SEH、TLS、多线程的 P1 优先级。

## 明确未验证的风险（不得在 P0 报告中写成已解决）

- 独立 `.app` 的 ARM64 入口、包内动态依赖、无开发工具路径、可写缓存目录、签名/公证未在 P0 验证。
- `native` 与最终执行核心的集成未验证；P0-UI-01 的临时桥接只证明 GUI 语义。
- `compat` 最终兼容实现未选定；P0-UI-01 的语义 harness 不是产品运行库。
- x86-64 内存序、原子、未对齐/跨页访问、异常/SEH、TLS、多线程未在 P0 完整验证。
- AOT 重定位/ASLR/动态加载与代码版本失效仅在 spike 范围内验证，未达到产品 artifact 契约。
- 许可组合（尤其 GPL/QEMU/Wine/Unicorn）未做法律结论；只记录工程风险。
