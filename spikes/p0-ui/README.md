# P0-UI-01: compat / native GUI callback spike

状态：spike 完成，边界明确。**不是产品实现，不能写成已支持 Win32 GUI 或已完成 D3。**

## 结论摘要

- Windows x64 测试 DLL 使用 MSVC 14.51 编译，`/Brepro` 可复现；固定 SHA-256
  `c5b89e83eae77db4b40710d991baaeb2d797cdf49310687492698c277bf55b14`。
  非交互 `/check`、`/auto`（含按钮、编辑框、模态、工作线程、同步 echo）通过；可见 GUI
  仍需人工在交互桌面确认。
- macOS arm64 harness 用 Unicorn 2.1.4 实际执行该 DLL 的 x64 机器码。**Unicorn 仅 P0
  临时执行桥接，不是产品 AOT/JIT 核心；没有用 Host 重写 Guest 业务函数。**
- `compat`：独立的自绘 Cocoa 后端（`CompatWindow/CompatStatic/CompatEdit/CompatButton`），
  模拟 Windows 风格窗口/菜单/控件；**不是最终 compat 运行库，D3 未决**。
- `native`：真实 `NSWindow`、`NSTextField`、`NSButton`、`NSMenu`、`NSAlert`，
  同步回调在 AppKit 主线程上进入 Guest 并返回；证明数据见 `evidence/native/summary.json`。
- 两个后端各 25 项自动检查通过；业务状态一致：`clicks=2`、`edit_changes=2`、
  `modal_count=1`、`modal_result=1`、`worker_posts_sent/received/exits=1/1/1`、
  `sync_echo=0x5A5A`、Guest 计算的 FNV 哈希
  `13377052239980446376`（文本 `第二次输入-λ-47`）。
- native 负例：Guest 调用未支持 `USER32!MessageBeep`，Host 明确抛
  `UnsupportedApi`，日志中 `compat` 事件数为 0；没有静默回退。

## 目录

```
guest/          Windows 测试 DLL / 驱动 / MSVC 构建脚本
harness/        Unicorn PE 执行桥接、Win32 API 子集、两种 GUI 后端、场景
scripts/        本地 venv 与后端运行脚本
evidence/       Windows 参考日志、双后端日志/JSON/截图、架构检查
artifacts/      本地构建输入 DLL（gitignored，不提交）
```

## 构建与运行

### 1. Windows x64 Guest

在 Windows 参考机（本任务目录
`C:\Users\PeterWind\AevotraceP0\P0-UI-01\guest`）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_msvc.ps1
.\p0_gui_driver.exe /check
.\p0_gui_driver.exe /auto
```

构建脚本通过 `vswhere` 找 VS 18 + MSVC 14.51，使用
`cl /LD /O2 /GS- /utf-8 /W4 /DUNICODE /D_UNICODE /DP0_GUI_EXPORTS ... /link /Brepro /NODEFAULTLIB
kernel32.lib user32.lib /ENTRY:DllMain`；driver 同样 `/Brepro`。脚本不使用
`Get-FileHash`，兼容 `cmd.exe /c` 调起 PS 5.1 的 `PSModulePath`。DLL 不依赖 CRT；导入表见
`evidence/dumpbin_imports.txt`。固定二进制哈希见 `REPORT.md`。

### 2. macOS harness

```sh
cd spikes/p0-ui
./scripts/setup_venv.sh
./scripts/run_compat.sh /path/to/p0_gui_test.dll evidence/compat
./scripts/run_native.sh /path/to/p0_gui_test.dll evidence/native
```

退出码 0 表示场景与证据完整；失败会写 `summary.json.error` 和 traceback。
必须使用与 Windows 参考相同的 DLL SHA-256（`summary.json.dll_sha256`）。

## 自动场景

1. `P0Reset` -> 创建窗口 -> `WM_CREATE`（Guest 在 x64 代码中创建控件/菜单）。
2. 输入 `macOS 验收输入-β-2026-11`，Guest `GetWindowTextW`/EN_CHANGE 更新状态。
3. 点击按钮：Guest `WM_COMMAND` 调 `SendMessageW(WM_GETTEXT)` 与
   `SetWindowTextW`，同步修改 Host 控件；第二次输入 `第二次输入-λ-47` 再点击。
4. Host 发送 `WM_P0_ECHO`，Guest 返回 `0x5A5A`。
5. `IDM_ABOUT` -> Guest `MessageBoxW` -> native `NSAlert` / compat 自绘模态；
   0.7s 后自动确认，日志/截图在模态 run loop 内完成。
6. `IDM_RUN_WORKER` -> Guest `CreateThread` -> 独立 Unicorn Guest 线程
   `Sleep(40)` -> `PostMessageW` -> `ExitThread`；主 Guest 收到
   `WM_P0_WORKER_DONE`。
7. `WM_CLOSE` -> Guest `DestroyWindow`；最后执行 negative probe
   (`MessageBeep`)，期望明确失败。

`evidence/*/run.log` 是顺序消息/API/Guest 调用轨迹；`events.jsonl` 是机器可读事件；
`summary.json` 含状态、消息序列、Guest 指令数/API 计数、后端证明和截图哈希。

## 已知限制（必须保留在报告中）

- **Unicorn 是 P0 临时桥接**：手工 PE 映射、IAT stub、无 TLS/SEH/heap/真实 loader；
  不能当作产品执行核心或性能数据。
- **Host-owned dispatcher**：macOS 上由 Host 调用 Guest `P0WndProc`；Guest 自己的
  `GetMessageW/DispatchMessageW` 循环只在 Windows 参考运行。嵌套
  `emu_start` 不安全，因此 Guest 对自身窗口的同步 `SendMessageW` 被明确拒绝；
  当前测试只让 Guest 同步调用控件 API。
- **compat 不是最终兼容运行库**：只是一个自绘 Cocoa 的语义 harness，覆盖窗口/菜单/
  按钮/编辑框/模态的最小集合。候选运行库（Wine/CrossOver 等）的目标架构、
  许可证和集成方式未定；D3 未决。本报告不声称可直接承载真实 Windows 应用。
  compat 当前为 arm64 原生代码路径，不依赖 Rosetta；这只描述本 spike。
- **native 不是通用 Win32→AppKit 映射**：仅覆盖 `CreateWindowExW` 的
  static/edit/button、`WM_COMMAND`、`Set/GetWindowTextW`、`SendMessageW` 控件子集、
  `MessageBoxW`；未知 API 抛错。
- GUI 自动化使用 `performClick` / 自绘 `activate` / 自定义菜单命中路径，不是物理
  鼠标事件；物理点击、焦点/输入法/外观仍需人工。
- 手工 PE 载入在 preferred base，未测试重定位；没有跨次映射失效实验。
- Windows `/auto` 在 SSH 隐藏 window station 中成功，但截图/可见窗口未验证；
  Windows GUI 参考标为 `待人工验证`。

## 人工验收步骤

Windows 交互桌面：

```powershell
C:\Users\PeterWind\AevotraceP0\P0-UI-01\guest\p0_gui_driver.exe
```

确认中文标题、按钮/编辑框/菜单、点击后状态变化、`关于` 模态、`启动工作线程`
后状态行 `线程发/收/退=1/1/1`，退出正常。截图/屏幕录制请回传。

macOS 交互桌面（与自动证据对照）：

```sh
cd spikes/p0-ui
./scripts/run_native.sh artifacts/p0_gui_test.dll evidence/native-manual
```

观察真实 AppKit 窗口与 NSAlert；也可运行 compat 观察自绘窗口差异。物理鼠标点击
和键盘输入未纳入自动断言，需人工确认并标记 `待人工验证`。

## 依赖与许可（P0 only）

- Unicorn 2.1.4（wheel 元数据 License: BSD，上游 README 标注 GPLv2；本 spike 不随产品分发）
- pefile 2024.8.26（MIT）
- PyObjC 13.0 / pyobjc-framework-Cocoa 13.0（MIT）
- Apple AppKit/Cocoa、MSVC/Windows SDK（系统/工具链）
- 记录见 `REPORT.md`；未把 Unicorn 写入产品依赖或能力声明。
