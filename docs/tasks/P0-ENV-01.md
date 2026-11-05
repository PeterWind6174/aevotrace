# P0-ENV-01：双机工具链与结果回传 smoke

状态：完成（coordinator 复核；仅限本任务范围）。

## 输入与责任

- 阶段、需求 ID、前置任务：P0；R01/R02/R05；无前置。
- owner / reviewer / QA：qa / reviewer（按需）/ qa 自测后由 coordinator 复核。
- 基线 commit 或源码包哈希：`373becbdd7a65cafd63a98cb526bb1a9c933f731`。
- 分支 / worktree / Windows 工作目录：不改产品源码；Windows 使用 `C:\Users\PeterWind\AevotraceP0\P0-ENV-01`。
- 公共 ABI / artifact / 平台接口版本：不涉及，仅验证工具链和传输。

## 目标与范围

- 可观察的完成结果：在 Windows 通过 SSH 初始化 VS x64 开发环境，编译并运行一个 x64 C smoke 程序；把源码、二进制、stdout/stderr、退出码、文件副作用和 SHA-256 回传到 Mac；在 Mac 复核传输与哈希。
- 允许修改的文件或组件：仅 `.local/results/P0-ENV-01/` 与 Windows 任务目录；不修改仓库产品源码、不提交构建产物。
- 明确不包含的内容：GUI 窗口、Wine/兼容层、AOT/JIT、安装新工具链、修改系统 PATH。
- 需要的依赖及许可记录：仅使用机器现有 MSVC/Windows SDK、Git、Python；不引入第三方运行库。

## 验收

- 测试程序、输入、准备集和独立验证集：`p0_smoke.c`，包含整数/分支/字符串输出、文件写入、命令行参数和确定性退出码；Mac 侧保留源码哈希。
- Windows 参考执行步骤：`vcvars64.bat` 后 `cl /nologo /O2 /W4 /EHsc p0_smoke.c`；运行两次（正常参数、错误参数），记录退出码和输出文件。
- macOS 目标执行步骤：`scp` 取回 exe、stdout/stderr、结果文件；`shasum -a 256` 对比；`file` 确认 PE32+ x86-64。
- 预期业务输出、状态或副作用：stdout 含固定字符串和计算结果；输出文件内容固定；正常退出码 0，错误参数退出码非 0 且可区分。
- GUI 自动/人工部分：无 GUI。
- 必须证明真实生效的路径：Windows 原生 x64 编译与运行；SSH 非交互会话的退出码和文件回传。

## 完成报告

- 提交或 diff：无产品源码变更；报告与原始日志在 `.local/results/P0-ENV-01/`。
- 命令、退出码、环境及配置：记录 `cl`/`link`/SDK/CMake/Ninja 版本、`vcvars64.bat` 路径、完整命令和退出码。
- 二进制哈希、日志和报告位置：exe/source/output 的 SHA-256；原始日志文件名。
- 正确性/性能结果：只报告功能结果，不做性能结论。
- 审查问题及修复：
- 未测项、已知限制、需要用户操作的具体步骤：
- 后续任务与接口变更：

## 完成报告（2026-11-05）

- 提交或 diff：无产品源码变更；报告与原始日志在 `.local/results/P0-ENV-01/`（被忽略）。
- 环境：Windows 主机 `WindPC`，Windows 11 10.0.26340 x64；VS Community 2026 18.11.12328.437；MSVC 14.51.36231；`cl` 19.51.36260 x64；Windows SDK 10.0.26100.0；VS 内置 CMake 4.3.1-msvc1、Ninja 1.13.2。Mac：macOS 27.0.1 arm64、OpenSSH 10.3p1、Python 3.15.0。
- 命令与退出码：`scp` 传输 0；`vcvars64.bat` 0；`cl /nologo /O2 /W4 /EHsc p0_smoke.c` 0；正常运行 0；重复运行 0；错误参数 2；`fc /b` 0。Windows 脚本内每步退出码保存在 `windows/logs/*.exit.txt`。
- 二进制/输入哈希：源码 `3abea8fafbb337a43c1c5d17b9fba0a02382d715b7d5b87728d88963b49b89ff`；exe `9e8243a778ddbe7ff799c8281174ce50a3a3e4bb2074b41b8ea3dfd3d8ff1a87`；输出 `bddfb510963ba7da4ba1d9798ffe2c5d7b66280938af9a3ef583fc22b80be9db`。
- 独立复核：Mac `shasum -a 256 -c MANIFEST.sha256` 76/76 OK；`verify_mac.py` 15 项 PASS；`file` 确认 PE32+ x86-64，PE 头 `machine=0x8664`、`optionalMagic=0x20b`；`cmp` 两次输出一致。
- 重要限制：默认 SSH shell 为 PowerShell 7.6.9，未包装时 `exit 5/7` 会被塌缩为本地 1；必须使用 `ssh win 'cmd.exe /c <script>; exit $LASTEXITCODE'` 或读取脚本写出的退出码文件。本地 `perl alarm` 可在约 5 秒终止超时命令（退出 142），SSH 本身无单命令超时参数。
- 未测：GUI、Wine/兼容层、AOT/JIT、DLL/回调、`compat`/`native`、签名/公证、包内动态依赖、性能；未运行 macOS 目标（该 exe 仅为 Windows 参考二进制）。
- 后续：P0 其他任务继续；Windows 自动化统一使用显式退出码包装。
