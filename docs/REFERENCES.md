# 资料与证据边界

核查日期：2026-11-05。下列链接用于技术与许可依据，未集成第三方实现。仓库分支会变化；真正采用依赖前须固定提交并复核许可。R1–R15 来自原 AArch64/Switch 构想，保留为历史研究线索，不代表首版依赖选择。

| 编号 | 一手资料 | 本文使用范围与核查边界 |
| --- | --- | --- |
| R1 | [BinRec: Dynamic Binary Lifting and Recompilation，EuroSys 2020](https://download.vusec.net/papers/binrec_eurosys20.pdf) | 动态 lifting、Trace 合并、未观察控制流及增量恢复；不外推为 Switch 兼容性 |
| R2 | [trailofbits/binrec-tob](https://github.com/trailofbits/binrec-tob) | README 的 S2E、LLVM 和 x86/Linux 环境边界；未构建 |
| R3 | [Apple：Porting just-in-time compilers to Apple silicon](https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon) | Hardened Runtime、MAP_JIT、写入保护、签名和指令缓存；通过官方 Markdown 版本读取，接口仍需按目标版本核实 |
| R4 | [Apple：Hypervisor](https://developer.apple.com/documentation/hypervisor) | 官方 Markdown 说明 VM/vCPU、Guest 内存映射、退出处理及 entitlement；未运行实验 |
| R5 | [Ryubing：Setup & Configuration Guide / PPTC](https://docs.ryujinx.app/guides/setup-guide/#profiled-persistent-translation-cache) | CPU 函数翻译持久化；不把其启动收益当作本项目测量 |
| R6 | [Ryujinx 初始 AppleHv 提交 a53cfdab78 的历史源码镜像](https://git.axenov.dev/Museum/ryujinx/commit/a53cfdab78c382677eb826bd5bedb58b3b838796?show-outdated=&style=split&whitespace=show-all) | 检索可见原提交与 AppleHv 文件片段，证明历史工程路线；原博客及镜像直连存在访问限制，未做完整源码审计 |
| R7 | [FEX-2601 发布记录](https://github.com/FEX-Emu/FEX/releases/tag/FEX-2601)；[开发者关于 AOT IR 与 AOT OBJ 的说明（2021）](https://skmp.dev/blog/fex-aotir/) | 离线编译、代码映射、加载、验证，以及 IR 与最终代码缓存的区别；旧文不代表 2026 年完整状态 |
| R8 | [QEMU TCG Plugins](https://www.qemu.org/docs/master/devel/tcg-plugins.html) | TCG 插件的基本块/指令/访存观察；不推断存在 Switch 平台实现 |
| R9 | [DynamoRIO 官方仓库](https://github.com/DynamoRIO/dynamorio) | 官方 README 列出的架构和平台；macOS 支持仍标为进行中，不假定 Apple Silicon 完整支持 |
| R10 | [Frida Stalker 官方文档](https://frida.re/docs/stalker/) | 线程追踪的工作层次；未验证对目标 Guest 执行后端的可见性 |
| R11 | [suyu-emu/suyu-main，mk8-recomp 分支 README](https://github.com/suyu-emu/suyu-main/blob/mk8-recomp/README.md) | 项目自报 Static/Hybrid、macOS ARM64 和有限场景测试；README 明确仍实验性，未由本项目复现 |
| R12 | [N64Recomp 官方仓库](https://github.com/N64Recomp/N64Recomp) | 静态重编译及运行时接入的参考，不视为 Trace 驱动 AOT 实现 |
| R13 | [PolyForm Noncommercial 1.0.0 官方文本](https://polyformproject.org/licenses/noncommercial/1.0.0) | 非商业许可、机构例外、修改及再分发、通知等条款；LICENSE 保持官方文本，不添加改变其效果的限制 |
| R14 | [OSI：The Open Source Definition](https://opensource.org/osd) | 第 6 条禁止限制商业等使用领域，因此本项目称 source-available |
| R15 | [GNU GPL FAQ：双重许可与修改后版本许可](https://www.gnu.org/licenses/gpl-faq.html#HeardOtherLicense) | 权利人可另行授权其有权授权的代码；不能替其他权利人授予再许可权 |

## 0.2 需求扩展与协作依据

| 编号 | 一手资料 | 使用范围与边界 |
| --- | --- | --- |
| N1 | [Microsoft PE Format](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format) | PE 装载、导入、重定位、TLS 和异常相关元数据；不代表本项目已实现 |
| N2 | [Microsoft x64 Calling Convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170) | Windows x64 调用约定及跨 ABI 桥接的来源 |
| N3 | [Wine 官方介绍](https://www.winehq.org/?page=supported_applications) | Windows API 兼容层的现有路线；官方页面检索可读，首页直连曾返回 403；未选型、未构建或集成 |
| N4 | [Apple：Placing content in a bundle](https://developer.apple.com/documentation/bundleresources/placing-content-in-a-bundle) | macOS 应用包中的代码、资源与布局；不把包装等同于完成代码转换 |
| N5 | [Microsoft：Window Messages](https://learn.microsoft.com/en-us/windows/win32/learnwin32/window-messages)；[Window Procedures](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-procedures) | Windows 消息派发与回调语义；native 适配仍需单独设计 |
| N6 | [Apple：NSApplicationMain](https://developer.apple.com/documentation/appkit/nsapplicationmain) | AppKit 主线程初始化与事件循环入口 |
| N7 | [OpenCode V2 Agents](https://opencode.ai/v2/docs/agents/) | Markdown agent、mode、模型继承和 permissions；配置按 V2 编写，未运行实际模型会话 |
| N8 | [OpenCode V2 CLI](https://opencode.ai/v2/docs/cli/)；[Commands](https://opencode.ai/v2/docs/cli/commands/) | 独立 CLI、run、指定 agent/session 与后台服务；本机 v2.1.3 的 version/run help 已核对 |
| N9 | [OpenCode V2 Permissions](https://opencode.ai/v2/docs/permissions/)；[Config](https://opencode.ai/v2/docs/config/) | V1/V2 字段差异、规则和项目配置；不推断自动 worktree 隔离或已存在集群调度器 |

## 如何使用这些资料

技术事实引用论文、官方文档或源码，并区分作者报告与本项目验证。采用具体实现前再记录固定 commit、许可证文件、版权通知和修改范围。

没有证据支持“任意游戏完整固化”“无需回退”“必然超过 Hypervisor”“全游戏兼容”或具体加速倍数；当前文档不作这些声明。
