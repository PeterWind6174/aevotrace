# P0 证据索引

日期：2026-11-05。基线：`373becb`（任务记录前）；任务记录提交：`586662a`。本索引只列可复核证据，不代替报告结论。

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

## P0-ENGINE-01（完成，QA 功能 PASS；F1–F5 修复集成）

- worktree：`.worktrees/p0-engine`，分支 `p0/engine-spike`，原始 spike `a4ef11c723c929a61030ef3e0ae8fc8fba537ab2`；F1–F5 修复后 HEAD `8c0fc15087221e408e84f4b7adf50fc67f2a9abf`
- 报告：`.worktrees/p0-engine/spikes/p0-engine/REPORT.md`、`README.md`、`ROUTE_B.md`、`THIRD_PARTY.md`
- 证据：`.worktrees/p0-engine/.local/results/P0-ENGINE-01/final/`；QA 复跑在 `.local/results/P0-QA-01/engine-results/`
- 输入 COFF `a12fa7ff...7b1e`；AOT dylib/runner 哈希随构建目录变化（install name），IR/object 可复现；QA 三方输出一致。
- V-1 加载时代码版本/失效未实现；P-1 `.app`/随包依赖未满足。
- 正确性修复：代码 `9f21649b2a0470d2d6dc4bea576785af22ac6e8c`；报告 `.worktrees/p0-core-fix/spikes/p0-engine/REPORT-CORE-FIX-01.md`；独立 QA `.local/results/P0-CORE-FIX-QA-01/REPORT.md`（F1–F5 PASS，123 PASS + 1 N/A）；集成到 `p0/engine-spike`。

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
