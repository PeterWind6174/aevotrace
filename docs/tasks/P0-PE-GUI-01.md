# P0-PE-GUI-01：持久 PE session 与双 GUI 集成

状态：进行中（契约冻结候选，待实施/QA/审查）。
日期：2026-11-07。

## 输入与责任

- 阶段/需求：P0 高风险验证；R01/R04/R06/R08；P0-03/P0-04 的 PE 驱动 GUI 补充分项。
- 基线：`de67e2be2ea2252bb0e2b522ba52aa5d208e5d21`（`p0/pe-core-fix-01` / `p0/pe-core-fix-integration`）；main 仍 `c91a3f7`。
- 本轮集成分支/worktree：`p0/pe-gui-01` / `.worktrees/p0-pe-gui-01`。
- 契约：[PE-SESSION-CONTRACT-20261107](../p0/PE-SESSION-CONTRACT-20261107.md) v1；header `spikes/p0-engine/include/aev_pe_session.h`。
- 既有 ABI：Guest 2/296、callback 3、artifact/translator 2、trace sink 1、PE trace file 4；本轮不改变。
- owner：coordinator（契约/集成/文档）；compiler（core/ABI/runner/headless）；platform（GUI/双后端）；binary（模块身份会签、Windows stateful reference）；QA（behavior/Windows、artifact/lifecycle）；reviewer（只读最终版本）。
- 模型：所有实际会话必须核对 OpenCode Go / DeepSeek V4.1 Flash / Max；仅桌面端原生会话，不启动 CLI 模型 worker，worker 不递归派生。

## 并行隔离

| 任务 | 分支/worktree | 构建/缓存 | 文件 owner | 状态 |
| --- | --- | --- | --- | --- |
| contract/integration | `p0/pe-gui-01` / `.worktrees/p0-pe-gui-01` | `.local/build-pe-gui-int` | 契约/任务/报告/根文档 | 进行中 |
| compiler core | `p0/pe-session-01` / `.worktrees/p0-pe-session` | `.local/build-pe-session` | `include/aev_pe_session.h`（已冻结）、`src/pe_session_core.*`、`src/pe_session.cpp`、`src/pe_runner.cpp`、engine `CMakeLists.txt`、session headless scripts | 待派发 |
| platform GUI | `p0/pe-gui-platform-01` / `.worktrees/p0-pe-gui-platform` | `.local/build-pe-gui-platform`、venv 复用只读 | `spikes/p0-ui/harness/pe_session.py`、`pe_host.py`、`pe_scenario.py`、`run_pe_gui.py`、`scripts/run_pe_gui_test.sh`、`native_backend.py`（仅多 edit handle 修复） | 待派发 |
| binary Windows ref | `p0/pe-gui-binary-01` / `.worktrees/p0-pe-gui-binary` | Windows `C:/Users/PeterWind/AevotraceP0/P0-PE-GUI-01` | `testdata/pe/aevguest_ref.c`、`build_msvc.ps1`、`reference_stateful_stdout.txt`、`SHA256SUMS`、`build.log` | 待派发 |
| QA behavior/Windows | `p0/pe-gui-qa-behavior` / `.worktrees/p0-pe-gui-qa-behavior` | 独立最终构建/结果 | 只读最终源码 + `.local/results/P0-PE-GUI-01/qa-behavior` | 待最终提交 |
| QA artifact/lifecycle | `p0/pe-gui-qa-artifact` / `.worktrees/p0-pe-gui-qa-artifact` | 独立最终构建/结果 | 只读最终源码 + `.local/results/P0-PE-GUI-01/qa-artifact` | 待最终提交 |
| reviewer | 只读会话 | 无 | 只读最终提交 | 待最终提交 |

同一 ABI/CMake/dispatcher 单 owner；compiler 先冻结 core/header 实现，platform 不修改 engine 公共文件。QA/reviewer 不修改源码。

## 目标与范围

可观察结果：

1. 版本化 C ABI `libaev_pe_session.dylib`：create/load、export 查询/调用、身份/selected/计数、Trace 开关、owner 线程、reentry/stale/错误、destroy。
2. `aev-pe-runner` 与 GUI 共用同一 PE 装载/内存/AOT/JIT/HostImport/Trace 实现；runner 仅薄 CLI。
3. compat/native 各一个窗口持有一个持续 session；参数输入、Compute、Reset、结果/Guest State/Calls/错误显示；业务状态来自实际 DLL。
4. 同一固定 `aevguest.dll`（SHA `1f82b679...`）与 `aevhost.dll`（`0eabd013...`）；DLL 字节不变。
5. Windows stateful reference 使用同一 DLL，覆盖本轮 GUI 操作序列（含 100 次连续调用），不改变转换输入源码依赖。
6. 新 PE GUI 独立场景/oracle；不把旧 COFF GUI 测试改名。

允许修改：上述 owner 文件、`docs/p0/PE-SESSION-CONTRACT-20261107.md`、`docs/tasks/P0-PE-GUI-01.md`、本轮报告/证据索引、必要的 `docs/p0/EVIDENCE-INDEX.md` 追加。

明确不包含：通用 EXE/Win32 窗口自动转换、完整消息泵、任意嵌套/线程/模态、TLS/CRT/SEH、COM、通用依赖图、真实第三方应用、运行中代码失效/重装、跨 base artifact 复用、独立 app 分发/公证、性能结论。边界外明确拒绝。

## 契约要点

见 `docs/p0/PE-SESSION-CONTRACT-20261107.md`。关键行为：

- handle 为 generation token；stale 返回 9；owner 线程固定；非 owner 13；活动 call 中 call/destroy/trace/observer 返回 10。
- `AevPeCallResult.rax_i32` 明确 32 位有符号结果；GUI 不以高位显示业务差异。
- `load_count` 每 session 成功 load 后为 1；100 次按钮不得增长。
- Trace open/close 幂等/截断/header/call index 语义固定；实际 PE trace ABI 4。
- observer 仅观察，回调异常被捕获；observer 内重入被拒绝。
- runner 既有 JSON/退出码/`--expect-error`/`--reload`/stale handle 保持；装载前失败仍 `status=3 LOAD`。

## 验收

### 自动/headless

- 新 ctest `p0_pe_session`：C ABI create/info/counts/export/selected/block；同一 session Reset+3 组 Compute（含独立输入）对照 Python oracle；100 次连续 call；两个 session 隔离；destroy/stale/double-destroy；wrong-thread；observer 重入；部分加载失败；AOT/base 错配/未知 import/registry 错配；权限故障后恢复；trace open/close/重复 open。
- 原 6 ctest、`p0_pe_dll` 147、语义 791、Trace/AOT 78、provenance 与六组 COFF GUI 44/44 保持；必要时列等价断言，不删弱失败/权限/CF 测试。
- 新 PE GUI 场景：compat/native 各跑三组以上输入 + 100 次按钮 + 负例；实际控件文本、DLL State/Calls、session counts；截图与对象类/主线程证明。
- base profile 使用真实 JIT；trace 离线 supplement 后新进程/新 session 零 JIT、promoted 块 AOT、结果一致。
- 无每次按钮启动 runner；GUI 进程内加载 `libaev_pe_session.dylib`；无 Unicorn/Rosetta/Host 业务替代。

### Windows

- `ssh win` 重建 reference EXE（DLL 不变），生成 `reference_stateful_stdout.txt`，覆盖 Reset + 三组以上 Compute + 100 次连续 compute 的 result/state/calls/helper 序列；Mac 同序列逐项一致。
- 记录 Windows 工具版本、命令、退出码、DLL/EXE SHA；不把 SSH 未看到 GUI 当作 GUI 失败。

### 独立 QA

- QA-behavior/Windows：最终提交 + 独立新构建；用自己新输入/序列检查真实 DLL 状态与真实控件，不只信实施者 JSON。
- QA-artifact/lifecycle：最终提交 + 独立新构建；C ABI 负例、stale/thread/reentry/隔离、AOT/JIT trace/artifact 身份、无静默回退、依赖/反替代。
- reviewer：只读最终版本，检查契约覆盖、ABI/兼容、回归与证据；不代替 QA 运行测试。

### 人工

- 物理键鼠/焦点/IME/观感仍待用户；自动截图/事件与主线程对象类只证明自动部分。需要用户操作时给版本、步骤、预期结果。

## 完成报告

- 提交/diff、分支/worktree、实际模型会话记录。
- 命令、退出码、环境、配置、构建目录。
- DLL/helper/库/AOT/Trace/provenance SHA；base/new/promoted/demoted 块表。
- 持久 session/100 次/load_count/状态序列证据；compat/native 结果/截图。
- Windows stateful 对照。
- 独立 QA/reviewer 结论与未测项。
- 停在本轮集成分支，供外部复核；不自行合 main/push/清理工作树/开启下一轮。
