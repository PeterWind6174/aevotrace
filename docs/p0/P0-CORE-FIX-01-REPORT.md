# P0-CORE-FIX-01 报告：F1–F5 执行核心正确性修复

日期：2026-11-05。状态：**完成并已顺序集成到 `p0/engine-spike`；P0 整体仍未通过**。本报告不宣称首版完成、真实应用支持或性能提升。

## 1. 版本、模型与工作区

- main（任务记录前）：`3d8e2e0`；本任务记录提交后 main `691c6cc` 起（后续文档提交另计）。
- ENGINE 基线：`a4ef11c723c929a61030ef3e0ae8fc8fba537ab2`。
- 修复分支：`p0/core-fix-01`，worktree `.worktrees/p0-core-fix`。
- 代码修复 commit：`9f21649b2a0470d2d6dc4bea576785af22ac6e8c`。
- 报告 commit / 分支头：`8c0fc15087221e408e84f4b7adf50fc67f2a9abf`。
- 集成：`p0/engine-spike` 已 fast-forward 到 `8c0fc15`；未 push、未合并 main。
- 模型/档位：主会话、compiler、QA、reviewer 四个会话均由 OpenCode 会话库 `session_v2.model` 核对为 `opencode-go/deepseek-v4.1-flash` variant `max`；证据 `.local/results/P0-CORE-FIX-01/model-verification.json` 与 `MODEL-VERIFICATION.md`。
- 执行方式：OpenCode 桌面端原生子会话；未运行 `opencode run` 或 CLI worker。

## 2. 复现（未修改的 `a4ef11c`）

独立构建/ctest 基线通过；用 ORC JIT/AOT probe 复现：

| 项 | 基线行为 | 要求 |
| --- | --- | --- |
| F1 | `lock add/inc/and/or/xor/sub/dec` 翻译成功并执行普通 load/store（如 `0x2a→0x2b`） | 显式拒绝所有 LOCK 形式 |
| F2 | `imul eax,ecx,7`（EAX=10/ECX=3）得 70；负数/16 位三操作数同样误用旧目的 | 得 21/-9；2/3 操作数均正确 |
| F3 | 非法 load 后同块 store 仍写 123；RAX/RSP 被改；`error_pc` 为块首 | 故障立即停止、无副作用、精确 PC |
| F4 | `pop rsp` 得旧 RSP+8；`pushw/popw` 被按 8 字节接受 | RSP 正确；16 位显式拒绝 |
| F5 | 64 位 `-1*1` CF=OF=1；`min*1` CF=1；溢出漏报 | 64 位符号扩展正确 |

复现证据：`.local/results/P0-CORE-FIX-01/repro-compiler/`（REPRO.md、summary、日志、哈希）。

## 3. 修复

- **F1**：`x64.cpp` 保留 Capstone `X86_PREFIX_LOCK`；`translator.cpp` 在 IR 前拒绝所有 LOCK 形式，错误含地址/助记符；emitter 增加第二道拒绝。
- **F2**：三操作数用 `operand[1] × operand[2]`，二操作数用旧 dst × src；覆盖 16/32/64 位、dst≠src、正负；one-operand 显式拒绝。
- **F3**：所有 load/store/栈/terminator 经 `load_mem`/`store_mem`；helper 后立即检查 `error_code`，失败分支到每块 `fault` 返回；调用前写当前指令 RIP，保证 `error_pc` 精确；写内存成功后才提交 flags/RSP/RIP。
- **F4**：`pop rsp` 直接写弹出值，跳过 old RSP+8；16 位 push/pop 在 translator/emitter 拒绝。
- **F5**：width=8 的截断结果先用 `(__int128)(int64_t)r` 符号扩展，再与完整有符号乘积比较；1/2/4 位保持原逻辑。

## 4. 测试与结果

- `cmake`/`ninja`：configure 0、build 0。
- `ctest`：**2/2 通过**（原 `p0_engine_closed_loop` + 新 `p0_core_fix_semantics`）。
- 原 5 case：**22 PASS / 0 FAIL**，`reference.diff` 0 字节；AOT/JIT/ASLR/负例保持。
- 新 semantics：**364 PASS / 0 FAIL**；F1 10 形式、F2 ORC ARM64 执行、F3 JIT+AOT（含第二条指令故障 PC=0x140001005、无寄存器/内存/flags/RSP/RIP 副作用）、F4 `pop rsp`/16 位拒绝、F5 helper 25 例 + JIT 22 例、4/2/1 位回归。
- 独立 QA：`P0-CORE-FIX-QA-01` PASS；`checks.tsv` 123 PASS + 1 N/A（IMUL AOT by design）；`ssh win` 重跑参考输出一致（CRLF/LF 归一化），参考 exe `80b6429f...`。
- Reviewer：未发现 F1–F5 代码级阻断，建议合并；非阻断测试补强与零 AOT block 非法 IR 另列。

## 5. 关键哈希

| 项 | SHA-256 |
| --- | --- |
| `guest_x64.obj` | `a12fa7fff6087e2e0ae95a09e395cd41874f5871674a4feda0cbff8ca9da7b1e` |
| `reference_stdout.txt` | `7d623c9acca20c8a546648a50067161e1e5ee7c0fd08c69ad994bfa3fff81a9c` |
| `guest_aot.ll` | `df9aa1d6902aaed26995e7e5cfc08022f817ee95c8301065c1438b3804b21d1f` |
| `guest_aot.o` | `5b158e59aae4ef0cd869c8a161fd48345a2723db022c21bdf60c36e7fb2beb8d` |
| 实现构建 `libguest_aot.dylib` | `a520ea44fe552c4602b21a11db8078f75bd04d2826b760a15d5b6e39bff65f72` |
| 实现构建 `aev-runner` | `86142db8015ea6da4a8fec0ea4176d10cf768da1bd3fd0398a8e974de9444c77` |
| QA Windows 参考 exe | `80b6429f0af0c1cbc84fd1fcc50d98023c4988f9bf57a47770c9c23f3cf5e12f` |

跨构建目录的 Mach-O dylib/runner 哈希不同（`LC_ID_DYLIB`/install name 嵌绝对路径）；IR/object/测试脚本/输入哈希可复现。完整列表见 `.local/results/P0-CORE-FIX-01/fix/`、`P0-CORE-FIX-QA-01/`。

## 6. 非阻断遗留 / 下一轮最小任务

1. F3 回归补 `add/inc/neg/logic [mem]` 故障 flags，并走 `aev_aot_run`→`jit_dispatch` 的返回路径。
2. F1 `translate_fail` 断言失败时无 `.ll/.h`；emitter 对 terminator 的 LOCK 第二道检查。
3. F4 `push imm8/imm32` 成功回归；`pushw $imm` 负例。
4. F5 JIT 用例预置 CF/OF，验证 0/0 确实清零。
5. one-operand IMUL 诊断文案区分 8/16/32/64 位。
6. 零 AOT block 函数生成 `[0 x %BlockEntry]` 非法 IR（基线同样存在）另立任务：修正空表 IR 或在 `aot_blocks==0` 明确失败。
7. 之后才回到 P0 收尾：共同回调 ABI、ENGINE 与双 GUI 最小集成、Trace、版本失效。**本轮不启动这些实现。**

P0 整体仍未通过；F1–F5 关闭不等于 P0 通过。
