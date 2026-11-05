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

## P0-ABI-01（进行中）

- 报告：待 `.local/results/P0-ABI-01/`
- 依赖/许可证：待填

## P0-TRACE-01（调查完成，未验证）

- 报告：`.local/results/P0-TRACE-01/REPORT.md`，SHA-256 `7f840aab400e8f14878dffe8eae978bb13a6bc21eae13aab011ff1967a1b484e`
- CLI 日志：`.local/results/P0-TRACE-01/cli.log`（退出 0）
- 仅方案；无实际 Trace/样例/记录实现；D6 未决。

## P0-ENGINE-01（进行中）

- worktree：`.worktrees/p0-engine`，分支 `p0/engine-spike`
- 报告/日志：待工作树内 `.local/results/P0-ENGINE-01/` 或 `.local/results/P0-ENGINE-01/`
- 输入/产物哈希：待填

## P0-UI-01（进行中）

- worktree：`.worktrees/p0-ui`，分支 `p0/ui-spike`
- 报告/日志/截图：待工作树内或 `.local/results/P0-UI-01/`
- Windows 参考程序哈希：待填

## P0-ADR-01 / P0-REVIEW-01

- ADR 草案：`docs/adr/ADR-0001-p0-selection.md`
- 审查：待 reviewer 对 ADR 和证据的独立意见

## 平台探针（coordinator 自查）

- JIT/MAP_JIT 探针：ad-hoc 与 Hardened Runtime + `com.apple.security.cs.allow-jit` 均成功；Hardened Runtime 无 entitlement 时 errno 22。
- AOT dylib 探针：未签名 `dlopen` 失败，linker-signed 或 `codesign --sign -` 后成功。
- 截图/辅助功能：`screencapture` 成功；System Events 可读取 Finder 窗口元数据。
