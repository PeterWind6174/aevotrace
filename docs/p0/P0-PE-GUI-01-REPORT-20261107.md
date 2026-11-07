# P0-PE-GUI-01 报告：持久 PE session 与双 GUI 集成

日期：2026-11-07。状态：**有界任务实现/集成与独立 QA 完成（QA-behavior/Windows、QA-artifact/lifecycle PASS）；最终 reviewer 无阻断、建议进入外部复核；P0 整体与首版仍未完成。** 分支 `p0/pe-gui-01`，基线 `de67e2be2ea2252bb0e2b522ba52aa5d208e5d21`；本轮最终源码提交 `c5adf4fc26c3f360c9931d3833aa45d96eedcd7a`（`c18eeec` + JIT step-budget→STEP_LIMIT=8 修复及 session 测试加强）。main 仍 `c91a3f7`，未合并、未 push、未清理工作树。

本报告只证明：一个固定 Windows x64 PE32+ DLL（含全局数据、DIR64 relocation、一个声明 HostImport）通过版本化 C ABI 的持久 session，在同一 macOS ARM64 进程内由 compat/native GUI 真实控件事件调用；共享 AOT/JIT 核心、实际 DLL State/Calls、100 次连续调用、base JIT→GUI Trace→离线补充→新进程零 JIT AOT、Windows 同序列对照。不证明通用 EXE、完整 Win32/消息泵、TLS/CRT/SEH/COM/线程、真实第三方应用、运行中代码失效、独立 app 分发或性能。

## 1. 版本与契约

| 项 | 值 |
| --- | --- |
| 实现/集成分支 | `p0/pe-gui-01`（`.worktrees/p0-pe-gui-01`） |
| 最终源码 HEAD | `c5adf4fc26c3f360c9931d3833aa45d96eedcd7a` |
| 集成分支 | `p0/pe-gui-01`（最终 HEAD 以 `git rev-parse` 为准；源码/测试为 `c5adf4f`；GUI 干净证据 HEAD `38cfaba`） |
| 基线 | `de67e2b`（`p0/pe-core-fix-01` / `p0/pe-core-fix-integration`） |
| 契约 | [PE-SESSION-CONTRACT-20261107](PE-SESSION-CONTRACT-20261107.md) v1 rev1 |
| C ABI | `include/aev_pe_session.h`，`AEV_PE_SESSION_ABI_VERSION=1` |
| 既有 ABI | Guest 2/296、callback 3、artifact/translator 2、trace sink 1、PE trace file 4；未改 |
| session handle | 64-bit generation token，8 slots |
| owner | create 线程；GUI 为 AppKit 主线程 |
| 状态码 | 0 OK、2 INVALID_ARGUMENT、3 LOAD、4 ABI_MISMATCH、5 UNKNOWN_IMPORT、6 MEMORY、7 JIT、8 STEP_LIMIT、9 STALE_HANDLE、10 REENTRY、11 HOST、12 INTERNAL、13 WRONG_THREAD、14 TRACE |

rev1 吸收了契约只读审查 B1–B7：输出结构 `struct_size` 规则、AOT 文件 IO 失败归 `LOAD`、AOT 已加载身份错配归 `ABI_MISMATCH`、runner 所有 create/load 失败统一 `status=3 LOAD`、observer 拷贝/回调生命周期、trace 只记录实际进入 Guest 的调用、名称 511 字节/NUL 规则。slot full 统一 `INVALID_ARGUMENT`。

实现提交：

| 部分 | 提交 |
| --- | --- |
| 契约/任务/header | `b181260` |
| compiler core/C ABI/runner | `a37c836`，报告 `4f467b3` |
| platform GUI | `3ee254a`、`950e38f`、`9fd4a67` |
| binary Windows reference | `ee468f2` |
| coordinator 审查修正/集成 | `73fbeb3`、`c1b9a95`、`c18eeec` |

实际会话（`session_v2`）：主控 `ses_e4ccea16bffeuIPAGBKgge0zXo`，compiler `ses_e4cbd9888ffeyyULgE88LKIt2W`，platform `ses_e4cbd9887ffesFEdmQBfw75EyM`，binary `ses_e4cbcb808ffe8FMfTWeJSXx7Hc`，契约 reviewer `ses_e4cba4b71ffeVeld29oUOOSto8`，QA-behavior `ses_e4c6748b9ffew03RnttZFc3UhD`，QA-artifact `ses_e4c6748b7ffeLPzoowFPhq4RcR`。记录均为 `opencode-go / deepseek-v4.1-flash / variant=max`（见 `.local/results/P0-PE-GUI-01/models.json`）。

## 2. 实现范围

- `src/pe_session_core.{hpp,cpp}`：共享 `Session`/`SessionTable`，包含 PE Module/Program、AOT 校验、JIT runtime/dispatcher、HostImport（固定 `aevhost!AevUpdate` id=1 args=2）、TraceWriter、run_call、计数、owner/reentry、trace open/close。**没有第二套 PE loader/dispatcher/JIT cache/Trace writer。**
- `src/pe_session.cpp`：`libaev_pe_session.dylib` 薄 C ABI；结构尺寸/ABI、generation handle、owner、stale、reentry、observer 转换/异常边界、错误缓冲、trace 开关。
- `src/pe_runner.cpp`：薄 CLI 消费者；保留既有 JSON 字段、`--info`、`--dump-image/blocks/relocations`、`--reload`、`--expect-error`、stale handle 和退出码；任何 create/load 失败统一 `status=3 LOAD`。
- `spikes/p0-ui/harness/pe_session.py`：ctypes 包装，布局自检 80/208/936/80/536/64/24。
- `pe_host.py`：一个窗口一个持久 session；seed/a/b/n 编辑框、Compute/Reset、结果/State/Calls/load_count/错误显示；负例 ReadAt 控件；非法输入 Host 拒绝；失败不提交。
- `pe_scenario.py`：独立 Python Oracle；Reset + 4 组连续 Compute（含准备集外 `0x13572468,0x2468,5`）+ 100 次真实按钮 + 非法输入 + 合法 ReadAt 覆盖 + 事件驱动 `0xDEAD0000` MEMORY 负例 + 恢复；base/supplemented/实际 AOT/manifest 校验。
- `run_pe_gui.py`/`run_pe_gui_test.sh`：compat/native 双跑、截图、跨后端一致性、forced native failure、依赖/linkage/主线程证据。
- `native_backend.py`：多 edit handle 修复，旧单 edit COFF 路径保留。
- Windows `aevguest_ref.c`/`build_msvc.ps1`：固定 DLL 不变；新增 stateful 模式，重建 reference EXE，构建脚本校验 DLL SHA 并记录命令/退出码。

## 3. 固定输入/Windows

| 文件 | SHA-256 |
| --- | --- |
| `aevguest.dll` | `1f82b67978dc13e1e31c1262497a9fe186d39f637949b94408c646d4b11152b0` |
| `aevhost.dll` | `0eabd013127ef5cc4e73c149aa0988b3d88dbb009d491c0c895e7a0d479757d1` |
| `aevguest_ref.exe` | `f7f7c7434313c8c32c5c876e3b8d0a808d304c6b9cba8d909bf5fe2f622a9fe8` |
| `reference_stateful_stdout.txt` | `5c74e0f8d0bacbdf20f2ecf905de3416797e34583db50d1435fc9bacae551b48` |

Windows：`ssh win`，VS 18 Community，MSVC 19.51.36260 x64；`build_msvc.ps1` 重建后 `cmp`/SHA 证明 DLL 未变，reference EXE 重建。stateful 模式 106 行，覆盖 Reset(0x1234)、4 组 Compute（含独立输入 324478056/9320/5）、100 次 Compute(5,7,3)。原始命令/退出码/`build.log`/stdout 在 `testdata/pe/` 与 `.local/results/P0-PE-GUI-01/`。

## 4. 构建与测试

| 项 | 结果 | 证据 |
| --- | --- | --- |
| configure/build | rc 0，arm64 | `.local/build-pe-gui-int-configure.log`、`-build.log` |
| ctest | **7/7 PASS**（原 6 + `p0_pe_session`） | `.local/build-pe-gui-int-ctest-final2.log` |
| `p0_pe_session` | **233/233** | `.local/build-pe-gui-int/results-pe-session/checks.json` |
| `p0_pe_dll` | **149/149**（147 核心 + 2 新 fixture hash） | `.local/build-pe-gui-int/results-pe-dll/checks.json` |
| 语义 | **791 PASS / 0 FAIL** | `.local/build-pe-gui-int/results-core-fix/summary.txt` |
| Trace/AOT | **78 PASS / 0 FAIL** | `.local/build-pe-gui-int-trace-aot.log` |
| 旧六组 COFF GUI | base/supp/empty × compat/native 各 **44/44**、4 截图 | `.local/results/P0-PE-GUI-01/oldgui-final/` |

原 6 个 ctest 名称/覆盖保留；新 `p0_pe_session` 是独立 ctest，不替换旧测试。

## 5. 持久 PE GUI：base（空 AOT，真实 JIT）

构建产物：`libaev_pe_session.dylib` SHA `ecf6fcce...`，`aev-pe-runner` `3ac30d70...`，base AOT `libpe_empty.dylib` `2931e696...`。本节与 §6 的 GUI summary 已在最终文档 HEAD `38cfaba`（源码 c5adf4f）干净工作树重跑，summary `git_head=38cfaba`、library SHA `ecf6fcce...`；独立 QA 最终报告绑定 c5adf4f。

| backend | checks | 截图 | load_count | calls_total | host_imports | aot_blocks | jit_compiles/execs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| compat | 560/560 | 5 | 1 | 323 | 105 | 0 | 21 / 2534 |
| native | 561/561 | 5 | 1 | 323 | 105 | 0 | 21 / 2534 |

- 跨后端 DLL/AOT/library/业务状态一致；native 使用 `NSWindow`/`NSTextField`/`NSButton`，compat 使用 `CompatWindow`/`CompatStatic`/`CompatEdit`；均 AppKit 主线程。
- 强制 native failure：rc=1，`compat_events=0`、`compat_classes_used=false`，无静默回退。
- 依赖：`unicorn`/`pefile`/`harness.guest_pe`/`engine_host`/`engine_bridge` 未加载；`otool -L` 无 Unicorn/pefile；session 库为 arm64。
- 进程内 session：`runner_process_spawned=false`；无每次按钮启动 runner/解析 runner JSON。

100 次真实按钮循环：`load_count=1`、同一 handle、DLL Calls/State 逐次递增；循环终态 `state=-268985912, calls=104`，与 Windows reference 一致。完整输入序列（Reset + 4 Compute）也与 Windows 106 行一致。

## 6. Trace→补充→新进程 AOT

- base trace 从该 GUI session 实际执行产生；关闭 trace 后执行事件驱动 `AevDllReadAt(0xDEAD0000)` 负例（MEMORY），因此用于补充的 trace 只含成功调用（`call_end.status` 全 0）。
- 为使负例 ReadAt 块在 supplemented 中 AOT，base trace 在循环后执行一次成功的 `AevDllCursorAddress` + `AevDllReadAt(0x18000300c)` 覆盖 ReadAt 块；负例仍通过真实按钮触发，结果/State/Calls 不变，随后 Compute 恢复。
- `aot_supplement.py --input-kind pe` 使用实际 `libpe_empty` base + compat/native GUI trace：selected **21**、promoted **21**、demoted **0**、all **25**；selected 含 `0x1800012f0`（ReadAt）。
- supp AOT `libpe_gui_supp.dylib` SHA `0fc358a0...`；新 GUI 进程（compat/native）：
  - checks 596/597，5 截图，`load_count=1`、`calls_total=323`、`host_imports=105`；
  - **JIT compiles/execs = 0**，AOT blocks 2534，trace `aot_events=21`，`jit_compile/jit_exec=0`；
  - `promoted` 块实际 AOT；结果/State/Calls 与 base 及 Windows 一致；
  - manifest/base/new 的实际 selected/all/SHA/module identity 由 `aot_artifact.py`/`load_artifact` 逐项校验，GUI 模式未放宽。

完整块表/SHA/命令见 `.local/results/P0-PE-GUI-01/artifacts/gui_supp.json` 与 `supplement.log`。

## 7. Session C ABI/生命周期

`p0_pe_session` 233/233 覆盖：ABI/struct size、create/info/counts/exports/selected/all/block、Reset+3 组 Compute+独立输入、100 次连续 call、load_count=1、双 session 隔离、8 slot/第 9 个、stale/double-destroy、wrong-thread=13、observer 重入 call/destroy/trace=10、observer 异常不传播、部分加载失败、missing/unloadable AOT=LOAD、wrong base/ABI=ABI_MISMATCH、unknown import/registry 错配、内存故障 MEMORY 后恢复、trace open/close/reopen/truncate、失败 call 不写 `call:0`、输出 struct_size 零/错误值、JIT dispatcher 1,000,000 step 预算映射 `STEP_LIMIT=8`（不是 INTERNAL）并恢复。独立 QA 见 §8。

**QA 发现并修复的真实缺陷（c5adf4f）**：独立 QA-artifact 通过 C ABI 调用 `AevDllCompute(1,2,2000000)` 时，JIT dispatcher 达到 1,000,000 step 预算后错误返回 `INTERNAL(12)`，而不是契约的 `STEP_LIMIT(8)`。根因是 JIT 循环只设置局部 `rc`，未写 `GuestState.error_code`；修复为设置 `AEV_ERR_STEP_LIMIT`/`error_pc` 后映射 status 8。新增 4 项 `p0_pe_session` 检查并在 QA-behavior/QA-artifact 独立复测中通过；随后正常 Compute 恢复。该缺陷只在超长循环（2,000,000 次）触发，GUI 常规输入（n≤17/100 次循环）未受影响，但已纳入最终测试。

## 8. 独立 QA/reviewer

- QA-behavior/Windows（`ses_e4c6748b9ffew03RnttZFc3UhD`）：**PASS（c18eeec 原始 + c5adf4f 最终复验；人工待验收）**，报告 `.local/results/P0-PE-GUI-01/qa-behavior/REPORT.md`（§12 为最终源码复验）。最终 ctest 7/7、`p0_pe_session` 233/233、PE DLL 149/149、语义 791、Trace/AOT 78；base 560/561、supplemented 596/597（重新生成 supp，selected 21/promoted 21/demoted 0，零 JIT/2534 AOT）；自己的 AppKit 新输入（Reset 0x0BADF00D、Compute 13579/2468/11、100 次不同输入）942/943 checks、0 failure；Windows stateful 106 行除 ASLR 外逐行一致，QA-only Windows 新序列 **1624/1624** 与 Mac/oracle 一致；旧六组 COFF GUI **264/264**；independent step-limit 复核 `STEP_LIMIT/8`+恢复；`libaev_pe_session.dylib` 进程内出现一次，无 runner/Unicorn；无阻断。
- QA-artifact/lifecycle（`ses_e4c6748b7ffeLPzoowFPhq4RcR`）：**PASS（无阻断）**，报告 `.local/results/P0-PE-GUI-01/qa-artifact/REPORT.md`。最终 c5adf4f 独立构建 ctest 7/7；独立 C ABI/lifecycle **455/455**（struct/offset、create/info、双 session、8 slot/第 9、stale/double/illegal、wrong-thread、observer 重入/清除、部分加载失败、missing/unloadable AOT、wrong base/ABI/COFF、append/同尺寸变异失效、权限/MEMORY、trace、step-limit、AOT hybrid）；provenance **359/359**（独立 GUI trace 生成 selected 19/promoted 4/demoted 1，新 AOT 零 JIT，4 个 promoted 块实际 `aot`，publisher 与伪造 base SHA/selected/promoted 负例）；observer C++ 异常 **17/17**；ASAN 辅助构建 C ABI 455/455、observer 17/17、无 ASAN 报告（`detect_leaks=0`，只证明已执行路径无 UAF）。它发现并促成 step-limit 真实缺陷修复；确认 trace 文件 reopen 后 `call=1/2`，而 `result.call_index` 为 session 累计 4/5。
- 最终 reviewer（`ses_e4c388f23ffe1VV6V0hVDaksY5`）：**无阻断，可进入外部复核**；只读审查最终源码 c5adf4f/文档，未亲自运行测试，引用 QA 最终报告。它确认 B1–B7 的实现覆盖、共享 core/薄 runner、GUI 进程内/双后端、trace→supplement→AOT、回归/证据；未发现 UAF/半 session 阻断。非阻断问题（下一轮处理）：
  1. `create` 未拒绝超过 511 字节的 `entry_export`（查询侧 NUL 截断；固定 fixture 不触发）。
  2. 部分 C ABI 入口先校验 NULL/尺寸再 `resolve`，极端情况下 wrong-thread/reentry 可能被 `INVALID_ARGUMENT/ABI_MISMATCH` 掩盖（正常有效参数测试不触发）。
  3. Python `pe_session.set_observer` 在 C ABI 失败时提前清空旧 callback 列表，可能导致旧 observer 悬空（GUI 业务路径未使用 observer；C ABI/QA C 测试不触发）。
  4. trace header `entry_exports` 按 `module.exports` 顺序写，而 supplement 要求与排序 manifest 精确相等；固定 fixture 恰好有序。
  5. runner 的 table-full/handle-lost 仍为 2/12 而非 LOAD=3（正常 `--reload` 不可达；契约措辞可再收紧）。
  6. HostImport 只校验 id=1，尚未拒绝 registry 声明 `args!=2`/不同 ABI 的固定实现。
  评审还提示：集成 `.local` GUI summary 原先绑定 c18eeec dirty 构建；本轮已在 HEAD `38cfaba`（源码 c5adf4f）重跑 base/supp，summary `git_head=38cfaba`、library `ecf6fcce…`，QA 最终报告绑定 c5adf4f。结论：可进入外部复核；人工 GUI/打包/性能仍未验收。

## 9. 未测/限制与风险

- 人工物理键盘/焦点/IME/窗口缩放/观感未验收；自动 AppKit 事件/截图不等于人工观感。
- 未实现/未验证：通用 EXE/进程启动、完整 Win32/消息泵、任意嵌套/线程/模态、TLS/CRT/SEH/COM、通用依赖图、真实第三方应用、运行中代码失效/重装、同一 artifact 跨 base、持久 JIT 缓存产品化、独立 app 分发/签名/公证、性能结论。
- `compat` 仍是 P0 语义 harness（自绘控件/命中测试），不是最终兼容运行库；D3 兼容运行库选择仍未决。本轮只证明 compat/native 两个后端与同一 PE session 的真实控件接线，不把 compat 外观/控件当作最终产品兼容层。
- base profile 使用 empty AOT（全 JIT）；这是有界验证，不是性能测量；未把 hybrid 22 AOT/3 JIT 写成 full-AOT 零 JIT（`p0_pe_dll` 已覆盖混合）。
- Windows reference 的 `cursor_addr` 是 Host 地址、ASLR 高 bits 可变；比较低 16 位/relocation 关系，不绑定绝对地址。
- `build.log` 为 GBK/CRLF 原样证据；`git diff --check` 可能把 CR 报为 trailing whitespace，属预期。
- QA-behavior 在 macOS `DYLD_PRINT_LIBRARIES` 日志中观察到系统加载 `/usr/lib/libRosetta.dylib`；该进程 `sysctl.proc_translated=0`，Python 与所有 Aevotrace 组件均为 arm64，没有 x86 翻译执行证据。按“系统库存在”记录，不作为 Rosetta 路径通过。
- base trace 的合法 ReadAt 覆盖调用是测试/覆盖设置，不是业务结果；负例仍通过真实按钮并独立记录。
- 未在磁盘满/写入中途失败、generation 2^32 回绕、并发非 owner destroy 与 owner get 的极端竞争下验证。
- reviewer 的 6 项非阻断（长 `entry_export` 拒绝、C ABI 错误优先级、Python observer 失败路径、trace `entry_exports` 排序、runner table-full/handle-lost 状态、HostImport registry args/ABI 校验）未在本轮修改，固定 fixture 不触发；列入外部复核/下一轮，不把它们当作已修。

## 10. 结束状态

停在 `p0/pe-gui-01` 集成分支，供外部复核；未合 main、未 push、未删除/归档工作树、未开启下一轮。P0 整体/首版仍未完成。
