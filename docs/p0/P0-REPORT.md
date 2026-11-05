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
| P0-ABI-01 | 进行中 | | |
| P0-TRACE-01 | 调查完成，未验证 | 三层分工、记录字段草案、准备/验证集隔离方案；CLI 退出 0 | 无实际 Trace/样例/记录实现；D6 未决 |
| P0-ENGINE-01 | 进行中 | worktree 已有 COFF 输入、translator/runner、隐藏/未对齐/跨页用例 | 待 AOT/JIT 运行证据 |
| P0-UI-01 | 进行中 | worktree 已有 Guest DLL/driver、compat/native harness、Windows 参考 auto.log | 待 Mac 双后端运行/截图 |
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
- 推荐首选：7-Zip File Manager 26.05，`7zFM.exe` + `7z.dll`；原生 Win32/公共控件，无 MFC/.NET/Chromium；主工作流离线可验证。备选：WinMerge 2.16.60（静态 MFC/ATL + GDI+ + COM，API 面更大）。
- 后续候选：PuTTY 0.85（MIT，但网络/加密/线程/自绘终端风险高）；Notepad++ 8.9.8.1（GPL-3.0，21 个直接 DLL、插件/更新器、Scintilla 自绘，风险高）。
- 7-Zip 首轮工作流：打开 `scope-fixture.7z`（SHA-256 `b1f05255...e68`）→ 列出 4 文件/2 目录 → 解压 → 校验成员 SHA-256 → 退出；Shell/拖放/UNC/密码/多卷/`7zG.exe` IPC 单列。
- 未验证：没有运行任何候选 GUI；COM/Shell/拖放、`7zG.exe`、注册表、DPI/IME、Authenticode、`native` 自绘边界均未测。报告见 `.local/results/P0-SCOPE-01/app-candidates.md`。

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

## 5. 执行核心与 AOT/JIT（待 P0-ENGINE-01）

## 6. 双 GUI 后端（待 P0-UI-01）

## 7. ABI、依赖与许可（待 P0-ABI-01）

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
