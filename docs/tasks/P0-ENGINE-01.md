# P0-ENGINE-01：x64→ARM64 AOT/JIT 最小闭环与执行引擎选型

状态：进行中。

## 输入与责任

- 阶段、需求 ID、前置任务：P0；R01/R06/R07；依赖 SCOPE 的初步指令范围。
- owner / reviewer / QA：compiler / reviewer / qa。
- 基线 commit 或源码包哈希：以 worktree 创建时的 `main`（`373becb` 或 coordinator 指定的更新基线）为准；开始实现前记录 `git rev-parse HEAD`。
- 分支 / worktree / Windows 工作目录：`p0/engine-spike` / `.worktrees/p0-engine`；Windows 任务目录 `C:\Users\PeterWind\AevotraceP0\P0-ENGINE-01`。
- 公共 ABI / artifact / 平台接口版本：spike 内部接口；不得宣称为已冻结 v0 契约。

## 目标与范围

- 可观察的完成结果：
  1. 对至少两条路线做可核查比较：自建 x64 语义 + LLVM AOT/JIT；以及现有执行引擎（QEMU TCG / FEX / Box64 等）作为候选的 AOT 导出/嵌入/许可评估。比较维度：x64 语义覆盖、ARM64 生成、AOT 导出与重定位、JIT 回退、异常恢复、代码失效、构建成本、许可。
  2. 最小真实闭环：输入一个由 Windows 编译产生、具有固定 SHA-256 的 x64 函数/PE 片段；读取其实际机器码，含分支、访存和间接调用；生成 ARM64 Mach-O 目标文件；由新进程加载并执行；结果与 Windows 参考输出或独立参考实现一致。
  3. 强制一个 AOT 未覆盖目标进入 JIT，JIT 编译成功后返回 AOT 代码继续；日志能证明 JIT 实际执行，而不是假返回。
  4. 未实现指令必须显式失败/回退，禁止用返回 0 等占位行为让测试通过。
- 允许修改的文件或组件：仅 `.worktrees/p0-engine/` 下的 `spikes/p0-engine/` 与任务报告；不修改 `main` 或其他 worktree。
- 明确不包含的内容：完整 PE 装载器、完整 Win32 API、GUI、真实应用、性能结论、最终 artifact 格式。
- 需要的依赖及许可记录：Capstone 5.0.9（BSD-3-Clause）、LLVM 23.1.4（Apache-2.0 with LLVM exceptions）等；固定版本和许可证文件/链接写入报告。不得引入 GPL 组件到产品路径而不记录组合风险。

## 验收

- 测试程序、输入、准备集和独立验证集：Windows 侧编译 `p0_engine_input.c`（或等价）生成 x64 函数，记录源码/二进制 SHA-256 和反汇编；Mac 侧使用同一字节，不重写业务函数。至少一个未参与 AOT 准备的输入分支/参数。
- Windows 参考执行步骤：用 MSVC 编译并运行，输出结果、退出码、输入/二进制哈希。
- macOS 目标执行步骤：CMake/Ninja 构建 translator、AOT runner、JIT 路径；`file`/`otool` 检查 ARM64 Mach-O；运行并比较结果；强制 JIT 场景记录计数和返回路径。
- 预期业务输出、状态或副作用：固定输入得到固定结果；AOT 与 JIT 的结果一致；未覆盖指令产生明确诊断或真实 JIT 执行。
- GUI 自动/人工部分：无 GUI。
- 必须证明真实生效的路径：AOT 产物在独立进程加载执行；JIT 回退真实编译并返回；不允许只跑 JIT 或只生成文件。

## 完成报告

- 提交或 diff：分支、commit、改动文件。
- 命令、退出码、环境及配置：CMake/Ninja/LLVM/Capstone 版本，完整构建/运行命令。
- 二进制哈希、日志和报告位置：输入 x64 二进制、生成对象、runner 的 SHA-256；原始日志在 `.local/results/P0-ENGINE-01/` 或 worktree 内忽略目录。
- 正确性/性能结果：只报告功能与路径证据；性能只记录测量方法，不作产品结论。
- 审查问题及修复：
- 未测项、已知限制、需要用户操作的具体步骤：
- 后续任务与接口变更：需要 coordinator 冻结的 ABI/artifact 条目，以及 P1 实现建议。
