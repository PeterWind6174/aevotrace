# Aevotrace · 溯迹

**从执行中发现代码，在运行前完成重编译。**

Aevotrace is a developer-oriented cross-architecture, cross-OS binary conversion project, starting with Windows x86-64 to macOS ARM64.

Aevotrace 面向开发者，将已有程序的机器代码与平台依赖转换、适配并构建为目标平台应用。源码不是必需输入；有源码、调试符号或适配提示时可以额外利用。通过静态分析、受控执行、多轮记录与动态代码版本捕获补充代码发现，正式运行优先执行 AOT 产物，允许 JIT 回退，将新增记录用于后续离线构建。

**当前状态：需求与开发计划阶段。尚无可运行转换器、兼容性清单或性能测量。**

公开仓库：[PeterWind6174/aevotrace](https://github.com/PeterWind6174/aevotrace)。

## 首个版本

- 输入：Windows x86-64 用户态 EXE、DLL 及资源。
- 输出：Apple Silicon 上可独立启动的 macOS ARM64 `.app`。
- 应用：普通桌面 GUI 程序；先以明确的 Win32 能力范围和固定应用验证。
- 执行：AOT 优先，允许 JIT 回退；减少正式运行时编译，测量持续运行性能。
- 两种可选输出：`compat` 随包携带兼容运行库；`native` 将已支持的系统接口与界面适配到 macOS 原生框架。
- 排除驱动和内核组件。Switch 属于后续应用方向。

两种输出模式都需要真实实现和独立验收；增加一个选项不等于支持该模式。`native` 仍可携带必要的 ABI、代码调度与接口适配组件，未支持接口必须明确报告，不能静默退回 `compat`。

## 文档入口

1. [需求规格](docs/需求规格.md)：已确认需求、建议的首版边界、双模式含义与验收。
2. [项目构想与技术路线](docs/项目构想与技术路线.md)：可扩展架构、核心契约和关键技术验证。
3. [开发计划](docs/开发计划.md)：P0–P6 阶段、任务依赖和交付门槛。
4. [OpenCode 协作方案](docs/OpenCode协作方案.md)：角色、worktree、Windows 测试与启动提示。
5. [资料与证据边界](docs/REFERENCES.md)：一手资料与未验证事项。
6. [项目命名](docs/PROJECT_IDENTITY.md)、[商业授权](COMMERCIAL.md)、[贡献说明](CONTRIBUTING.md)。

项目内提供 [OpenCode 主控配置](.opencode/agents/aevotrace-coordinator.md)和[任务模板](docs/tasks/TASK_TEMPLATE.md)。这些是协作准备文件，尚未执行模型任务或验证多 agent 调度。

## 长期方向

逐步增加源 ISA、源操作系统、目标 ISA 和目标操作系统的组合；每种组合独立声明能力范围和测试结果。当前仅规划 Windows x64 → macOS ARM64，其他组合尚未实现。

## 许可

项目自有内容采用 **PolyForm Noncommercial License 1.0.0**。非商业用途及标准文本列明的机构使用例外，以 [LICENSE](LICENSE) 为准。超出公开许可范围的商业使用，需取得相关权利人的另行书面许可，见 [COMMERCIAL.md](COMMERCIAL.md)。

本项目采用 **source-available（源码公开）** 模式，不宣称采用 OSI 认可的开源许可。第三方材料保留各自的权利和许可。
