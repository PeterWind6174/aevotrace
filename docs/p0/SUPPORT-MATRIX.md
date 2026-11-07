# 支持矩阵（2026-11-07 证据版）

本表描述 main 中的有界实验，不是产品兼容性声明。最新产品源码 `47b2b7a`，集成 `ce9c11b`；当前状态见 [总入口](../CURRENT_STATUS.md)，合并和归档见 [收尾记录](INTEGRATION-20261107.md)。早期 2026-11-05 UI 报告保留作历史，不代表当前集成链仍只使用 Unicorn。

## 输入与执行

| 项目 | 已验证范围 | 未覆盖/风险 |
| --- | --- | --- |
| 输入 | MSVC x64 COFF；固定 PE32+ AMD64 DLL 的节映射/零填充、DIR64、命名导入导出、`.pdata` 代码发现 | 通用 EXE/资源/依赖图、CRT/DllMain/TLS/delay import/API set/forwarder/ordinal import、CFG/异常展开 |
| 执行核心 | Capstone → LLVM IR → ARM64 dylib；AOT 与 ORC JIT 共用状态/访存/helper；未知指令明确拒绝 | 有限指令子集，不是完整 x64；SSE/AVX/x87/string/syscall 等未形成支持集 |
| 指令回归 | 分支、整数、访存、间接调用、栈；F1–F5、SAR CF/节权限修复；语义 791 项 | `LOCK`/原子/TSO、异常、多线程等不因标量测试通过而视为支持 |
| AOT/JIT | AOT 独立加载、空 AOT 表真实回退、默认混合路径；支持的 IMUL 可由离线补充提升为 AOT | 持久 JIT 缓存、普遍覆盖率/性能结论 |
| Trace/补充 | 实际块级 AOT/JIT/HostImport 事件；PE trace ABI 4；真实 base/selected/all/identity 校验；新进程验证 | 非通用完整指令 Trace；不能证明未执行代码或任意动态目标 |
| PE 身份 | module/file/load base/image/import/host/entry/thunk/ABI/all/selected 绑定；首选与非首选 base 分别构建 | 同一 artifact 跨 base；运行中修改/解映射/地址复用失效 |
| 内存 | Guest VA/Host backing 分离；PE Region R/W/X；无权限节 prot=0；跨边界/未映射/只读写/非执行 fault 与恢复 | Host 页保护、TSO/原子、并发映射生命周期与任意 PE 变体 |
| 持久 session | 版本化 C ABI；8 slot/generation、owner/reentry、隔离、持续 DLL 状态、trace 生命周期；session 267 项 | 更复杂的嵌套/线程/应用生命周期 |
| observer/HostImport | 真实 wrapper 436 项；被拒绝的注册保留旧回调；固定 id/args/ABI 不匹配拒绝 | 真实 stale token 专项、registry 名称别名政策等见待办 |

## 双 GUI：区分三个入口

| 入口 | 执行路径 | 结论 |
| --- | --- | --- |
| 早期 `harness.run` / `guest_pe` | Unicorn 临时 PE 桥、有限 Windows API 语义 | 历史实验；不能用其占位 API 声明产品兼容能力 |
| `harness.run_engine` | COFF fixture → 共享 `libaev_bridge` → ARM64 AOT/真实 JIT → 双后端 | 六组场景各 44/44；不依赖 Unicorn；仍是 Host-owned GUI 调度 |
| `harness.run_pe_gui` | 固定 PE DLL → `libaev_pe_session` → 与 runner 共用的 core → 双后端 | base 560/561，supplemented 596/597；100 次按钮、load_count=1、状态来自 DLL |

`compat` 使用独立自绘 `CompatWindow/Static/Edit/Button` 语义实现，尚非最终兼容运行库（D3 未决）。`native` 使用真实 AppKit 控件和主线程，强制失败无 compat 回退。PE GUI 的窗口和操作流程由 Host harness 建立，尚未证明普通 Windows GUI EXE 的窗口创建、消息循环和完整回调链转换。

PE GUI 本轮 QA：base 为真实 JIT（compile/execute 21/2534、AOT=0）；补充后新进程 AOT=2534、JIT=0/0；selected/promoted=21、demoted=0。这里只是固定场景覆盖，不是性能测量。人工物理输入/IME/焦点/缩放/观感仍未验收。

## 真实应用与需求映射

7-Zip 为此前静态调查的主候选，WinMerge/PuTTY/Notepad++ 仅候选范围分析。没有真实第三方应用在 macOS 转换后完成双模式工作流的验收证据；候选版本/哈希见原 SCOPE 报告，不视为自动选定首版支持集。

| 需求 | 状态 |
| --- | --- |
| R01 Windows x64 → macOS ARM64 | 有界 COFF/PE DLL 实验通过，完整应用未满足 |
| R02 EXE/DLL/资源输入 | DLL 子集部分满足；通用 EXE/资源未满足 |
| R03 可选符号/元数据 | 尚未完成带符号/去符号对照 |
| R04 桌面 GUI | 两个实验后端与共享核心已贯通；Windows GUI 程序转换未满足 |
| R05 独立启动应用 | 未满足，无完整 `.app`/随包依赖/无开发工具验收 |
| R06 AOT 优先/JIT 回退 | 固定场景闭环已验证；运行中失效/持久缓存尚缺 |
| R07 性能 | 未验证，无三类正式对照与加速结论 |
| R08 compat/native 双输出 | 后端实验部分满足；产品运行库/双 `.app` 尚缺 |
| R09 驱动/内核 | 需求范围外 |

## 证据入口

- [PE DLL](P0-PE-DLL-01-REPORT-20261106.md)、[核心修复](P0-PE-CORE-FIX-01-REPORT-20261106.md)。
- [Trace/AOT](TRACE-AOT-REPORT-20261106.md)、[promotion 修复](PROMOTION-FIX-REPORT-20261106.md)。
- [持久 PE GUI](P0-PE-GUI-01-REPORT-20261107.md)、[session-fix](P0-PE-SESSION-FIX-01-REPORT-20261107.md)。
- [未完成项](../BACKLOG.md)、[完整证据索引](EVIDENCE-INDEX.md)。
