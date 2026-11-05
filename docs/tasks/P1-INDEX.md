# P1 任务草案（待 P0 证据确认，尚未派发）

状态：草案。P0-ADR-01 完成后由 coordinator 修订；不得把本表当成已承诺排期。2026-11-05 的下一轮限定为 [共同回调 ABI 与双 GUI 最小集成](../plans/P0-CALLBACK-INTEGRATION-01.md)，尚未派发；下文 P1 全量契约不作为该最小实验的前置循环依赖。

## 进入 P1 的门槛

- P0 至少一条执行核心路线有 AOT 独立进程 + JIT 回退证据，且 P0-QA-01 独立复跑通过。
- `compat` 与 `native` 各有真实 GUI 回调/窗口证据；P0 临时桥接只能证明 GUI 语义，不能替代“选定执行核心 + 选定兼容层”的集成验证。
- ABI/artifact/公共构建 owner 和 v0 契约草案明确，P0-REVIEW-01 签署。
- TRACE 方案有可执行记录格式或明确标为 P1 待验证，不得把调查当成已验证。
- 没有未解决的许可阻断（尤其 GPL/QEMU/Wine/Unicorn 组合）。P1 核心依赖（LLVM/Capstone）许可已记录；P0-only Unicorn 与 compat 最终运行库（Wine/自建）未批准为产品路径，必须在 P1-CONTRACT 前分开处理。

## P0 收尾任务（P1 前/并行，尚未派发）

| 任务 | owner | 依赖 | 交付 |
| --- | --- | --- | --- |
| P0-CALLBACK-ABI-01 | coordinator + compiler/platform | main 已合并的 ENGINE/UI | 仅本轮所需的回调/内存/HostImport/退出契约与 fixture 范围 |
| P0-CORE-BRIDGE-01 | compiler + platform | P0-CALLBACK-ABI-01 | 可复用 ENGINE 薄接口，无 GUI 共同调用链 |
| P0-UI-INT-01 | platform + compiler | P0-CORE-BRIDGE-01 | 同 Guest 输入/同核心驱动 compat/native；集成路径不使用 Unicorn |
| P0-TRACE-SPIKE-01 | compiler + binary | ENGINE runner、TRACE 方案 | 实际 Guest PC/基本块/间接目标记录 + 独立输入重组；关闭 D6/P0-05 |
| P0-APP-RUN-01 | qa | SCOPE 7-Zip 候选、Windows 交互桌面 | Windows 参考 7-Zip 固定工作流/截图/进程树/输出哈希；不要求已转换 |
| P0-PKG-01 | platform + qa | ENGINE/UI spike | 最小 ARM64 `.app`/随包 dylib/`@rpath`/本地签名/JIT entitlement 探针；关闭 R05 的 P0 部分 |
| P0-BUILD-REPRO-01 | compiler + qa | ENGINE/UI 构建 | 统一可复现构建口径（`.text` + 完整二进制 `/Brepro`/install name） |

## 候选任务

| 任务 | owner | 依赖 | 交付 | 备注 |
| --- | --- | --- | --- | --- |
| P1-CONTRACT-01 | coordinator（platform/compiler/binary 会签，reviewer 审查） | P0-ADR-01 | GuestState、内存/线程/退出、AOT/JIT helper、模块身份/失效、manifest、平台后端能力声明/配置/版本兼容、测试结果格式 v0 | 先冻结再并行实现；模块身份 schema 由 binary 拥有，compiler 消费 |
| P1-BUILD-01 | coordinator + qa | P1-CONTRACT-01 | CMake/Ninja 双机构建入口、Windows 参考收集、Mac 产物检查、报告脚本 | 统一构建目录/缓存规则 |
| P1-PE-01 | binary | P1-CONTRACT-01 | PE32+ 映射、导入/导出/forwarder/API set、重定位、资源、TLS/初始化、delay import、`.pdata/.xdata`、load config/CFG、`RELOCS_STRIPPED` 诊断、模块身份 | 模块身份 schema owner；覆盖真实候选所需 PE 特性 |
| P1-CORE-01 | compiler | P1-CONTRACT-01、P1-PE-01（模块身份） | x64 解码/语义子集、ARM64 生成、AOT 调度、JIT 交接、基础失效 | 消费 binary 的模块身份 |
| P1-PLATFORM-01 | platform | P1-CONTRACT-01 | kernel32/user32 基础子集、Guest/Host ABI 桥、回调、LastError、线程状态 | 先做测试族需要的 API |
| P1-ABI-01 | binary（lead）+ platform | P1-CONTRACT-01 | GuestState/桥接/异常/PE 能力表/TEB 版本化 | 与 P1-PLATFORM-01 共用 ABI 契约 |
| P1-MEM-01 | compiler + qa | P1-CORE-01、P1-ABI-01 | x86 TSO/原子/未对齐/跨页/权限失败 litmus | 全部当前未验证；先声明保证级别 |
| P1-TEST-01 | qa | P1-BUILD-01、P1-CONTRACT-01 | 固定 Win32 测试族、参考结果、独立验证输入、差分报告 | 准备集/独立集分开；作为 P1 验收 gate |
| P1-TRACE-01 | binary + compiler | P1-CONTRACT-01、P1-CORE-01 | 动态发现记录、输入身份绑定、增量合并和失效 | 不把一次 Trace 当回放 |
| P1-GUI-COMPAT-01 | platform | P1-CONTRACT-01、P1-CORE-01、P1-PLATFORM-01 | compat 窗口/控件/消息/模态最小产品路径 | 选定兼容实现，ARM64/no-Rosetta；与 native 共用核心 |
| P1-GUI-NATIVE-01 | platform | P1-CONTRACT-01、P1-CORE-01、P1-PLATFORM-01 | AppKit 原生窗口/控件/对话框/主线程调度 | 不静默回退 compat |
| P1-REVIEW-01 | reviewer | 上述产物 + P1-TEST-01 | 需求覆盖、ABI、正确性、许可、证据审查 | 只读；QA 补测；作为 P1 退出 gate |

## 并行与集成约束

- 初期最多两个写代码任务并行；分别使用独立分支/worktree/构建目录。
- ABI、artifact、公共构建和平台接口变更由 coordinator 先更新契约，再分派。
- 每个任务必须提供输入/二进制哈希、命令、退出码、日志和未测项；QA 在指定提交独立验证。
- GUI 自动部分与人工观察分开；人工未完成时保持 `待人工验证`。
