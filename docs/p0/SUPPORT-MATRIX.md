# P0 支持矩阵（证据版，P1 契约输入）

日期：2026-11-05。状态：P0 证据汇总，不是产品支持声明。来源：P0-ENGINE-01 `a4ef11c`、P0-UI-01 `847dad7`、P0-SCOPE-01、P0-ABI-01、P0-QA-01 `REPORT.md`/`RECHECK.md`。

## 1. 输入与执行

| 项 | P0 实测 | 未覆盖/风险 |
| --- | --- | --- |
| 输入格式 | MSVC x64 COFF `.text`；有界 PE32+ x64 DLL（映射/零填充/DIR64/普通命名导入导出/.pdata 代码发现）；另一个 UI DLL 仍为宏观映射 | 通用 EXE/资源/完整依赖图、TLS/delay import/API set/forwarder/ordinal import、load config/CFG、异常展开 |
| 输入身份 | COFF `a12fa7ff...`；UI DLL `c5b89e83...`；Windows/macOS 同 pinned 哈希 | 运行时输入/代码版本校验与失效（V-1）未实现 |
| x64 指令子集 | `mov/lea/push/pop/add/sub/and/or/xor/cmp/test/inc/dec/neg/jmp/jcc/call/ret/cmov/nop/endbr64/shl/shr/sar/cdqe`；`imul` 仅 JIT | `cdq/idiv`、`lock/xadd`、SSE/AVX/x87/string/syscall 拒绝；AVX/SHA 真实候选路径未执行；8/16 位移语义仅按共享核心现状 |
| AOT | COFF 与有界 PE DLL：Capstone 解码 → LLVM IR → ARM64 Mach-O dylib → 独立 runner；PE artifact 绑定 module/file/load/image/import/host/entry/thunk/all/selected；首选与非首选 base 各独立进程 | 同一 artifact 跨 base 复用、签名/分发、运行中代码失效 |
| JIT | ORC LLJIT 运行时编译未覆盖合法块/`imul`；PE hidden 块 JIT→Trace→supplement→新进程 AOT | 持久化 JIT 缓存；JIT-only 性能对照；未知间接目标按需建函数 |
| 内存 | COFF identity 兼容区；PE `AevMemRegion` R/W/X 检查、跨边界/未映射/只读写/非执行取指 fault 字段；未对齐/单区域跨界通过 | TSO/原子（原子拒绝）；页级 Host 保护；更大范围 PE/线程内存语义 |

## 2. Win32 / GUI 子集（P0 UI harness）

| 能力 | compat | native | 证据/限制 |
| --- | --- | --- | --- |
| 窗口/菜单 | 自绘 `CompatWindow`/菜单栏 | 真实 `NSWindow`/`NSMenu` | 25 checks；视觉/焦点待人工 |
| Static/Edit/Button | `CompatStatic/Edit/Button` 自绘 | `NSTextField`/`NSButton` | 25 checks；物理输入/IME 待人工 |
| 模态 | 自绘模态 | `NSAlert` | 截图 + `modal_result=1` |
| 工作线程 | Guest `CreateThread`→`PostMessage`→`ExitThread` | 同 | `1/1/1` |
| 同步回调 | Host-owned dispatcher → Guest WndProc → Host API | 同 | 35 次 Guest→Host；Guest 对自身窗口同步 `SendMessage` 被拒绝 |
| 未支持 API | `UnsupportedApi` 明确失败 | 同 | `MessageBeep` 负例；但 `DefWindowProcW/TranslateMessage/DispatchMessageW` 仍有占位返回，P1 需改 |
| 消息循环 | Host-owned；Guest `GetMessage/DispatchMessage` 仅 Windows 参考 | 同 | P0 架构限制 |
| 中文/Unicode | 是 | 是 | 文本/FNV 哈希 |

## 3. 真实应用

| 应用 | 角色 | P0 状态 | 首版边界 |
| --- | --- | --- | --- |
| 7-Zip 26.05 `7zFM.exe`+`7z.dll` | 主选 | 固定版本/哈希/PE/API 已调查；**未运行** | 打开→列出→解压→校验→退出；Shell/拖放/UNC/密码/多卷/`7zG.exe` IPC 单列 |
| WinMerge 2.16.60 | 不作首版备选 | 静态调查；762 导入/MFC/ATL/GDI+/COM/CFG | 复杂度高于 7-Zip；后置 |
| PuTTY 0.85 | 条件式后续 | 静态调查；MIT，但网络/加密/自绘终端 | 需网络/SSH 范围 |
| Notepad++ 8.9.8.1 | 条件式后续 | 静态调查；GPL-3.0，Scintilla/D2D/插件 | 需编辑器/插件范围 |

## 4. 需求映射

| 需求 | 状态 | 说明 |
| --- | --- | --- |
| R01 Win x64→macOS ARM64 | 部分 | Windows 参考 + ARM64 AOT/JIT spike；无完整应用 |
| R02 EXE/DLL/资源输入 | 部分 | 新增有界 PE32+ AMD64 DLL loader（映射/DIR64/普通命名导入导出/.pdata 代码发现）；仍无通用 EXE/资源/完整依赖图 |
| R03 符号/元数据 | 未验证 | P0 未做带符号/去符号双验证 |
| R04 桌面 GUI | 部分 | 双后端语义/回调/截图；非产品集成 |
| R05 独立启动应用 | 未满足 | 无 `.app`/随包依赖/签名/无开发工具运行 |
| R06 AOT 优先/JIT 回退 | 部分 | COFF 与有界 PE DLL 的 AOT 独立进程 + ORC JIT + Trace→supplement→新进程 AOT；无运行中失效/持久缓存 |
| R07 性能 | 未验证 | 仅原始混合路径；三类对照未测 |
| R08 compat/native 双输出 | 部分 | 双后端语义验证；D3 未决、未与核心集成 |
| R09 驱动/内核 | 范围外 | 未做系统级测试 |

## 5. 明确未覆盖

- 通用 EXE/进程启动、资源、完整依赖图；PE 的 TLS/delay import/API set/forwarder/ordinal import/资源/manifest/load config/CFG/异常展开，以及非 fixture 的 PE 变体。
- 本轮有界 PE DLL 只支持：PE32+ AMD64、DLL、无 DllMain/CRT/TLS、普通命名导入导出、DIR64 relocation、explicit registry；见 [PE-DLL-CONTRACT-20261106](PE-DLL-CONTRACT-20261106.md)。
- x64 异常/SEH/C++ EH、线程/TLS 回调顺序、信号、longjmp、并发自修改/失效。
- x86 TSO/原子/未对齐/跨页/权限 fault 的 Guest 语义。
- 真实 7-Zip 工作流、真实应用双模式验收、`.app`/签名/公证/无开发工具环境。
- 同核心 JIT-only、持久化缓存 JIT、AOT Hybrid 性能对照与阈值。
- Windows/macOS 可见 GUI、物理鼠标/键盘、输入法、焦点、外观。
