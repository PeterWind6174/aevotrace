# ADR-0001：P0 执行引擎、平台路线与双 GUI 后端选型（草案）

状态：草案，等待 P0 实验证据；不得据此宣称已实现。
日期：2026-11-05。
决策者：coordinator；审查：reviewer；证据责任人：compiler / platform / binary / qa。

## 背景

首版要求 Windows x86-64 用户态 EXE/DLL/资源 → macOS ARM64 独立 GUI 应用；AOT 优先、允许 JIT 回退；同时提供 `compat` 与 `native` 两种输出。AOT 产物必须实际执行转换后的 Guest 机器代码，不能用 Host 重写程序或包装现有运行器冒充。`native` 必须真实使用 macOS 原生 GUI/系统接口，不能静默回退 `compat`。

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
| QEMU TCG | JIT；GPLv2；未见 AOT 导出接口 | 是否能导出可重定位代码、嵌入成本 | 许可与 AOT 目标不符 |
| FEX-Emu | MIT；面向 ARM64 Linux；需 x86-64 RootFS；code cache 是运行期缓存 | macOS 可用性、AOT 导出 | 非 macOS、非 PE |
| Box64 | MIT；Linux userspace；DynaCache 运行期缓存 | AOT 导出、macOS | 非 macOS、非 PE |
| Rosetta 2 | macOS 27 及更早可用；macOS 28 起仅特定旧游戏 | 是否可作为开发期参考 | 产品路径不能依赖；不可嵌入 |

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

## 决策（待填）

- D1 执行核心：
- D2 AOT/JIT 交接与 artifact：
- D3 `compat` 平台路线：
- D4 `native` GUI 路线：
- D5 构建/语言/最低系统：
- D6 采集与准备执行：
- D7 首版真实应用与支持矩阵：

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
