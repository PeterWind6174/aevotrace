# P0-PE-GUI-01 compiler 实施报告：持久 PE session 与 C ABI

日期：2026-11-07。角色：compiler。分支/worktree：`p0/pe-session-01` / `.worktrees/p0-pe-session`。基线：`b181260`（`docs(p0): freeze minimal PE session contract for GUI integration`）。实现提交：`a37c836`（`feat(p0-pe): extract persistent PE session core and C ABI`）。

本报告只覆盖 compiler owner 范围：共享 PE session core、`libaev_pe_session.dylib` C ABI、`aev-pe-runner` 薄 CLI、engine CMake 与 headless C ABI 测试。GUI/platform、Windows stateful reference、最终 QA/reviewer 不在本 worker 完成范围内。

> **协调者注（2026-11-07，最终集成）**：本文件是 compiler `a37c836` 时的历史实施报告。最终集成在 `73fbeb3`/`c5adf4f` 将第 9 个活动 session 改为 `INVALID_ARGUMENT/2`（与冻结契约和旧 runner 一致），并修复 JIT step budget→`STEP_LIMIT=8`；`p0_pe_session` 最终 **233/233**。最终结论以 `docs/p0/P0-PE-GUI-01-REPORT-20261107.md` 为准。

## 1. 修改文件

| 文件 | 内容 |
| --- | --- |
| `spikes/p0-engine/src/pe_session_core.hpp` | 内部 `Session` / `SessionTable` / core result/counts/observer 接口 |
| `spikes/p0-engine/src/pe_session_core.cpp` | 从 `pe_runner.cpp` 抽取的模块装载、Program、AOT 身份校验、JIT、HostImport、Trace、`run_call`、计数与销毁 |
| `spikes/p0-engine/src/pe_session.cpp` | `include/aev_pe_session.h` 全部 C ABI 函数；类型/大小/owner/重入/错误缓冲/observer 适配，不复制执行逻辑 |
| `spikes/p0-engine/src/pe_runner.cpp` | 改为同一 core 的薄 CLI/JSON/dump/`--reload`/`--expect-error` 消费者 |
| `spikes/p0-engine/CMakeLists.txt` | 新增 `aev_pe_session_core` STATIC、`aev_pe_session` SHARED；runner 与 shared lib 共用 core；新增 ctest `p0_pe_session` |
| `spikes/p0-engine/scripts/pe_session_test.py` | ctypes 直载 `libaev_pe_session.dylib` 的 headless 验收 |
| `spikes/p0-engine/scripts/run_pe_session_test.sh` | ctest 包装器 |
| `docs/p0/P0-PE-GUI-01-COMPILER-REPORT.md` | 本报告 |

未修改冻结 header `include/aev_pe_session.h`、既有 Guest/callback/artifact/translator/trace ABI、PE fixture DLL/registry、其他 worktree。

## 2. 实现要点

### 2.1 共享 core

- 一个 `aev::pe::Session` 拥有：`pe::Module`、`Program`、AOT dylib 校验与 selected 表、`JitRuntime`、HostImport 固定实现（`aevhost.dll!AevUpdate`，id=1，args=2）、`TraceWriter`、累计计数、owner 线程、`in_call` 原子标志。
- `pe::load_module` / `discover_functions` / `build_program_for_module` / `JitRuntime` / `aev_mem.c` 全部复用既有实现，没有第二份 loader、dispatcher、JIT cache 或 trace 格式。
- AOT 身份校验沿用既有模块/输入/FNV/entry export/import thunk/all-block/selected block/符号表检查；不匹配返回 `ABI_MISMATCH`。
- 失败路径统一 `cleanup()`：关闭 trace、释放 JIT、`dlclose` AOT、清空 Module/Program/region/selected 表；失败 create 不进入 handle table。
- `SessionTable` 为 8 slot generation token：`(slot+1) | (generation << 32)`；destroy 递增 generation 并跳过 0；stale token 不解引用对象。

### 2.2 C ABI

- `struct_size`：除 `aev_pe_session_abi_info`（允许 0 或精确大小）外，输出结构必须精确等于库内 `sizeof`；`abi_version` 由库覆盖。
- `create` 严格检查 `struct_size`/`abi_version`/`flags`/`reserved0`/`reserved1`；路径、load base/AOT/entry 规则由 core 细分。
- owner 固定为 create 线程；非 owner 的 info/counts/export/selected/block/call/trace/destroy/observer 返回 `WRONG_THREAD`。
- 活动 call 内同 owner 的 call/destroy/trace/set_observer 返回 `REENTRY`（call 的重入判定先于导出名校验）。
- 所有 C ABI 入口用 `try/catch` 隔离 C++ 异常；错误缓冲 NUL 截断。
- observer 通过 ABI 转换 trampoline 调用；核心路径和 wrapper 都捕获回调异常，异常不改变结果或不逃出 C ABI。

### 2.3 runner

- 保留原 CLI 选项、JSON 字段、`--info`、`--dump-image`、`--dump-blocks`、`--dump-relocations`、`--reload`、`--expect-error`、stale handle 和退出码。
- 装载失败统一按既有 runner 契约输出 `status=3 LOAD`；session 层仍返回细分状态。
- `--reload` 每次创建新 Session；旧 trace 的 `iteration * 100000` call 偏移通过 core 内部 runner-only `set_trace_call_base` 保持；C ABI trace 仍在每次 open 时从 1 编号。
- `host_import_calls` 来自 session 实际计数，不再是 runner 影子计数。

## 3. 契约差异 / 风险说明

1. **handle table 满**：第 9 个活动 session create 返回 `INTERNAL/12`（错误文本 `session table full`）。契约未固定该状态；测试按此实现验证。旧 runner 的 table 满是 `INVALID_ARGUMENT/2`，runner 路径仍保持旧状态。
2. **AOT Hybrid 不是零 JIT**：固定 fixture 的完整 AOT artifact manifest 为 22 AOT / 3 JIT blocks（`aev_translate` 当前覆盖策略）。本测试验证 AOT block 真实执行、未覆盖块真实 JIT、第二次相同输入复用 JIT cache；没有把 hybrid 结果误写成 full-AOT 零 JIT。零 JIT 的 trace→supplement→新进程路线属于平台/QA 的 GUI 场景，本 worker 未宣称完成。
3. **trace_records**：统计 header 之外的事件行（block/host_import/dispatch_miss/call_end）；关闭后保留，重新 open 清零。与契约“已写事件数”一致，但 header 不计入。
4. **trace call 编号**：`trace_open` 从 1 重新编号；runner `--reload` 的旧偏移仅通过内部方法保留，C ABI 不暴露。
5. **失败结果的累计字段**：未进入 Guest 的失败（未知导出、stale、wrong-thread、reentry）不增加 `calls_total`，`AevPeCallResult.total_*` 保持当前值/零；契约只对成功进入 Guest 的 call index 和累计计数作要求。
6. **observer 异常测试**：ctypes 的 Python 回调抛出异常由 ctypes 自身打印并吞掉；测试验证结果/状态不受影响。核心的 C++ `try/catch` 已实现，但没有另建 C++ 注入器验证“抛 C++ 异常的回调”。
7. **错误文本**：session 层 AOT/registry 细分类与旧 runner 文本基本沿用；runner 对装载失败仍统一 `status=3`。若 GUI 依赖精确错误文本，应以本提交的 session 输出为准。

## 4. 构建与验收

环境：macOS 27.0.1（26A434），arm64，Apple clang 21.0.0，CMake 4.4.4，LLVM 23.1.4（Homebrew），Capstone（Homebrew）。构建目录 `.local/build-pe-session`。

```sh
cmake -S spikes/p0-engine -B .local/build-pe-session -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build .local/build-pe-session -j 8
ctest --test-dir .local/build-pe-session --output-on-failure
```

退出码：configure `0`，build `0`，ctest `0`。

### 4.1 ctest 结果（7/7）

| 测试 | 结果 |
| --- | --- |
| `p0_engine_closed_loop` | Passed |
| `p0_core_fix_semantics` | Passed |
| `p0_engine_empty_aot` | Passed |
| `p0_callback_bridge` | Passed |
| `p0_trace_aot` | Passed |
| `p0_pe_dll` | Passed（147/147 checks） |
| `p0_pe_session`（新增） | Passed（226/226 checks） |

原始 6 个 ctest 全部保留且通过；未删除/弱化失败、权限或 CF 测试。

### 4.2 新测试覆盖

`p0_pe_session` 用 ctypes 直接加载 `libaev_pe_session.dylib`，覆盖：

- ABI info/struct sizes、status name、错误 struct_size/abi/flags/reserved。
- create 后 load_count=1、trace 自动 open、info/export/find/selected/all/block、owner 查询。
- 同一 session Reset(0x1234)+3 组 Compute（含独立输入 `123456789,7,6`）+ State/Calls，对照独立 Python oracle。
- 同一 session 连续 100 次 Compute：DLL Calls=103、State、host_import、load_count 恒为 1、无重建。
- 8 slot handle table 满、第 9 个 INTERNAL、generation stale、double destroy、handle 0。
- 两 session 隔离、destroy 一个后另一个继续。
- Python 非 owner 线程的 counts/call/destroy 返回 13。
- observer 内重入 call/destroy/trace_close 返回 10；observer 异常不改变结果；observer 清除。
- 截断 DLL、坏 registry、空 registry/未知 import、无 host 实现的 registry ID、AOT 缺文件/错 base/错 ABI、不可打开 trace。
- `AevDllReadAt(0xDEAD0000)` → MEMORY/6、fault kind/addr/size/access/error_pc、计数；随后 Compute 恢复。
- trace create/open/close/重复 open/截断/重开、header ABI 4、all_blocks/aot_selected、实际 jit_compile/host_import/call_end 事件、call 从 1 编号、trace_records 与事件行一致。
- 真实 `aev-translate` + clang 生成的 PE AOT artifact；AOT selected/block_info/info sha；AOT Reset/Compute 与 JIT fallback cache。

### 4.3 哈希

实现提交 `a37c836`。最终构建产物与输入：

| 项 | SHA-256 |
| --- | --- |
| `libaev_pe_session.dylib` | `db22d998fed81302d28d4f03389e808f5292caef15d24a490943730f230ee2ce` |
| `aev-pe-runner` | `3d5229c2bc422a794fe769e58b9baebb52871f0ebcbb1e051b3bc9fae8ef4b61` |
| `libaev_bridge.dylib`（回归） | `d349b9115b5ac13c84fc558cf121f5fb06c67ab5b397fdda630afbdedf0564b7` |
| `aevguest.dll` | `1f82b67978dc13e1e31c1262497a9fe186d39f637949b94408c646d4b11152b0` |
| `aevhost.dll` | `0eabd013127ef5cc4e73c149aa0988b3d88dbb009d491c0c895e7a0d479757d1` |

fixture DLL 字节未修改；`pe_session_test.py` 启动时校验 `testdata/pe/SHA256SUMS`。

### 4.4 证据路径（ignored 本地目录）

- `.local/results/P0-PE-GUI-01/compiler/configure.log`
- `.local/results/P0-PE-GUI-01/compiler/build-final.log`
- `.local/results/P0-PE-GUI-01/compiler/ctest-final.log`
- `.local/results/P0-PE-GUI-01/compiler/hashes-final.txt`
- `.local/results/P0-PE-GUI-01/compiler/pe-session/checks.json`（226 项，0 失败）
- `.local/results/P0-PE-GUI-01/compiler/pe-session/artifacts/`、`logs/`
- `.local/build-pe-session/results-pe-session/`（ctest 运行目录）
- `.local/build-pe-session/results-pe-dll/checks.json`（147 项，0 失败）

## 5. 未测项 / 移交

- Windows 原生 reference（Reset+3 输入+100 次、result/state/calls/helper 序列）未在本 worker 运行；由 binary/QA 完成。
- compat/native GUI 的实际控件、主线程、截图、100 次按钮、错误按钮、AOT trace 补充后零 JIT 新进程场景未在本 worker 验证；由 platform/QA 完成。
- session 库的 `create/load` 在真实 AppKit 主线程上的 owner 行为未验证；headless create 线程即 owner，已测跨线程拒绝。
- trace 文件 “open 后磁盘满/写入中途失败” 未模拟；当前实现只在 open/header 阶段返回 `TRACE`，事件写入沿用旧 runner 的不检查流状态行为。
- `AevDllCallPtr` 间接目标、权限/CF 回归仍由原 `p0_pe_dll` 147 项覆盖，未在本新测试重复。
- generation 2^32 回绕未实际运行；仅验证 generation 递增/跳过 0 的代码路径与连续 8 slot。
- 没有建立 C++ 抛异常 observer 的独立可执行注入器；Python/ctypes 异常路径已测。
- 本会话模型标记为 opencode-go / DeepSeek V4.1 Flash；Max 档位无法从当前进程内独立核验，按主控指定执行。未使用 `opencode run` 或其他 CLI 模型 worker，未派生 agent。
