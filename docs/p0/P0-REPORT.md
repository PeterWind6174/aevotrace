# P0 报告：范围、选型与高风险验证

状态：进行中（等待 SCOPE/ABI/TRACE/ENGINE/UI 证据；不得据此宣称 P0 通过或产品已实现）。
日期：2026-11-05。基线：`373becbdd7a65cafd63a98cb526bb1a9c933f731`；任务记录提交：`586662a4ff09465d7d3aa453fb44e5aa6e880c79`。

## 1. 基线与协作

- `main` 与 `origin/main` 在任务开始前均为 `373becb`；未重新初始化 Git、未创建远端。
- 远端：`https://github.com/PeterWind6174/aevotrace.git`（已存在）。
- P0 任务记录：`docs/tasks/P0-INDEX.md` 及各任务文档。
- 隔离 worktree：
  - `.worktrees/p0-engine`，分支 `p0/engine-spike`。
  - `.worktrees/p0-ui`，分支 `p0/ui-spike`。
  - `.worktrees/p0-trace`，分支 `p0/trace-investigation`（CLI worker）。
- 主工作树只由 coordinator 写文档；spike 代码未合并到 `main`。
- 未推送本地提交；如需公开同步由用户确认后执行。

## 2. 实际执行摘要

| 任务 | 状态 | 关键证据 | 失败/未测 |
| --- | --- | --- | --- |
| P0-ENV-01 | 完成 | Windows x64 编译/运行/回传，76/76 哈希一致，15 项 Mac 校验 PASS | GUI/AOT/JIT 不在范围 |
| P0-SCOPE-01 | 静态调查完成（待复核） | 4 个候选官方哈希匹配；7-Zip 26.05 推荐、WinMerge 2.16.60 备选；7-Zip fixture 可解压 | 未在 Windows 运行任何应用 |
| P0-ABI-01 | 调查完成 | x64 调用约定/影子空间/栈对齐实测；TEB 版本化、TLS 计数、PE/异常/内存序清单；许可表 | 运行时 ABI/内存序/异常未实测 |
| P0-TRACE-01 | 调查完成，未验证 | 三层分工、记录字段草案、准备/验证集隔离方案；CLI 退出 0 | 无实际 Trace/样例/记录实现；D6 未决 |
| P0-ENGINE-01 | 待审查（QA 进行中） | commit `a4ef11c`；AOT dylib/ASLR/JIT ORC/负例证据；5 用例匹配 Windows 参考 | 无 PE/重定位/异常/线程/TSO/失效；独立 QA 未完成 |
| P0-UI-01 | 待审查（QA 进行中） | commit `60ba7a6`；native/compat 各 25 checks、真实 AppKit/自绘、截图、负例 | Unicorn 临时桥接；与 ENGINE 未集成；人工可见窗口待验 |
| P0-QA-01 | 进行中 | CLI worker 独立复跑 | 待报告 |
| P0-ADR-01 | 进行中 | ADR 草案与 proposed 决策 | 待 QA/REVIEW |
| P0-REVIEW-01 | 未开始 | 预审已完成任务分解审查 | 未审最终证据 |

## 3. Windows 参考机与双机流程（P0-ENV-01）

- Windows：`WindPC`，Windows 11 10.0.26340 x64；VS Community 2026 18.11.12328.437；MSVC 14.51.36231；`cl` 19.51.36260 x64；Windows SDK 10.0.26100.0。
- 实际命令：`vcvars64.bat` → `cl /nologo /O2 /W4 /EHsc p0_smoke.c` → 正常运行/错误参数/重复运行；退出码 0/0/2。
- 哈希：源码 `3abea8...b89ff`；exe `9e8243...f1a87`；输出 `bddfb5...be9db`。
- 回传：Windows→Mac 56 个文件逐个哈希一致，Mac `MANIFEST.sha256` 76/76 OK，`verify_mac.py` 15 项 PASS。
- 关键限制：SSH 默认 PowerShell 会塌缩非零退出码；必须显式 `cmd.exe /c ...; exit $LASTEXITCODE` 或读取每步 `.exit.txt`。
- GUI：SSH 落在 services/session 0；交互桌面是 console session 1（`PeterWind`），GUI 参考运行需用户在该会话执行。

## 4. 候选应用与支持矩阵（P0-SCOPE-01）

- 固定候选与官方哈希：7-Zip 26.05（`fd99e86f...47d0`）、WinMerge 2.16.60（`be3f850e...5222`）、PuTTY 0.85（`e0b36e53...6668`）、Notepad++ 8.9.8.1（`beddf554...f414`）；四个包均与官方发布值匹配。
- 推荐首选：7-Zip File Manager 26.05，`7zFM.exe` + `7z.dll`；原生 Win32/公共控件，无 MFC/.NET/Chromium；主工作流离线可验证。备选不按 WinMerge 排序：binary 的 PE/API 证据显示 WinMerge 2.16.60 有 762 个导入、静态 MFC/ATL、GDI+、COM/OLEACC、CFG/TLS 2 回调，复杂度最高；PuTTY 0.85（MIT，网络/加密/自绘终端）或 Notepad++ 8.9.8.1（GPL-3.0，Scintilla/D2D/插件）仅在其范围被纳入时作条件式备选。
- 后续候选：PuTTY 0.85、Notepad++ 8.9.8.1；均需独立范围，不与 7-Zip 首版混合宣称。
- 7-Zip 首轮工作流：打开 `scope-fixture.7z`（SHA-256 `b1f05255...e68`）→ 列出 4 文件/2 目录 → 解压 → 校验成员 SHA-256 → 退出；Shell/拖放/UNC/密码/多卷/`7zG.exe` IPC 单列。
- PE/API 事实：7zFM 292 导入/10 DLL，无 TLS 回调、无 CFG、无 Authenticode；`7z.dll` 89 导入、含 AVX/SHA-512 代码路径；WinMerge 762 导入、TLS 2 回调、CFG 函数表；Notepad++/PuTTY TLS 目录存在但非空回调为 0。`tls_callbacks` 口径统一为“目录存在/非空回调数量”，不混用。
- 未验证：没有运行任何候选 GUI；COM/Shell/拖放、`7zG.exe`、注册表、DPI/IME、Authenticode、`native` 自绘边界均未测。报告见 `.local/results/P0-SCOPE-01/app-candidates.md` 与 `binary-report.md`。

## 4.1 需求覆盖映射（防止把调查当成能力）

| 需求 | P0 相关任务 | 当前证据 | 状态 |
| --- | --- | --- | --- |
| R01 Win x64→macOS ARM64 | ENV + ENGINE | Windows x64 编译/运行；待 ARM64 产物 | 部分 |
| R02 EXE/DLL/资源输入 | SCOPE + ABI + ENGINE | 待 PE 分析/spike | 待证据 |
| R03 可选符号/元数据 | SCOPE | 未验证 | 未验证 |
| R04 普通桌面 GUI | UI | 待双后端 spike | 待证据 |
| R05 独立启动应用 | 未覆盖 | 未做 `.app`/签名/无开发路径 | 未验证 |
| R06 AOT 优先/JIT 回退 | ENGINE | 待 AOT/JIT spike | 待证据 |
| R07 性能优化 | 未覆盖 | 不做 P0 性能结论 | 未验证 |
| R08 compat/native 双输出 | UI | 待双后端 spike | 待证据 |
| R09 排除驱动/内核 | 范围 | 未做系统级测试 | 范围外 |

## 5. 执行核心与 AOT/JIT（P0-ENGINE-01）

- 分支/提交：`p0/engine-spike`，提交 `a4ef11c723c929a61030ef3e0ae8fc8fba537ab2`（`spikes/p0-engine/`，28 文件）。
- 输入：MSVC x64 COFF `testdata/guest_x64.obj`，SHA-256 `a12fa7fff6087e2e0ae95a09e395cd41874f5871674a4feda0cbff8ca9da7b1e`；`.text` 函数 `guest_kernel` 128 B / `guest_op` 9 B；含循环、分支、数组读写、间接调用、`imul`。
- AOT：Capstone 解码 → 自建 GuestState/helper ABI → LLVM IR → ARM64 Mach-O object → `libguest_aot.dylib`（ARM64，linker ad-hoc 签名）→ 独立 `aev-runner` 进程 `dlopen`/`dlsym`；`otool -tv` 显示真实 ARM64 块代码；`nm` 显示 `aev_blk_*` 符号。
- JIT：`imul` 块故意不在 AOT 表；LLVM ORC LLJIT 运行时编译 `aev_jit_140001042`，`--dump-jit` 反汇编出 `mul w20,w8,w9` 等 ARM64 指令，执行后设置 RIP=0x140001047 返回 AOT。
- 结果：5 个用例（prep_a/prep_b/hidden/unaligned/crosspage）与 Windows 参考 stdout 逐字节一致（`reference.diff` 为空）；统计 `aot_blocks=18–38`、`jit_compiles=1`、`jit_execs=2–4`、`error=0`。
- 独立进程/ASLR：两次 runner 加载 dylib 的基址 `0x104c50000` / `0x102e48000`（不同），`aev_aot_run` host 地址不同。
- 负例：`--bad-op` 未知间接目标显式失败（rc=3，`JIT ERROR: unresolved guest indirect target ...`）；`lock xadd`/`cdq` 翻译显式失败（rc=1，带地址/助记符/字节），无假返回值。
- 未测/限制：无 PE 装载/重定位/TLS/异常/线程/自修改/失效；无 Guest 页表/权限/跨页 fault；无 TSO/原子（原子直接拒绝）；无 MAP_JIT/Hardened Runtime/分发签名路径；AOT artifact 为 spike 格式，未冻结；`imul` 是故意 AOT 边界，不是能力上限。报告：`.worktrees/p0-engine/spikes/p0-engine/REPORT.md`。

## 6. 双 GUI 后端（P0-UI-01）

- 分支/提交：`p0/ui-spike`，提交 `60ba7a61acbb332a2840f46e7def744c7f71cdf2`（`spikes/p0-ui/`，42 文件；未提交 `.venv`/大 artifacts）。
- Guest 输入：Windows MSVC x64 `p0_gui_test.dll`，SHA-256 `a7934acee5520ac5c18ad1a4306682bba3b5c2263755bce30d261ee3da132602`；Windows 与 macOS 使用同一二进制哈希；导出 `P0WndProc`/`P0GetState`/`P0Reset`/`P0CreateMainWindow`/`P0RunGui`/`P0ProbeUnsupported`/`P0VersionW`。Guest 实际执行 main 15,933 条、worker 15 条 x64 指令；`SetWindowTextW` 14 次、`SendMessageW` 2 次、`MessageBoxW` 1 次、`CreateThread/PostMessageW/ExitThread` 各 1 次。
- `native`：真实 AppKit `NSWindow`/`NSTextField`/`NSButton`/`NSMenu`/`NSAlert`；`main_thread=True`、`creation_thread=MainThread`；截图 `01-initial-native.png`、`02-after-actions-native.png`、`03-modal-native.png`、`04-final-native.png`。25 项检查 PASS，含 clicks/edit/modal/worker/sync echo/`modal_screenshot_saved`/negative unsupported API/`native_no_compat_events`。
- `compat`：独立自绘 Windows 风格窗口/菜单/控件（`CompatWindow/CompatStatic/CompatEdit/CompatButton`），截图 `01-initial-compat.png` 等；同样 25 项检查 PASS，含 `compat_no_native_events`；未静默回退 native。
- 业务状态由 Guest 导出函数读取；编辑/按钮/菜单/模态/工作线程均驱动 Guest WndProc；输入文本/按钮序列变异改变 Guest 计算的长度与 FNV 哈希；`negative_unsupported_api` 证明未支持 `MessageBeep` 明确失败，未进入 compat。
- 关键限制：Guest 执行使用 Unicorn 2.1.4 临时桥接（GPLv2，仅 P0，不是产品核心）；message dispatcher 由 Host 拥有，`GetMessage/DispatchMessage` 仅 Windows 参考运行覆盖；Guest 对自身窗口的同步 `SendMessage` 在嵌套 Unicorn 中会崩溃，P0 明确拒绝，产品需要独立消息泵契约；无 TLS/SEH/heap/真实 PE loader；`compat` 是 P0 语义 harness，不是最终兼容运行库，D3 未决；未与 ENGINE AOT/JIT 核心集成。
- 结论：双后端 GUI 语义/回调路线有真实运行证据；不能据此宣称产品 `compat`/`native` 完成，也不能把临时桥接当执行核心。
- 报告：`.worktrees/p0-ui/spikes/p0-ui/REPORT.md`；Windows 参考状态 `evidence/state_utf8.txt`（可见窗口仍待人工验证）。

## 7. ABI、依赖与许可（P0-ABI-01）

- 已交叉编译/反汇编验证：RCX/RDX/R8/R9、XMM0–3、32 字节 shadow、第 5/6 参数 `[rsp+0x28/0x30]`、调用点 16 字节对齐；非叶函数 `.pdata/.xdata`；`GS:0x58` TLS 路径；`DefWindowProcW` 是 `NTDLL.NtdllDefWindowProc_W` forwarder（`ssh win` `dumpbin`）。
- GuestState 至少需要 GPR/RIP/RFLAGS/FS_BASE/GS_BASE/XMM0–15/MXCSR/x87/TEB/LastError/线程栈；TEB/PEB 必须版本化，不能把完整偏移当稳定 ABI；`GS:0x68` 未实测。
- `SendMessage` 同线程直接重入、跨线程阻塞并泵消息；`native` 不能异步化后仍宣称兼容。Guest SEH/C++ EH 不能穿过 Host 帧；7-Zip 有 212 个 `__CxxFrameHandler` 函数，首版 C++ EH 级别必须显式决定。
- PE 需覆盖 forwarder/API set/delay import/TLS 回调/异常目录/load config/CFG/资源；`RELOCS_STRIPPED` + 基址冲突必须诊断。
- 内存序/原子/未对齐/跨页/权限/映射代次全部未实测；P1 必须先声明 TSO 保证级别。
- 依赖许可：LLVM Apache-2.0 WITH LLVM-exception；Capstone BSD-3-Clause；Remill Apache-2.0（未构建）；QEMU GPLv2；FEX/Box64 MIT；Wine LGPL-2.1+；Unicorn GPLv2（仅 P0）。未集成、未做法律结论。
- 报告：`.local/results/P0-ABI-01/binary-abi.md`。

## 8. 准备执行与记录（P0-TRACE-01）

- 结论：Windows 原生执行只能作行为 oracle；SSH 不是 Trace 通道；ETW/Intel PT/DynamoRIO/Frida/Pin/QEMU TCG 均不能直接产出 Aevotrace Guest 级记录；权威采集应由目标端自建执行核心日志完成。
- 报告给出记录字段草案：输入/模块哈希、Guest PC/基本块/间接目标/代码版本、`guest_va_preferred`/`guest_va_runtime`/`rva` 区分、映射代次、字节依赖、失效规则；明确不持久化 Host 裸指针、不固化回放、不支持输入给诊断。
- 准备集/独立验证集按输入哈希、场景 ID、参数、构建版本、目录/manifest 五维隔离；验证集覆盖新输入重组、DLL/回调、代码版本、重定位/ASLR、失效、去符号、错误路径。
- 状态：仅调查/方案；**未实现、未运行 Trace**，无样例运行结果。报告 SHA-256 `7f840aab400e8f14878dffe8eae978bb13a6bc21eae13aab011ff1967a1b484e`。P0-05 的实际记录验证未完成，D6 未决。

## 9. macOS 平台约束（coordinator 自查）

- JIT：ad-hoc/linker-signed 与 Hardened Runtime + `com.apple.security.cs.allow-jit` 均可用；Hardened Runtime 无 entitlement 时 `mmap MAP_JIT` 失败（errno 22）。
- AOT dylib：未签名 `dlopen` 失败；linker-signed 或 `codesign --sign -` 后成功。
- Rosetta：Apple 官方支持页说明 macOS 27 及更早可用，macOS 28 起仅特定旧游戏保留有限功能；产品 Host 路径不得依赖 Rosetta。
- 截图/辅助功能：当前进程 `screencapture` 与 System Events 窗口读取已可用；自动化截图仍需按最终用户权限复核。

## 10. P0 结论（待填）

- 已成立：
- 未成立/需返工：
- 不得宣称：
- 选型建议：
- P1 下一批任务：

## 11. 命令与证据索引

- 见 `docs/p0/EVIDENCE-INDEX.md`。
