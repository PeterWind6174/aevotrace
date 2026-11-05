# P0-ENGINE-01：x64→ARM64 AOT/JIT 最小闭环与执行引擎选型

状态：完成（spike 功能路径经独立 QA PASS；V-1 代码失效、P-1 `.app` 转 P1/P6）。

## 输入与责任

- 阶段、需求 ID、前置任务：P0；R01/R06/R07；依赖 SCOPE 的初步指令范围。
- owner / reviewer / QA：compiler / reviewer / qa。
- 基线 commit 或源码包哈希：任务记录基线 `586662a4ff09465d7d3aa453fb44e5aa6e880c79`；开始实现前记录实际 `git rev-parse HEAD`，完成报告同时记录输入/产物哈希。
- 分支 / worktree / Windows 工作目录：`p0/engine-spike` / `.worktrees/p0-engine`；Windows 任务目录 `C:\Users\PeterWind\AevotraceP0\P0-ENGINE-01`。
- 公共 ABI / artifact / 平台接口版本：spike 内部接口；不得宣称为已冻结 v0 契约。

## 目标与范围

- 可观察的完成结果：
  1. 对至少两条路线做可核查比较：自建 x64 语义 + LLVM AOT/JIT；以及现有执行引擎（QEMU TCG / FEX / Box64 等）作为候选的 AOT 导出/嵌入/许可评估。比较维度：x64 语义覆盖、ARM64 生成、AOT 导出与重定位、JIT 回退、异常恢复、代码失效、构建成本、许可。
  2. 最小真实闭环：输入一个由 Windows 编译产生、具有固定 SHA-256 的 x64 函数/PE 片段；读取其实际机器码，含分支、访存和间接调用；生成 ARM64 Mach-O 目标文件；由新进程加载并执行；结果与 Windows 参考输出或独立参考实现一致。
  3. 强制一个 AOT 未覆盖目标进入 JIT，JIT 编译成功后返回 AOT 代码继续；日志能证明 JIT 实际执行，而不是假返回。
  4. 未实现指令必须显式失败/回退，禁止用返回 0 等占位行为让测试通过。
  5. 反伪证证据：输入 Guest PC/基本块到生成符号的映射；生成代码的反汇编/`otool -tv`；输入参数/分支变异后结果随之变化；在不同加载地址或 ASLR 下加载（至少证明不依赖单一固定地址）；若静态链接，必须明确“重定位/动态加载未验证”。
  6. 内存模型最小项：至少一个未对齐访问、一个跨页访问和一个原子/内存序场景（可实现为诊断或明确未覆盖，但不能静默当作正确）；记录 x86 TSO 与 ARM64 的差异处理方式。
- 允许修改的文件或组件：仅 `.worktrees/p0-engine/` 下的 `spikes/p0-engine/` 与任务报告；不修改 `main` 或其他 worktree。
- 明确不包含的内容：完整 PE 装载器、完整 Win32 API、GUI、真实应用、性能结论、最终 artifact 格式。
- 需要的依赖及许可记录：Capstone 5.0.9（BSD-3-Clause）、LLVM 23.1.4（Apache-2.0 with LLVM exceptions）等；固定版本和许可证文件/链接写入报告。不得引入 GPL 组件到产品路径而不记录组合风险。

## 验收

- 测试程序、输入、准备集和独立验证集：Windows 侧编译 `p0_engine_input.c`（或等价）生成 x64 函数，记录源码/二进制 SHA-256 和反汇编；Mac 侧使用同一字节，不重写业务函数。至少一个未参与 AOT 准备的输入分支/参数；另加一个变异参数验证结果不是固定回放。
- Windows 参考执行步骤：用 MSVC 编译并运行，输出结果、退出码、输入/二进制哈希。
- macOS 目标执行步骤：CMake/Ninja 构建 translator、AOT runner、JIT 路径；`file`/`otool` 检查 ARM64 Mach-O；运行并比较结果；强制 JIT 场景记录计数和返回路径；检查 Guest PC→生成符号映射和反汇编。
- 预期业务输出、状态或副作用：固定输入得到固定结果；AOT 与 JIT 的结果一致；变异输入改变结果；未覆盖指令产生明确诊断或真实 JIT 执行；未对齐/跨页/原子项有结果或明确“未覆盖”。
- GUI 自动/人工部分：无 GUI。
- 必须证明真实生效的路径：AOT 产物在独立进程加载执行；JIT 回退真实编译并返回；不允许只跑 JIT 或只生成文件；不允许 Host 手写业务函数替代输入机器码。

## 完成报告

- 提交或 diff：分支、commit、改动文件。
- 命令、退出码、环境及配置：CMake/Ninja/LLVM/Capstone 版本，完整构建/运行命令。
- 二进制哈希、日志和报告位置：输入 x64 二进制、生成对象、runner 的 SHA-256；原始日志在 `.local/results/P0-ENGINE-01/` 或 worktree 内忽略目录。
- 正确性/性能结果：只报告功能与路径证据；性能只记录测量方法，不作产品结论。
- 审查问题及修复：
- 未测项、已知限制、需要用户操作的具体步骤：
- 后续任务与接口变更：需要 coordinator 冻结的 ABI/artifact 条目，以及 P1 实现建议。

## 完成报告（2026-11-05，spike）

- 提交：分支 `p0/engine-spike`，commit `a4ef11c723c929a61030ef3e0ae8fc8fba537ab2`；只改 `spikes/p0-engine/`，未改主工作树、未 push、未提交构建产物。
- 输入：`testdata/guest_x64.obj` SHA-256 `a12fa7fff6087e2e0ae95a09e395cd41874f5871674a4feda0cbff8ca9da7b1e`；`guest_kernel` `.text` `f8b91b47...15fe`，`guest_op` `.text` `ccd52689...40d4`；Windows 参考 stdout `7d623c9a...1a9c`。
- 命令/退出码：CMake 配置 0；构建 0；ctest 0（1/1）；prep_a/hidden 运行 0；`--bad-op` 3（预期失败）；`divide_guest`/`atomic_guest` 翻译 1（显式 unsupported）。
- AOT：`libguest_aot.dylib` arm64（`d227034c...8fba`）；`otool -tv`/`nm` 显示 9 个 `aev_blk_*` AOT 块；独立 `aev-runner` 进程 `dlopen`/`dlsym`；两次 ASLR 基址 `0x104068000`/`0x104458000`。
- JIT：`imul` 块 `0x140001042` 故意不在 AOT 表；ORC LLJIT 运行时编译，`--dump-jit` 显示 `mul w20,w8,w9` 等 ARM64 指令；`JIT return next_rip=0x140001047` 后 AOT 继续。
- 结果：5 个用例（prep_a/prep_b/hidden/unaligned/crosspage）与 Windows 参考逐字节一致（`reference.diff` 空）；hidden 结果/JIT 次数不同，排除固定回放。
- 负例：未知间接目标 `0xdead0000` 显式 `JIT ERROR`；`cdq`/`lock xadd` 显式翻译失败并带地址/助记符/字节；无假返回值。
- 未测：PE 装载/重定位/导入/TLS/异常/unwind/线程/自修改失效；Guest 页表/权限/跨页 fault；TSO/原子（原子拒绝）；MAP_JIT/Hardened Runtime/分发签名；AOT 运行时版本校验；真实应用；性能。`crosspage` 仅单线性区域 host page 跨界。
- 报告：`spikes/p0-engine/REPORT.md`；证据：`.worktrees/p0-engine/.local/results/P0-ENGINE-01/final/`。
- 待独立 QA 复核；通过后再决定是否合并 spike 代码。
- QA 结果（P0-QA-01）：功能复跑 PASS；QA 重新构建 Windows 参考 exe 哈希 `9159c74f...` 与实现者一致，三方输出逐字相等；AOT/JIT/ASLR/负例均独立确认。V-1（加载时输入/代码版本校验与失效）未实现，按 P1 处理；AOT dylib/runner 的哈希因构建目录 install name 不同而变化，IR/object 可复现。
