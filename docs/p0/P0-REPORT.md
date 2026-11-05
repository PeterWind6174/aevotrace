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
| P0-SCOPE-01 | 进行中 | | |
| P0-ABI-01 | 进行中 | | |
| P0-TRACE-01 | 进行中 | | |
| P0-ENGINE-01 | 进行中 | | |
| P0-UI-01 | 进行中 | | |
| P0-ADR-01 | 进行中 | `docs/adr/ADR-0001-p0-selection.md` 草案 | 决策待证据 |
| P0-REVIEW-01 | 未开始 | | |

## 3. Windows 参考机与双机流程（P0-ENV-01）

- Windows：`WindPC`，Windows 11 10.0.26340 x64；VS Community 2026 18.11.12328.437；MSVC 14.51.36231；`cl` 19.51.36260 x64；Windows SDK 10.0.26100.0。
- 实际命令：`vcvars64.bat` → `cl /nologo /O2 /W4 /EHsc p0_smoke.c` → 正常运行/错误参数/重复运行；退出码 0/0/2。
- 哈希：源码 `3abea8...b89ff`；exe `9e8243...f1a87`；输出 `bddfb5...be9db`。
- 回传：Windows→Mac 56 个文件逐个哈希一致，Mac `MANIFEST.sha256` 76/76 OK，`verify_mac.py` 15 项 PASS。
- 关键限制：SSH 默认 PowerShell 会塌缩非零退出码；必须显式 `cmd.exe /c ...; exit $LASTEXITCODE` 或读取每步 `.exit.txt`。
- GUI：SSH 落在 services/session 0；交互桌面是 console session 1（`PeterWind`），GUI 参考运行需用户在该会话执行。

## 4. 候选应用与支持矩阵（待 P0-SCOPE-01）

## 5. 执行核心与 AOT/JIT（待 P0-ENGINE-01）

## 6. 双 GUI 后端（待 P0-UI-01）

## 7. ABI、依赖与许可（待 P0-ABI-01）

## 8. 准备执行与记录（待 P0-TRACE-01）

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
