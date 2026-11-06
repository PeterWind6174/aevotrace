# P0-PE-DLL-01 任务记录：最小 PE32+ DLL 装载与共享核心执行

状态：实施完成，独立 QA PASS，reviewer ACCEPT WITH NON-BLOCKING NOTES；代码提交 `f864d97`，集成分支/文档 `c93c165`，待外部复核（报告：[P0-PE-DLL-01-REPORT-20261106](../p0/P0-PE-DLL-01-REPORT-20261106.md)）。基线：`p0/promotion-fix-01` / `p0/promotion-fix-integration`，HEAD `aed3b46435bff4347966be03b33cf7013d1aa2d8`。本轮分支 `p0/pe-dll-01`，工作树 `.worktrees/p0-pe-dll`。不改 main（仍 `c91a3f7`）。

## 输入与身份

- 用户交接：[P0-PE-DLL-01 提示词](../prompts/P0-PE-DLL-01.md)（本分支首个提交 `f3e8320`）。
- 外部复核：[CODEX-PROMOTION-REVIEW-20261106](../../.local/results/CODEX-PROMOTION-REVIEW-20261106/REPORT.md)，F1 已关闭。
- 契约：[PE-DLL-CONTRACT-20261106](../p0/PE-DLL-CONTRACT-20261106.md)（v3；callback ABI 3、GuestState 296/Guest ABI 2、trace file COFF 3/PE 4）。
- Windows：`ssh win`，VS 18 Community，vcvars64 位于 `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`；构建脚本显式调用。
- Mac：arm64 macOS，LLVM 23.1.4（Homebrew），CMake 4.4.4，Ninja 1.13.2。

## 调度与模型

- 主控会话 `ses_e510bd71cffdkpJjzuxMRrMM1G`：`opencode-go / deepseek-v4.1-flash / max`（从 `session_v2` 实际记录核对）。
- 本轮写代码采用**单实施者串行**：桌面端子会话无法由主控工具明确绑定独立 worktree，按 `docs/OpenCode弹性调度.md` 在不能保证隔离时串行实施。binary/compiler/platform 的会签以只读审查/契约评审会话完成；QA/reviewer 在冻结提交上独立验证。没有启动 CLI worker，没有递归派生。
- 契约 v3 会签（提交 `7d55333`）：binary `ses_e50c58852ffeMRvedwgW27tOTi`、compiler `ses_e50d40fcaffeCi9J97fQKq92dr`、platform `ses_e50c58850ffezlNChzcTDHUv5u`、reviewer `ses_e50c5884fffeWenm9He40TC5hl`；实际记录均为 `opencode-go / deepseek-v4.1-flash / max`（binary 会话从 `session_v2` 核对；其余会话头显示 provider/model，variant 由主控按 `session_v2` 复核）。
- 任何新增子会话必须记录其实际 `model`（provider/id/variant），不得假定继承。

## 文件 owner（本轮）

| 范围 | owner | 文件 |
| --- | --- | --- |
| 契约、任务、集成、公共构建 | coordinator | `docs/p0/PE-DLL-CONTRACT-20261106.md`、`CMakeLists.txt`、报告 |
| PE frontend/loader/模块身份 | binary | `spikes/p0-engine/src/pe.{hpp,cpp}`、`pe_loader.{hpp,cpp}`、`testdata/pe/*` |
| 内存模型/执行核心/artifact | compiler | `include/aev_guest.h`、`include/aev_guest_fields.def`、`src/aev_mem.c`、`src/translator.*`、`src/ir_emitter.cpp`、`src/aot_runtime.c`、`src/translate_main.cpp` |
| 导入 registry/runner/Windows 参考 | platform | `src/pe_runner.cpp`、`testdata/pe/aevhost.*`、`testdata/pe/*ref*`、Windows 脚本 |
| 测试与证据 | qa | `scripts/pe_*`、`.local/results/P0-PE-DLL-01/` |
| 只读审查 | reviewer | 无源码修改 |

同一 ABI/artifact/dispatcher/公共构建文件只由 coordinator 顺序修改；本轮没有并行写同一文件。

## 交付范围

1. 有界 PE32+ AMD64 DLL 解析/映射/零填充/命名导出/普通命名导入/IAT/DIR64 relocation/生命周期/诊断。
2. 受检查 Guest 内存区域模型（R/W/X、跨边界、未映射、只读写、非执行取指），Guest VA 与 Host 地址解耦；不用 `MAP_FIXED`。
3. 共享 AOT/JIT 核心：PE 导出实际进入 AOT；未覆盖合法块真实 JIT；Trace→离线 supplement→新进程 AOT 证明。
4. 固定自编无 CRT DLL + helper，Windows 系统 loader 与 Mac 共享核心跑同一 DLL；首选与非首选 Guest base。
5. 独立 QA 与 reviewer；保留原回归。

## 验收

- [x] 固定 DLL/helper/reference 版本、SHA-256、headers/sections/imports/exports/relocations 盘点；证明 fixture 实际使用全局数据、IAT、需修补指针。
- [x] 首选 base 与非首选 base 均成功；核对 relocation 修补值、导出地址、helper 参数、全局状态、结果；每配置新进程加载对应 AOT；基址错配拒绝。
- [x] 至少三组输入（含独立/空/溢出适用边界）；同一实例 100 次调用；重复 load/destroy 状态隔离。
- [x] 错架构/截断/越界目录、无效 export、未知 import、非法 relocation、基址/模块/ABI 错配、未映射/权限错误明确失败，且无故障后业务副作用，不破坏原 DLL/旧产物。
- [x] 实际 AOT→JIT→Trace→supplement→新进程 AOT：promoted/base/new 来自实际 artifact，零 JIT，块事件为 AOT。
- [x] 原 5 ctest、provenance 回归、六组 GUI 回归通过（ABI 升级只做等价断言替换）。
- [x] Windows 同 DLL 参考结果与 Mac 一致；原始日志/命令/退出码/哈希入证据目录。
- [x] 停在 `p0/pe-dll-01` 集成分支；不合 main、不 push、不删除工作树、不开下一轮。

## 未测/边界

见契约第 11 节。PE 驱动 GUI 不在本轮；Windows 参考执行无指令 Trace；人工 GUI 观感仍需用户确认。
