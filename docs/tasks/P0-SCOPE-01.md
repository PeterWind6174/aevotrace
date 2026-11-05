# P0-SCOPE-01：Win32 GUI 测试族与真实应用候选

状态：完成（静态调查与哈希/PE 复核；未在 Windows/macOS 运行候选应用）。

## 输入与责任

- 阶段、需求 ID、前置任务：P0；R02/R03/R04/R08；无前置。
- owner / reviewer / QA：binary（lead）+ platform（contributor） / reviewer / qa（后续按选定场景）。
- 基线 commit 或源码包哈希：`373becbdd7a65cafd63a98cb526bb1a9c933f731`。
- 分支 / worktree / Windows 工作目录：调查任务，不改产品源码；使用 `C:\Users\PeterWind\AevotraceP0\P0-SCOPE-01`；报告在 `.local/results/P0-SCOPE-01/`。
- 公共 ABI / artifact / 平台接口版本：不冻结；只提出候选和缺口。

## 目标与范围

- 可观察的完成结果：
  1. 固定一组首版自编 Win32 GUI 测试程序的需求清单（窗口/菜单、按钮/编辑、文件对话框、模态/线程、Unicode、DLL 回调、错误路径、重复启动）。
  2. 至少两个真实应用候选，固定版本、官方来源、许可证、x64 二进制/安装包 SHA-256、GUI 框架、导入 DLL/API 类别、运行库依赖和可自动化的主要工作流。
  3. 一个建议作为 `compat` 与 `native` 共同验收对象的候选，并说明理由和风险。
  4. 支持矩阵草案：明确首版覆盖、暂不覆盖、需要用户确认的能力。
- 允许修改的文件或组件：仅 `.local/results/P0-SCOPE-01/`；可从网络下载安装包/二进制到该目录做哈希和静态分析，但不提交、不再分发。
- 明确不包含的内容：不实现转换器、不承诺候选应用已可运行、不修改 Windows 机器上的应用安装、不把源码调查当成二进制转换证据。
- 需要的依赖及许可记录：`dumpbin`/`llvm-readobj`/`objdump`、Python `pefile`（如安装，记录版本与许可）；所有候选应用许可证单独列明。

## 验收

- 测试程序、输入、准备集和独立验证集：候选应用安装包与主 EXE/DLL 的文件名、大小、SHA-256、版本资源；静态导入表摘要；至少一个候选的实际启动/工作流步骤（可在 Windows 参考机由 QA 后续执行）。
- Windows 参考执行步骤：只做只读检查；如需下载，记录 URL、时间、文件哈希；不安装、不改注册表。
- macOS 目标执行步骤：不适用（本任务不运行目标应用）。
- 预期业务输出、状态或副作用：报告能让人按固定版本重现下载/哈希/静态分析；候选工作流可被 QA 转成测试步骤。
- GUI 自动/人工部分：本任务只定义场景，不宣称 GUI 已通过。
- 必须证明真实生效的路径：无（调查任务）；不得把调查结论写成已支持。

## 完成报告

- 提交或 diff：无产品源码变更；报告文件列表。
- 命令、退出码、环境及配置：下载 URL、`shasum -a 256`、静态分析命令与输出摘要。
- 二进制哈希、日志和报告位置：
- 正确性/性能结果：不适用。
- 审查问题及修复：
- 未测项、已知限制、需要用户操作的具体步骤：
- 后续任务与接口变更：建议的 P1 测试族、真实应用验收任务和需要 coordinator 冻结的接口。

## 完成报告（2026-11-05，静态调查）

- 提交或 diff：无产品源码变更；报告 `.local/results/P0-SCOPE-01/app-candidates.md`，证据在 `evidence/`，fixture 在 `fixtures/`。
- 实际下载/哈希（包 SHA-256 均与官方发布值匹配）：
  - 7-Zip 26.05 x64 `7z2605-x64.exe`：`fd99e86f57d8f429ef5ed045bf98315af962e2309b0f1ba87a147a44c71647d0`；`7zFM.exe` `e036ab157806354f877bc33f45c1e11ff8bf4d0116d17301034d250e16c1eaee`；`7z.dll` `78bf6b668d75801e3aa180b4ccb16e6d8395fba80dec8a065055bb6f8dfabb82`。
  - WinMerge 2.16.60 `winmerge-2.16.60-x64-exe.zip`：`be3f850ed2e450c474928ccb67d2998e812da90dea1240a46352714553342522`。
  - PuTTY 0.85 `putty-0.85-w64.zip`：`e0b36e5381ecc78dd06a291bab25fb9d95bc991d0d864cf2741a5bde82816668`。
  - Notepad++ 8.9.8.1 `npp.8.9.8.1.portable.x64.zip`：`beddf5548e75c97930d1575ba1c7d113e6e2b33d0a200310e734ca08e225f414`。
- 推荐：7-Zip File Manager 26.05（原生 Win32/公共控件；无 MFC/.NET/Chromium；主工作流离线可验证）。备选不按 WinMerge 排序：binary 的 PE/API 证据显示 WinMerge 2.16.60 有 762 个导入、静态 MFC/ATL、GDI+、COM/OLEACC、CFG/TLS 2 回调，复杂度最高；PuTTY 0.85（MIT，网络/加密/自绘终端）或 Notepad++ 8.9.8.1（GPL-3.0，Scintilla/D2D/插件）仅在其范围被纳入时作条件式备选。
- 首轮 7-Zip 工作流：打开 `fixtures/scope-fixture.7z` → 列出 4 文件/2 目录 → 解压 → 校验 4 个成员 SHA-256 → 退出；Shell/拖放/UNC/密码/多卷/`7zG.exe` IPC 单列。
- fixture：`b1f05255839f7ce6761338e129fc3ed99b0483c391f6efd54c5fc8e62ed82e68`；`7zz t` 自检通过，成员哈希见 `fixtures/MANIFEST.txt`。
- 独立复核：coordinator 复算 7-Zip 包/主模块哈希、`7zFM.exe` 292 个导入符号、TLS 目录（无）、fixture 哈希与解压；与报告一致。
- 未测/限制：未运行任何候选 GUI；无 Windows 启动/截图/进程树/退出码；COM/Shell/拖放、`7zG.exe`、注册表、DPI/IME、Authenticode 未验证；`native` 自绘内容边界未验证。
- 后续：QA 在一次性 Windows VM 执行 7-Zip 工作流并回传截图/进程树/输出哈希；用户确认许可证与 `native` 自绘差异容忍边界。
