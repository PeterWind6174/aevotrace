# P0-UI-01 完成报告（spike）

日期：2026-11-05。分支：`p0/ui-spike`。基线：
`586662a4ff09465d7d3aa453fb44e5aa6e880c79`（worktree 创建时的 HEAD）。
本轮只修改 `.worktrees/p0-ui/spikes/p0-ui/`；未改主工作树，不 push。

## 1. 交付物

| 项 | 路径 |
| --- | --- |
| Windows Guest DLL 源码/头/驱动/定义 | `guest/p0_gui_test.c`, `p0_gui_test.h`, `p0_gui_driver.c`, `p0_gui_test.def` |
| MSVC 构建脚本 | `guest/build_msvc.ps1` |
| macOS harness | `harness/`（PE 载入/Unicorn、Win32 子集、compat/native 后端、场景、日志） |
| 本地构建/运行脚本 | `scripts/setup_venv.sh`, `run_compat.sh`, `run_native.sh` |
| 英文/中文说明 | `README.md` |
| 证据 | `evidence/`（Windows 参考、双后端日志、summary、截图、架构检查） |
| 输入 DLL | `artifacts/p0_gui_test.dll`（gitignored，不提交） |

## 2. 命令、退出码与环境

### Windows 参考

```text
ssh win 'cmd.exe /c "cd /d C:\Users\PeterWind\AevotraceP0\P0-UI-01\guest && powershell -NoProfile -ExecutionPolicy Bypass -File .\build_msvc.ps1"; exit $LASTEXITCODE'
# ssh/PS exit=0；脚本内部 BUILD OK，连续两次构建 DLL/driver 哈希一致
p0_gui_driver.exe /check   # EXIT=0
p0_gui_driver.exe /auto    # EXIT=0
```

W-1 修复：构建脚本不再调用 `Get-FileHash`（在 `cmd.exe /c` 继承 PS7
`PSModulePath` 时 PS 5.1 无法加载 `Microsoft.PowerShell.Utility`），改用
`[System.Security.Cryptography.SHA256]::Create()`，不依赖模块自动加载。
W-2 修复：DLL/driver 链接均加 `/Brepro`，同一源码重编译哈希稳定。

- MSVC：14.51.36260（VS 18 Community，x64）；`/O2 /GS- /utf-8 /W4`
- `/check`：无窗口，`LoadLibrary` + 导出检查 + `P0Reset/P0GetState`
- `/auto`：真实创建窗口，驱动编辑框/按钮/模态（后台线程关闭 MessageBox）/工作线程/
  `WM_P0_ECHO`；输出 `state_utf8.txt`（UTF-8，避免控制台编码丢失中文）

`/auto` 关键结果（`evidence/state_utf8.txt`）：

```text
clicks=1
editChanges=2
textLen=12
textHash=3175B48FE300F0E4
modalCount=1
modalResult=1
workerCreated=1
workerPostsSent=1
workerPostsReceived=1
workerExits=1
syncEcho=23130
wndProcCalls=42
resets=1
lastText=Windows 参考文本
status=点击=1  文本长度=12  模态=1  线程发/收/退=1/1/1
```

`AUTO OK: Windows reference state reached`；SSH 会话无可见桌面，**窗口外观/焦点/
输入法仍为待人工验证**。

### macOS

```text
cd spikes/p0-ui
. .venv/bin/activate
python3 -m harness.run --backend compat --dll artifacts/p0_gui_test.dll --out evidence/compat  # EXIT=0
python3 -m harness.run --backend native --dll artifacts/p0_gui_test.dll --out evidence/native  # EXIT=0
```

环境：macOS 27.0.1 arm64，Python 3.15.0（arm64），Unicorn 2.1.4，pefile 2024.8.26，
PyObjC 13.0。`screencapture -x -o -l<windowNumber>` 返回 0，PNG 已保存并 `file`
校验（`evidence/architecture.txt`）。

## 3. 组件与真实生效路径

### Guest 输入

- x64 PE32+ DLL，固定 SHA-256：
  `c5b89e83eae77db4b40710d991baaeb2d797cdf49310687492698c277bf55b14`
- driver SHA-256（同次 `/Brepro` 构建）：
  `ad6007d0b8f35a7361a3ad175769e57812244b0e5fa75d16f069f40e840742af`
- 编译命令由 `build_msvc.ps1` 记录；DLL 与 driver 的链接均加 `/Brepro`，同一源码连续两次构建哈希一致（`evidence/windows_build_repro.txt`）；导入表 26 个、导出 7 个（`evidence/dumpbin_*.txt`）。
- 业务逻辑全部在 Guest 机器码中：`P0WndProc`、`P0GetState`、`P0Reset`、
  `P0CreateMainWindow`、`P0RunGui`、`P0ProbeUnsupported`、`P0VersionW`。
- 控件/菜单/中文/Unicode、`WM_COMMAND`、`SendMessageW(WM_GETTEXT)`、
  `Get/SetWindowTextW`、`MessageBoxW`、`CreateThread`/`PostMessageW`/`ExitThread`。

### macOS 执行桥（**仅 P0**）

- `harness/guest_pe.py`：宏观 PE 载入到 preferred base、重定位未测试；IAT 槽指向
  Host stub；Unicorn 2.1.4 执行 x64。**不是产品 AOT/JIT 核心**，不用于性能结论。
- `harness/win32_host.py`：声明子集；未知 API 抛 `UnsupportedApi`，不返回假值。
- Guest 的 `P0RunGui` 消息循环只在 Windows 参考运行。macOS 使用
  **host-owned dispatcher**：Host 直接同步调用 Guest `P0WndProc`。嵌套
  `uc_emu_start` 经实验会崩溃，因此 Guest 对自身窗口的同步 `SendMessageW`
  被明确拒绝；当前只允许 Guest 同步调用控件 API。这是 P0 架构限制，产品需要
  单独的 Guest 消息泵/线程契约。
- 同一 DLL 哈希在 Windows 与 macOS 两侧一致；状态来自实际 x64 执行：
  main 15,933 条指令，worker 15 条；`SetWindowTextW` 14 次、
  `SendMessageW` 2 次、`MessageBoxW` 1 次、`MessageBeep` 1 次（负例）。
- 文本哈希由 Guest 的 FNV-1a 代码计算；Python 独立重算并断言
  `13377052239980446376`（`第二次输入-λ-47`）。输入文本/按钮序列变异后
  `clicks=2`、`text_hash` 改变，排除 Host 直接写状态。

### `compat` 后端（**不是最终运行库，D3 未决**）

- `CompatWindow`/`CompatRootView` 自绘标题栏、菜单栏、客户区；
  `CompatStatic`/`CompatEdit`/`CompatButton` 为自定义 NSView，命中测试和
  `mouseDown/mouseUp` 走自绘路径；自定义模态窗口。
- 目标：arm64 原生；本 spike **不依赖 Rosetta**。候选兼容实现（Wine/CrossOver 等）
  的目标架构、许可证、嵌入/打包方式没有选定；D3 未决。不能把本后端描述为
  可运行真实应用。
- proof：窗口 `CompatWindow`，控件 `CompatStatic/CompatEdit/CompatButton`；
  `native_classes_used=false`；`compat` 事件数 15，`native` 事件数 0。
- 未实现 `MessageBeep` 时同样明确失败（负例也跑在 compat，证明不是静默回退）。

### `native` 后端

- 真实 AppKit 对象：窗口 `NSWindow`，控件 `NSTextField`（static/edit）、
  `NSButton`，菜单 `NSMenu`（`main_menu_is_nsmenu=true`），模态 `NSAlert`。
- 创建线程与回调线程：日志记录 `NSThread.isMainThread=True`、
  `thread=MainThread`、`creation_thread=MainThread`；窗口/控件创建、按钮 action、
  菜单 action、编辑 delegate、模态均在主线程（`evidence/native/run.log`）。
- 同步链路：AppKit action -> `host.send_sync` -> Guest `WndProc`（x64） ->
  Guest `SetWindowTextW/SendMessageW` -> Host 同步更新控件 -> 返回 Guest ->
  AppKit action 返回。`guest_execution.sync_reentries_guest_to_host=35`，
  `max_host_guest_depth=1`（Host↔Guest 入口嵌套；每次 Host→Guest 期间发生 35 次
  Guest→Host 调用）。
- 无静默回退：native 运行 `compat` 事件数 0，`compat_classes_used=false`；
  负例 Guest 调 `MessageBeep`，Host 抛
  `unimplemented API USER32.dll!MessageBeep`，日志
  `scenario.negative_pass`，没有进入 compat。

## 4. 自动证据

两后端各 25 项检查全部 `true`（见 `evidence/*/summary.json.checks`）：

- 初始化 `resets=1`、`clicks=0`
- 输入 A、B 的 Guest 文本/长度/FNV 哈希
- 按钮 1/2 次及 Guest 更新标签
- `WM_P0_ECHO` 返回 `0x5A5A`、状态 `sync_echo=0x5A5A`
- 模态 `modal_count=1`、`modal_result=IDOK(1)`、模态截图存在
- worker `worker_created=1`、`posts_sent=1`、`posts_received=1`、`exits=1`
- 关闭 `window_closed`、native 无 compat 事件 / compat 无 native 事件
- negative probe 明确失败

消息序列（两后端一致）：

```text
WM_CREATE
WM_COMMAND (EN_CHANGE A)
WM_COMMAND (BN_CLICKED 1)
WM_COMMAND (EN_CHANGE B)
WM_COMMAND (BN_CLICKED 2)
WM_P0_ECHO
WM_COMMAND (IDM_ABOUT -> MessageBoxW)
WM_COMMAND (IDM_RUN_WORKER -> CreateThread)
WM_P0_WORKER_DONE
WM_CLOSE
```

## 5. 截图与日志

| 后端 | 截图 | 说明 |
| --- | --- | --- |
| native | `evidence/native/01-initial-native.png` | 真实 NSWindow/NSTextField/NSButton |
| native | `evidence/native/02-after-actions-native.png` | 两次输入/点击后 |
| native | `evidence/native/03-modal-native.png` | NSAlert 模态（Python 图标为宿主进程） |
| native | `evidence/native/04-final-native.png` | worker 完成后 |
| compat | `evidence/compat/01-initial-compat.png` | 自绘 Windows 风格 |
| compat | `evidence/compat/02-after-actions-compat.png` | 自绘控件状态同步 |
| compat | `evidence/compat/03-modal-compat.png` | 自绘模态 |
| compat | `evidence/compat/04-final-compat.png` | worker 完成后 |

`summary.json.screenshots` 记录路径、SHA-256、大小和 `file` 输出；
`evidence/architecture.txt` 记录 guest DLL 为 `PE32+ x86-64`，Python/Unicorn 为
`Mach-O arm64`，PyObjC 为 universal（arm64 slice 使用），截图尺寸/格式。
截图像素不是业务正确性唯一证据；业务状态以 Guest 导出、日志和指令计数为准。

## 6. 依赖、版本、许可

| 依赖 | 版本 | 许可 | 用途 |
| --- | --- | --- | --- |
| Unicorn | 2.1.4 | wheel 元数据 BSD；上游 README 标注 GPLv2（待法务确认） | **仅 P0** x64 执行桥，不随产品分发 |
| pefile | 2024.8.26 | MIT | P0 PE 解析 |
| PyObjC / pyobjc-framework-Cocoa | 13.0 / 13.0 | MIT | P0 AppKit 绑定 |
| AppKit/Cocoa | macOS 27.0.1 | 系统框架 | native 后端 |
| MSVC/Windows SDK | 14.51 / Windows 10.0.26340 | 工具链 | Windows 参考编译 |

依赖安装为 worktree 内 `.venv`（不提交）。Unicorn 的 pip 元数据与上游许可证
表述不一致，必须在产品化前解决；本 spike 不把它作为产品依赖或能力声明。

## 7. 未测项 / 需人工验证

1. Windows 可见窗口：在交互桌面运行 `p0_gui_driver.exe`，观察中文窗口、按钮、
   编辑框、菜单、NSAlert 等价模态、worker 状态；截图/录屏回传。状态：
   `待人工验证`。
2. macOS 物理鼠标点击、键盘输入法、焦点、窗口外观、菜单观感：自动场景使用
   `performClick`/自绘 `activate`/程序化菜单路径。状态：`待人工验证`。
3. 真机 AOT/JIT、代码版本/失效、重定位、TLS/SEH、真实应用、`.app` 签名打包：
   未纳入本 spike。
4. 失败路径只覆盖 `MessageBeep` 一个负例；未系统枚举所有未支持 API。
5. compat 最终运行库选型（D3）、目标架构、许可证与嵌入方式未定。
6. V-1/P-1、性能测量、产品级 AOT/JIT、人工 GUI 验收均不属于本 spike 范围。

## 8. 建议冻结的契约草案（供 coordinator）

- **Guest→Host API 子集**：`RegisterClassExW/CreateWindowExW`（仅
  static/edit/button+main）、`ShowWindow/DestroyWindow`、`SetMenu/AppendMenuW`、
  `Get/SetWindowTextW`、`SendMessageW`（控件消息）、`MessageBoxW`、`PostMessageW`、
  `CreateThread/ExitThread/Sleep/GetCurrentThreadId/GetModuleHandleW/GetLastError`。
- **消息语义**：Host 拥有主消息泵；Host→Guest 同步 `P0WndProc`；Guest 对自身窗口
  的同步 `SendMessage` 需要明确的产品级嵌套/消息泵规则，P0 不伪造。
- **后端能力对象**：`create_main_window/create_control/set_text/get_text/set_menu/
  click_button/click_menu/show_message_box/window_number/proof`；native 未支持能力
  抛错，禁止回退 compat。
- **状态/证据**：每次运行记录 DLL SHA-256、消息序列、Guest 指令/API 计数、
  WndProc 计数、后端对象类名、截图哈希；业务状态只从 Guest 导出读取。
- **线程契约**：AppKit 仅主线程；Guest worker 仅通过 `PostMessageW` 回主线程；
  Guest 线程 ID 与 Host 线程 ID 显式映射；不做 Host 原生回调直接进 Guest 的假设。
