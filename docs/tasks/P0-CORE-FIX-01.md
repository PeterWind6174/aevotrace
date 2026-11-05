# P0-CORE-FIX-01：执行核心 F1–F5 正确性修复与回归

状态：完成（修复已顺序集成到 `p0/engine-spike`；独立 QA/reviewer PASS；非阻断测试补强与零 AOT block 缺陷另列）。

## 输入与责任

- 阶段、需求 ID、前置任务：P0 收尾；R01/R06；依赖 P0-ENGINE-01 `a4ef11c723c929a61030ef3e0ae8fc8fba537ab2` 与独立审查 `.local/results/CODEX-REVIEW-20261105/`。
- owner / reviewer / QA：compiler / reviewer / qa。
- 基线 commit：main `3d8e2e04a96d9640db1044b2933d949d246943f5`；ENGINE `a4ef11c...`；UI `847dad7...` 仅作参照，不在本轮修改。
- 分支 / worktree：`p0/core-fix-01` / `.worktrees/p0-core-fix`（从 `a4ef11c` 创建）；构建 `.local/build-core-fix`；结果 `.local/results/P0-CORE-FIX-01/`。
- 公共 ABI / artifact / 平台接口：沿用 ENGINE spike ABI；本轮不冻结、不扩展 PE/GUI/真实应用。

## 模型与执行约束

- 使用 OpenCode 桌面端及原生会话/子 agent；不运行 `opencode run` 或其他 CLI worker。
- 用户指定 OpenCode Go / DeepSeek V4.1 Flash / Max。主会话已从 OpenCode 会话库核对为 `opencode-go/deepseek-v4.1-flash` variant `max`；子会话派发时显式指定同一 provider/model/variant，并在会话库核对实际 `model` 字段，不能只凭文字自述。
- 并行写代码必须独立 worktree；无法确认隔离则串行。worker 不派生 agent。

## 目标与范围

先在隔离 worktree 独立复现，再修复：

- **F1 LOCK 前缀**：当前 `lock add` 被接受并生成普通 load/store。未实现原子语义时，显式拒绝所有 LOCK 前缀形式，覆盖 `LOCK ADD/INC/AND/OR/XOR/SUB/DEC/CMPXCHG/XADD` 等已支持助记符的加锁形式；不得只测本身未支持的 XADD。
- **F2 三操作数 IMUL**：`imul eax,ecx,7` 应从 ECX=3 得 21，不得用旧 EAX=10；分别验证二/三操作数、dst≠src、负数、8/16/32/64 位；检查实际 JIT 执行路径，不只检查 IR 文本。
- **F3 内存故障副作用**：非法 load/store/栈访问后立即停止当前块，记录正确 Guest 故障 PC；故障指令及后续指令的寄存器/内存副作用不得提交；dispatcher 返回错误。
- **F4 POP RSP**：`pop rsp` 必须把弹出值写入 RSP，而不是被旧 RSP+8 覆盖；16 位 push/pop 若不支持必须显式拒绝，不能按 8 字节接受。
- **F5 64 位 IMUL CF/OF**：`-1 × 1` 应 CF=OF=0；修正截断结果的符号扩展；覆盖负数无溢出、正/负溢出、最小值边界；不以未定义 ZF/SF/PF 判错。

允许修改：`.worktrees/p0-core-fix/spikes/p0-engine/{src,include,testdata,scripts,CMakeLists.txt,README.md,REPORT.md}`。
不包含：GUI、Unicorn 桥、PE loader、真实应用、API 扩展、性能测量、大规模重构、main 文档。

## 验收

- 先在未修改的 `a4ef11c` 上独立复现 F1–F5，保存命令/退出码/输入哈希/日志；不复用审查结论作为唯一证据。
- 修复后新增回归纳入 ENGINE 测试体系（`ctest`/`scripts/run_tests.sh`），覆盖 F1–F5 及位宽/符号/故障/栈特例；原 5 个闭环用例仍通过。
- F2 必须实际执行 ORC JIT 生成的 ARM64 代码；F3 必须检查目标内存/寄存器未被写入、`error_pc` 正确；F4 必须检查 `pop rsp` 返回值/后续 `mov rax,rsp`；F5 必须直接测试 `aev_flags_imul` 与 JIT/AOT 路径。
- 记录 commit、输入/产物哈希、命令、退出码、日志；QA 在指定 commit 独立复跑；reviewer 检查 diff/语义/证据。
- 不得删除反例、改期望值、屏蔽断言或假返回值关闭问题。若原审查有误，提交可复现反证。

## 完成报告

- 提交/diff、命令/退出码、哈希、日志、F1–F5 逐项结果、未测项、是否集成。

## 实施报告（2026-11-05）

- 分支 `p0/core-fix-01`；基线 `a4ef11c`；代码提交 `9f21649b2a0470d2d6dc4bea576785af22ac6e8c`；报告提交/分支头 `8c0fc15087221e408e84f4b7adf50fc67f2a9abf`。未 push、未合并 main；主工作树/旧审查材料未改。
- 先独立复现 F1–F5，确认审查结论；复现证据 `.local/results/P0-CORE-FIX-01/repro-compiler/`。
- 修复：LOCK 前缀在 translator/emitter 显式拒绝；三操作数 IMUL 用 operand[1]×operand[2]；所有 load/store/栈/terminator 经 fault guard，故障立即返回并保留精确 `error_pc`，写内存成功后才提交 flags/RSP/RIP；`pop rsp` 正确写 RSP，16 位 push/pop 拒绝；64 位 IMUL CF/OF 符号扩展修正。
- 测试：`ctest` 2/2 通过；原 5 case 22 PASS/0 FAIL，`reference.diff` 空；新 `p0_core_fix_semantics` 364 PASS/0 FAIL（F1 10 形式、F2 ORC JIT、F3 JIT+AOT、F4、F5）。
- 报告：`.worktrees/p0-core-fix/spikes/p0-engine/REPORT-CORE-FIX-01.md`；原始命令/哈希/日志 `.local/results/P0-CORE-FIX-01/fix/`。
- 限制：8 位 one-operand IMUL 仍显式拒绝；原子/TSO 未实现；`imul` 仍 AOT 排除；未涉及 PE/异常/线程/失效/GUI/真实应用/性能。
- QA：P0-CORE-FIX-QA-01 独立复跑 PASS（见 `.local/results/P0-CORE-FIX-QA-01/REPORT.md`）；发现一个基线同样存在的零 AOT block 非法 IR 范围外缺陷，另立任务。
- 集成：`p0/engine-spike` 已 fast-forward 到 `8c0fc15`（原 `a4ef11c`），修复分支 `p0/core-fix-01` 保留；未 push、未合并 main。
- 终审：reviewer 未发现 F1–F5 代码级阻断，建议合并；F5 的“AOT 生成 IMUL block”按 spike 的 JIT-only 设计接受为限制（AOT dylib 导出的 helper 已测，AOT 不生成 IMUL block）。
- 非阻断补强（下一轮/另立任务）：F3 faulting arithmetic/flags 与 JIT dispatcher 返回回归、F1 失败时不写 `.ll/.h` 断言、F4 push imm 回归、F5 JIT 预置 CF/OF、one-operand IMUL 诊断文案、零 AOT block 非法 IR。
