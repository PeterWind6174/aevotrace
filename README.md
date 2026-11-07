# Aevotrace · 溯迹

**从执行中发现代码，在运行前完成重编译。**

Aevotrace is a developer-oriented cross-architecture, cross-OS binary conversion project, starting with Windows x86-64 to macOS ARM64.

Aevotrace 面向开发者，将已有程序的机器代码与平台依赖转换、适配并构建为目标平台应用。源码不是必需输入；有源码、调试符号或适配提示时可以额外利用。通过静态分析、受控执行、多轮记录与动态代码版本捕获补充代码发现，正式运行优先执行 AOT 产物，允许 JIT 回退，将新增记录用于后续离线构建。

**当前状态：已验证的原型阶段（2026-11-07）。main 已统一固定 x64 PE DLL → ARM64 AOT/真实 JIT → 持久 session → compat/native GUI → 实际 Trace → 补充 AOT 的实验闭环，并包含已复核的正确性修复。P0 总体验收和首版产品仍未完成；尚无通用 EXE 转换器、独立 `.app`、真实应用转换验收或性能结论。** 当前能力与未完成项见 [当前状态](docs/CURRENT_STATUS.md)，本批收尾见 [统一合并记录](docs/p0/INTEGRATION-20261107.md)。

公开仓库：[PeterWind6174/aevotrace](https://github.com/PeterWind6174/aevotrace)。

## 已合入 main 的实验

- [执行核心实验](spikes/p0-engine/README.md)：有界 COFF/PE 装载、AOT/JIT、Host↔Guest 回调、Trace/产物身份及 PE session C ABI。
- [双 GUI 后端实验](spikes/p0-ui/README.md)：当前 `run_engine` / `run_pe_gui` 入口共用实际 AOT/JIT 核心；`compat` 为自绘语义实验，`native` 使用 AppKit。早期 Unicorn 入口仅保留作历史实验，不是当前集成链。
- [本批合并与归档记录](docs/p0/INTEGRATION-20261107.md)、[首轮合并历史](docs/p0/INTEGRATION-20261105.md)：代码版本、验证口径、工作树归档和恢复方式。

执行核心可在项目根目录构建验证（需其 README 列出的本地工具链与依赖）：

```sh
cmake -S spikes/p0-engine -B .local/build-main -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build .local/build-main -j 4
ctest --test-dir .local/build-main --output-on-failure
```

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

项目内提供 [OpenCode 主控配置](.opencode/agents/aevotrace-coordinator.md)和[任务模板](docs/tasks/TASK_TEMPLATE.md)。2026-11-05 已执行 P0 并行任务及独立 QA/reviewer；早期 CLI worker 仅保留为历史记录，后续使用桌面端与 Go / DeepSeek V4.1 Flash / Max，见协作方案。

## 长期方向

逐步增加源 ISA、源操作系统、目标 ISA 和目标操作系统的组合；每种组合独立声明能力范围和测试结果。当前仅规划 Windows x64 → macOS ARM64，其他组合尚未实现。

## 许可

项目自有内容采用 **PolyForm Noncommercial License 1.0.0**。非商业用途及标准文本列明的机构使用例外，以 [LICENSE](LICENSE) 为准。超出公开许可范围的商业使用，需取得相关权利人的另行书面许可，见 [COMMERCIAL.md](COMMERCIAL.md)。

本项目采用 **source-available（源码公开）** 模式，不宣称采用 OSI 认可的开源许可。第三方材料保留各自的权利和许可。
