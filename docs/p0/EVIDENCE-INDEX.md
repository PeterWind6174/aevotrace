# P0 证据索引

日期：2026-11-05。基线：`373becb`（任务记录前）；任务记录提交：`586662a`。本索引只列可复核证据，不代替报告结论。

## main 合并与历史路径

2026-11-05 已将 ENGINE `b4052dd` 和 UI `847dad7` 合入 main，并移除全部 11 个已合并的附加工作树。当前源码与跟踪报告位于根目录 `spikes/p0-engine/`、`spikes/p0-ui/`。下文及旧报告中的 `.worktrees/...` 是历史执行位置，不再是现存工作目录。

工作树中的全部未跟踪/忽略文件均在移除前归档并校验，保存在本地 `.local/worktree-archives/20261105_195018/`；每份 `<worktree>.tar.gz` 内路径相对于原工作树根目录，`manifest.json` 记录原 HEAD、文件哈希及归档哈希。例如 UI 最终 QA 证据在 `p0-qa-ui2.tar.gz` 的 `spikes/p0-ui/qa-evidence2/`。根目录 `.local/results/` 的既有证据保持原样。详见 [合并记录](INTEGRATION-20261105.md)。

## P0-ENV-01（完成）

- 报告：`.local/results/P0-ENV-01/REPORT.md`
- 原始日志：`.local/results/P0-ENV-01/logs/`、`.local/results/P0-ENV-01/windows/logs/`
- 输入：`p0_smoke.c` SHA-256 `3abea8fafbb337a43c1c5d17b9fba0a02382d715b7d5b87728d88963b49b89ff`
- Windows exe：`9e8243a778ddbe7ff799c8281174ce50a3a3e4bb2074b41b8ea3dfd3d8ff1a87`（PE32+ x86-64）
- 复核：`MANIFEST.sha256` 76/76 OK；`verify_mac.py` 15 项 PASS；`file`/PE 头检查通过。
- 关键限制：SSH 默认 PowerShell 会塌缩退出码，需显式 `cmd.exe /c ...; exit $LASTEXITCODE`。

## P0-SCOPE-01（完成，静态调查）

- 报告：`.local/results/P0-SCOPE-01/app-candidates.md`
- 证据：`.local/results/P0-SCOPE-01/evidence/`、`fixtures/`
- 4 个包 SHA-256 均与官方发布值匹配；7-Zip `7zFM.exe` `e036ab15...c1eaee`、`7z.dll` `78bf6b66...dfabb82`。
- fixture `scope-fixture.7z`：`b1f05255839f7ce6761338e129fc3ed99b0483c391f6efd54c5fc8e62ed82e68`；`7zz t` 通过。
- 未运行任何候选 GUI；COM/Shell/拖放、`7zG.exe`、Authenticode 未验证。

## P0-ABI-01（完成，调查）

- 报告：`.local/results/P0-ABI-01/binary-abi.md`，SHA-256 `3e8ca3e89db6301a734f5cef76ed18896532acad7b5b28c7d6aa6e0d15d9bbb4`
- 证据：`.local/results/P0-ABI-01/spike/`、`binary-evidence/`
- x64 调用约定/影子空间/栈对齐交叉编译实测；TEB/PEB 版本化、TLS 计数、PE/异常/内存序清单；运行时 ABI/TSO/异常未实测。
- TLS 口径：7-Zip 无 TLS；Notepad++/PuTTY 目录存在但非空回调 0；WinMerge 2 个回调。

## P0-TRACE-01（调查完成，未验证）

- 报告：`.local/results/P0-TRACE-01/REPORT.md`，SHA-256 `7f840aab400e8f14878dffe8eae978bb13a6bc21eae13aab011ff1967a1b484e`
- CLI 日志：`.local/results/P0-TRACE-01/cli.log`（退出 0）
- 仅方案；无实际 Trace/样例/记录实现；D6 未决。

## P0-ENGINE-01（完成，QA 功能 PASS；F1–F5 + 零 AOT 修复集成）

- worktree：`.worktrees/p0-engine`，分支 `p0/engine-spike`，原始 spike `a4ef11c723c929a61030ef3e0ae8fc8fba537ab2`；F1–F5 后 `8c0fc15`；零 AOT/回归补强后 HEAD `b4052dd16c14ec3f529c78032dc9b0e6bd372fbc`
- 当前报告：`spikes/p0-engine/REPORT.md`、`README.md`、`ROUTE_B.md`、`THIRD_PARTY.md`（已合入 main）
- 证据：`.worktrees/p0-engine/.local/results/P0-ENGINE-01/final/`；QA 复跑在 `.local/results/P0-QA-01/engine-results/`
- 输入 COFF `a12fa7ff...7b1e`；AOT dylib/runner 哈希随构建目录变化（install name），IR/object 可复现；QA 三方输出一致。
- V-1 加载时代码版本/失效未实现；P-1 `.app`/随包依赖未满足。
- 正确性修复：代码 `9f21649b2a0470d2d6dc4bea576785af22ac6e8c`；报告 `.worktrees/p0-core-fix/spikes/p0-engine/REPORT-CORE-FIX-01.md`；独立 QA `.local/results/P0-CORE-FIX-QA-01/REPORT.md`（F1–F5 PASS，123 PASS + 1 N/A）；集成到 `p0/engine-spike`。
- 零 AOT/回归补强：代码 `7487e04ed2ff8cc393dba14d3a17ec931c9175af`，报告/分支头 `b4052dd16c14ec3f529c78032dc9b0e6bd372fbc`；当前实施报告 `spikes/p0-engine/REPORT-FINISH-01.md`；独立 QA `.local/results/P0-FINISH-QA-01/REPORT.md`（PASS，0 产品 FAIL；ctest 3/3、489 semantics）；reviewer `.local/results/P0-FINISH-REVIEW-01/REPORT.md`（无代码阻断）；先 fast-forward 到 `p0/engine-spike`，随后已合入 main。

## P0-UI-01（待人工验证；自动部分 QA 功能 + W-1/W-2 PASS）

- worktree：`.worktrees/p0-ui`，分支 `p0/ui-spike`；spike `60ba7a6`，修复 `847dad737a3b8b34138f185167e613e1a194fed3`
- 报告/证据：`spikes/p0-ui/REPORT.md`、`README.md`、`evidence/`；QA 证据 `.worktrees/p0-qa-ui2/spikes/p0-ui/qa-evidence2/`
- 固定 DLL `c5b89e83...`（`/Brepro`）；driver `ad6007d0...`；compat/native 各 25 checks；Windows `/check`/`/auto` 15/15。
- W-1/W-2 已关闭；旧/新 DLL 整体差 1165 字节（`.text` 相同）；Unicorn 仅 P0，D3 未决。
- Provenance：UI 提交内 `evidence/*/summary.json.git_head` 仍为基线 `60ba7a6`；聚焦 QA `qa-evidence2` 绑定修复提交 `847dad7`，最终以 QA recheck 为准。

## P0-ADR-01 / P0-QA-01 / P0-REVIEW-01

- ADR：`docs/adr/ADR-0001-p0-selection.md`（reviewer 已审；P0 不通过）
- 支持矩阵：`docs/p0/SUPPORT-MATRIX.md`
- QA 报告：`.local/results/P0-QA-01/REPORT.md`；聚焦复核 `.local/results/P0-QA-01/RECHECK.md`
- 审查 bundle：`.local/results/P0-REVIEW-01/`（engine.patch、ui.patch、EVIDENCE.md、REVIEW-TASK.md）
- P0 整体：未通过（P0-03/P0-05、V-1/P-1、真实应用/性能/人工项未闭合）

## 平台探针（coordinator 自查）

- JIT/MAP_JIT 探针：ad-hoc 与 Hardened Runtime + `com.apple.security.cs.allow-jit` 均成功；Hardened Runtime 无 entitlement 时 errno 22。
- AOT dylib 探针：未签名 `dlopen` 失败，linker-signed 或 `codesign --sign -` 后成功。
- 截图/辅助功能：`screencapture` 成功；System Events 可读取 Finder 窗口元数据。

## P0-PE-DLL-01（本轮有界任务 PASS；P0 整体未通过）

- 分支/工作树：`p0/pe-dll-01` / `.worktrees/p0-pe-dll`；代码 `f864d97aa1b3bd58c678744f17f1ffb4e254fedb`；集成分支 `p0/pe-dll-integration`；基线 `aed3b46`。
- 契约：`docs/p0/PE-DLL-CONTRACT-20261106.md` v3（`7d55333`）。
- 报告：`docs/p0/P0-PE-DLL-01-REPORT-20261106.md`。
- fixture：`spikes/p0-engine/testdata/pe/`；`aevguest.dll` `1f82b679…52b0`、`aevhost.dll` `0eabd013…57d1`、`ref` `0b30d8ea…7b6f`；`SHA256SUMS`。
- 原始证据：`.local/results/P0-PE-DLL-01/`（artifacts、windows、ctest、trace-aot、gui-summary、checks）。
- 独立 QA：`qa-final2/REPORT.md`（PASS，0 阻断；ctest 6/6、`p0_pe_dll` 110/110、`p0_core_fix_semantics` 501/0、六组 GUI 44/44、relocation/COFF jmp/shift 复测）、`qa-behavior/REPORT.md`（PASS，274/274）。
- 关键：首选/非首选 Guest base、DIR64/IAT、真实 JIT→Trace→supplement→新进程 AOT 零 JIT、100 次调用、reload 隔离、显式负例；原 6/6 ctest、78 Trace/AOT、六组 GUI 通过。
- 未测：PE 驱动 GUI、人工键鼠/焦点/IME、Windows 指令 Trace、打包/签名/性能；不得据此宣布 P0 或产品完成。

## P0-PE-CORE-FIX-01（本轮有界修复 PASS；P0 整体未通过）

- 分支/工作树：`p0/pe-core-fix-01` / `.worktrees/p0-pe-core-fix`；基线 `00ee8d3`，代码 `987692b`，最终测试/文档 `ac84ae3`。
- 外部审查：`.local/results/CODEX-PE-DLL-REVIEW-20261106/REPORT.md` F1/F2。
- 契约：`docs/p0/PE-DLL-CONTRACT-20261106.md` §12。
- 报告：`docs/p0/P0-PE-CORE-FIX-01-REPORT-20261106.md`。
- 独立 QA：`qa-perm/REPORT.md`（F1 PASS，343/343，Windows PAGE_NOACCESS）、`qa-sar/REPORT.md`（F2 PASS，312 矩阵/52 Windows 对照）、`qa-final/REPORT.md`（最终提交 6/6、PE 147、语义 791、Trace 78、GUI 44×6）。
- 结果：ctest 6/6、PE 147/147、语义 715（代码提交）/791（测试加强后）、Trace/AOT 78、六组 GUI 44/44、旧 PE AOT 可加载。
- 未测：PE 驱动 GUI、人工输入/IME、打包/签名/性能、真实应用、TLS/CRT/SEH/COM/线程。

## P0-PE-GUI-01（实现/集成与独立 QA 完成；reviewer 最终复审待补；P0 整体未通过）

- 分支/工作树：`p0/pe-gui-01` / `.worktrees/p0-pe-gui-01`；最终源码提交 `c5adf4fc26c3f360c9931d3833aa45d96eedcd7a`（`c18eeec` + step-limit 修复）；基线 `de67e2b`；main 仍 `c91a3f7`。
- 契约：`docs/p0/PE-SESSION-CONTRACT-20261107.md` v1 rev1；header `spikes/p0-engine/include/aev_pe_session.h`。
- 报告：`docs/p0/P0-PE-GUI-01-REPORT-20261107.md`；compiler 报告 `docs/p0/P0-PE-GUI-01-COMPILER-REPORT.md`。
- 实现：共享 `pe_session_core`、`libaev_pe_session.dylib`、薄 `aev-pe-runner`、compat/native `pe_host.py`/`pe_scenario.py`、Windows stateful reference。
- 构建：`.local/build-pe-gui-int`；ctest **7/7**（原 6 + `p0_pe_session`）；session **233/233**；PE **149/149**（147 核心 + 2 新 fixture hash）；语义 **791/0**；Trace/AOT **78/0**；旧六组 COFF GUI 各 **44/44**、4 截图（`.local/results/P0-PE-GUI-01/oldgui-final/`）。
- 新 PE GUI base（empty AOT）：compat **560/560**、native **561/561**；load_count=1、calls_total=323、host_imports=105、JIT 21/2534、AOT 0；supplemented：compat **596/596**、native **597/597**；load_count=1、JIT **0**、AOT 2534；跨后端状态一致；forced native failure 无 compat 回退。
- Trace/AOT：base GUI trace → `aot_supplement.py --input-kind pe` → selected 21/promoted 21/demoted 0/all 25；supp AOT `libpe_gui_supp.dylib` `0fc358a0…`；新进程 trace `aot=21`、JIT=0；`publish_aot.py` 接受。
- Windows：固定 `aevguest.dll` `1f82b679…`、`aevhost.dll` `0eabd013…`；stateful reference 106 行；Windows vs 独立 Oracle **106/106**；GUI vs Windows reset/4 Compute/repeat100 **6/6**（base/supp 各）。
- 原始证据：`.local/results/P0-PE-GUI-01/{gui-base,gui-supp,oldgui-final,artifacts,windows-stateful-compare.log,gui-vs-windows.log,models.json,evidence-manifest.json}`。
- 独立 QA：QA-behavior/Windows PASS（c18eeec 原始 + c5adf4f 复验，报告 `qa-behavior/REPORT.md`）；QA-artifact/lifecycle PASS（C ABI 455/455、provenance 359/359、observer 17/17、ASAN 已执行路径无报告；报告 `qa-artifact/REPORT.md`）。reviewer 最终复审待补。
- 未测：人工键鼠/焦点/IME/观感、性能、通用 EXE/Win32/TLS/CRT/SEH/COM/线程、真实应用、运行中失效、分发/公证。
