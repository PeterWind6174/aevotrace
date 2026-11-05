# P1 任务草案（待 P0 证据确认，尚未派发）

状态：草案。P0-ADR-01 完成后由 coordinator 修订；不得把本表当成已承诺排期。

## 进入 P1 的门槛

- P0 至少一条执行核心路线有 AOT 独立进程 + JIT 回退证据，且 P0-QA-01 独立复跑通过。
- `compat` 与 `native` 各有真实 GUI 回调/窗口证据；P0 临时桥接只能证明 GUI 语义，不能替代“选定执行核心 + 选定兼容层”的集成验证。
- ABI/artifact/公共构建 owner 和 v0 契约草案明确，P0-REVIEW-01 签署。
- TRACE 方案有可执行记录格式或明确标为 P1 待验证，不得把调查当成已验证。
- 没有未解决的许可阻断（尤其 GPL/QEMU/Wine/Unicorn 组合）。

## 候选任务

| 任务 | owner | 依赖 | 交付 | 备注 |
| --- | --- | --- | --- | --- |
| P1-CONTRACT-01 | coordinator（platform/compiler/binary 会签，reviewer 审查） | P0-ADR-01 | GuestState、内存/线程/退出、AOT/JIT helper、模块身份/失效、manifest、平台后端能力声明/配置/版本兼容、测试结果格式 v0 | 先冻结再并行实现；模块身份 schema 由 binary 拥有，compiler 消费 |
| P1-BUILD-01 | coordinator + qa | P1-CONTRACT-01 | CMake/Ninja 双机构建入口、Windows 参考收集、Mac 产物检查、报告脚本 | 统一构建目录/缓存规则 |
| P1-PE-01 | binary | P1-CONTRACT-01 | PE32+ 映射、导入/导出、重定位、资源、TLS/初始化元数据、模块身份 | 模块身份 schema owner |
| P1-CORE-01 | compiler | P1-CONTRACT-01、P1-PE-01（模块身份） | x64 解码/语义子集、ARM64 生成、AOT 调度、JIT 交接、基础失效 | 消费 binary 的模块身份 |
| P1-PLATFORM-01 | platform | P1-CONTRACT-01 | kernel32/user32 基础子集、Guest/Host ABI 桥、回调、LastError、线程状态 | 先做测试族需要的 API |
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
