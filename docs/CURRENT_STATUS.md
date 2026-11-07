# 当前状态（2026-11-07）

本批已完成实施、独立 QA、外部复核与 main 集成，正在同一收尾操作内完成最终主工作树验证和归档；执行结果以 [收尾记录](p0/INTEGRATION-20261107.md) 为准。下一阶段尚未启动。需求仍是 Windows x64 EXE/DLL → macOS ARM64 独立 GUI 应用，AOT 优先、真实 JIT 回退，保留 compat/native 两种输出。

## 当前代码基线

main 已快进至集成提交 `ce9c11b0282375aec79ca7fcf01f4bf1f0c097eb`，包含全部本地任务分支；最终产品源码为 `47b2b7a86f15ee67274fcf613a352e38791ec140`。之后的收尾提交更新文档与证据入口，不改变产品源码。后续从当时最新 main 创建新任务，不沿用归档前的工作树。

| 能力 | 已有证据 | 边界 |
| --- | --- | --- |
| x64 → ARM64 | Capstone → LLVM IR → ARM64 dylib；AOT 调度与 ORC 真实 JIT | 有限指令集，非通用 x64 兼容 |
| PE DLL | 固定 PE32+ AMD64 DLL；映射/零填充、DIR64、命名导入导出、`.pdata` 发现、权限检查、非首选 base | `/NOENTRY`、无 CRT/TLS/DllMain；非通用 EXE/依赖图 |
| 共享执行与回调 | CLI/PE session 共用核心；版本化 C ABI；owner/generation/reentry；真实 DLL 全局状态 | 有界 HostImport；本轮修复 observer 失败路径与 arity 校验 |
| 双 GUI | 一个窗口一个持久 session，100 次按钮保持 load_count=1；native 使用 AppKit；compat 自绘 | 窗口由 Host harness 建立，尚非 Guest EXE 的 Win32 窗口/消息循环转换 |
| Trace 与补充 | 实际块级事件 → 校验实际 base → 离线补充 → 新进程 AOT；promoted/demoted 按真实表计算 | 有限场景覆盖；不是通用指令 Trace、运行中失效或持久 JIT 缓存 |
| 产物身份与失败 | 文件/image/导入/Host 绑定/base/ABI/块表校验；权限故障与恢复 | 装载期拒绝失配不等于实现运行中代码版本失效 |

## 已复核的自动结果

- 最终源码外部独立构建：8/8 CTest；PE session 267/267、Python wrapper 436/436、PE DLL 149/149、x64 语义 791、Trace/AOT 78。
- 双 PE GUI QA：base compat/native 560/561；同次实际 Trace 补充后 596/597。每后端 100 次按钮、load_count=1；supplemented 各 2534 AOT 块、零 JIT compile/exec。
- 原六组 COFF GUI：每组 44/44，共 264；native 强制失败不静默转 compat。
- Windows 固定输入/reference 有哈希绑定；session-fix 仅改 Host 绑定与 wrapper，按任务约定复用参考，未重新远程执行。
- 本批收尾将在主工作树从空目录重新构建，并用 main 入口复跑双 GUI；具体命令、退出码和哈希见收尾记录。

计数仅代表固定样例和断言，不是完整兼容率或加速比。原始证据被忽略；跟踪报告见 [证据索引](p0/EVIDENCE-INDEX.md)，本地归档恢复方式见收尾记录。

## 未完成与阶段判断

P0 总体验收未关闭，P1 全量公共契约未冻结，首版交付未完成。尚缺通用 EXE 启动、完整依赖/资源与应用所需 CRT/TLS/SEH/线程能力、真实 Win32 GUI 机器码到双后端的窗口/消息/回调链、最终 compat 路线、独立 `.app`/随包依赖/签名、真实第三方应用双模式工作流、运行中失效/持久缓存和正式性能对照。

物理键盘、焦点、IME、窗口缩放和观感仍待人工；自动 AppKit 事件和截图不替代人工验收。未完成项已转入 [待办清单](BACKLOG.md)，不会因为本批任务收尾被标为完成。

早期报告、旧 task worktree 和 ADR 的 2026-11-05 评审结论是历史快照。当前入口使用本文件、支持矩阵和最新合并记录；不能据旧报告再次宣称“当前 GUI 仍只走 Unicorn”或“实际 Trace 尚未实现”。
