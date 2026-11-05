# P0-UI-INT-01：ENGINE 核心与双 GUI 后端的最小集成

状态：计划已更新，尚未派发；本轮共同 ABI → 执行桥 → 双 GUI 的第三阶段。

## 输入与责任

- 阶段/需求：P0 收尾；R04/R06/R08；前置 P0-CALLBACK-ABI-01、P0-CORE-BRIDGE-01。
- owner/reviewer/QA：platform（lead）+ compiler（contributor）/ reviewer / qa。
- 基线：main `02309da` 已含 ENGINE `b4052dd`、UI `847dad7`；实际派发时核对最新 main，再创建新 worktree。
- 契约：P0-CALLBACK-ABI-01 的最小试验契约；不要求先冻结完整 P1-CONTRACT-01，不得另建互不兼容的 ABI。
- 详细范围与验收：[下一轮计划](../plans/P0-CALLBACK-INTEGRATION-01.md)。

## 目标与范围

- 新增使用共同 ENGINE 执行桥的集成模式，至少跑通一条 Host→Guest→Host 回调 + compat/native 窗口/控件路径。集成路径不加载/执行 Unicorn，历史 Unicorn 对照路径可保留并明确区分。
- 证明 Guest x64 机器码由 ENGINE 实际执行，不是 Host 重写/固定回放；native 真实 AppKit，compat 独立路径，无静默回退。
- 记录 AOT 块/JIT 回退、调用顺序、业务状态与错误；先单线程主线程上一层回调，不要求本轮迁移全部线程/模态场景，未支持 API/重入明确失败。
- 不包含：完整 PE loader、全部 Win32 API、真实应用、`.app` 打包、性能结论。

## 验收

- Windows 参考与 Mac 目标用同一 Guest 二进制哈希；独立输入/按钮序列变异。
- 旧 DLL 若需要范围外 PE/指令支持，可使用声明过限制的新小型 x64 fixture，不得把 Host 重写业务当作集成；两个后端和 Windows 参考仍使用同一输入机器码。
- `native`/`compat` 各至少一条完整回调链；ENGINE trace 显示 AOT/JIT 实际执行。
- 至少三组输入/操作序列（含独立验证场景），同一实例连续至少 100 次调用；检查状态、错误与生命周期。
- 命令、退出码、哈希、日志、截图；人工观感待验收。

## 完成报告

- 提交/diff、命令/退出码、哈希、日志、未测项、接口变更。

## 完成报告（2026-11-05，C 阶段）

- 基线/工作树：main `c91a3f7`；A `96ff890`；B code `f023d78`；C 在本 worktree `p0/callback-integration-01` 顺序实现。
- 共用链：`harness/engine_bridge.py`（ctypes → `libaev_bridge.dylib`）→ `harness/engine_host.py`（窗口/输入框/按钮/结果区 + UTF-16 Guest 缓冲 + HostImport pending）→ `harness/engine_scenario.py`；`compat`/`native` 只负责控件后端。同一 COFF `4f6929f7...`、同一 AOT `1c1f2319...`、同一 bridge `3ab6d951...`、同一入口 `0x140001000`。
- 无 Unicorn：`run_engine.py` 不导入 `guest_pe`/`unicorn`/`pefile`；运行时 `sys.modules` 均为 false，`otool -L libaev_bridge.dylib` 无 Unicorn/pefile。旧 `harness.run` 仍保留为显式历史路径，后端 `UnsupportedApi` 已抽到 `errors.py`，legacy import 回归通过。
- 自动场景：三组序列（alpha `Aevoβ中` / beta `macOS-47` / 独立 `独立-Ω-93`），点击 op 分别 7/9/0x5A5A；两后端 `guest_rax` 分别为 `3828036654`/`614625053`/`651115151`，与 Windows `reference_stdout.txt` 同 fixture/op 一致。结果控件显示 Guest 计算的 FNV hash/len/op（Host 不计算最终 XOR）。
- 100 次连续回调：同一 bridge/实例 `loop_100_counter` PASS（+100），每击 `jit_compiles=0`、`jit_execs>0`；`total_jit_compiles=1`，`total_jit_execs=896`（含前面序列/负例；两后端相同）。AOT 每击 4 块；stderr 各有 452 条 `AOT rip=`、JIT compile/exec `0x140001020` 和 16 条 `JITCODE`。
- 负例：未知导入 `UNKNOWN_IMPORT`、非法内存 `MEMORY`、未支持重入 `REENTRY`（外层仍 OK）、helper 成功后 Guest 故障 `MEMORY` 且结果控件不更新；故障后正常序列恢复；强制 native 失败退出 1，`compat_events=0`、`native_events=5`、`compat_classes_used=False`。
- 证据：`gui-all/compat/summary.json` `ab307341...`，`native/summary.json` `24a84c43...`，`native-failure/summary.json` `ffc03c49...`；stderr `f73af958...`/`b238316c...`/`c5c7db38...`；两后端各 4 张 PNG（compat 1120x420，native 1120x484），`backend_proof` 均 `main_thread=True` 且无跨后端类/事件。截图自动证据不等于物理键鼠/焦点/输入法/人工观感验收。
- 自动检查：compat 40/40、native 40/40；`run_engine_gui.sh` 两后端 + 跨后端一致性 + forced native failure 全部通过。
- 未完成/边界：无通用 PE/Win32、TLS/SEH、线程/模态/任意重入、真实应用、Trace/失效、`.app`、性能；`compat` 仍是 P0 语义 harness；Windows 可见窗口/物理输入待人工。旧 Unicorn 对照路径未重新运行真实 DLL，只做 import 回归。
