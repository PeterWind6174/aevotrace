# P0-CORE-BRIDGE-01：可复用 ENGINE 执行桥

状态：计划已准备，尚未派发。

## 输入与责任

- 阶段/需求：P0 收尾；R01/R06/R08；前置 P0-CALLBACK-ABI-01。
- owner：compiler（lead）+ platform；QA 独立复跑，reviewer 审查。
- 基线：从派发时 main 新建隔离 worktree；不使用已删除的旧 worktree。
- 允许范围：核心必要的薄接口/共享调度抽取、回调 fixture、桥接测试及构建记录。

## 目标

- 导出可由 GUI 使用的最小执行接口，按共同契约调用真实 x64 Guest 代码。
- 提供 Guest 内存受检查读写、HostImport 调用、明确错误返回和重复调用生命周期。
- 共用现有 AOT/JIT/helper，不复制第三套 ORC dispatcher，不手工重写 Guest 业务。
- 在无 GUI 测试中先完成 Host→Guest→Host→返回，证明正常 AOT/JIT 路径与负例。

## 验收

- 原 ENGINE ctest 不回归；新桥测试实际进入生成的 ARM64 代码。
- 至少覆盖输入变化、重复调用、Host helper、未知 helper、非法内存、未支持重入或调用状态。
- Windows 参考与 Mac 使用同一固定哈希的机器码输入；允许元数据/fixture 限制但如实说明。
- 最终提交绑定命令、退出码、输入/产物哈希和独立 QA。
- 不包含 GUI 后端实现、完整 PE/Win32、Trace/失效或性能产品化。

## 完成报告（2026-11-05，B 阶段）

- 基线/工作树：main `c91a3f7`，A 契约/fixture `96ff890`；本阶段代码 `f023d78`，分支 `p0/callback-integration-01`，worktree `.worktrees/p0-callback-integration`。
- 实现：新增 `include/aev_bridge.h`、`src/aev_bridge.cpp`、`src/aev_bridge_test.cpp`；从 `runner.cpp` 抽取 `src/jit_runtime.{hpp,cpp}`，runner 与 bridge 共用同一 ORC LLJIT 实现；`aev_mem.c`/`aev_load`/`aev_store`/AOT dispatcher 保持原路径。没有第三套 ORC dispatcher，没有在 Host 重写 Guest 业务。
- ABI/identity：AOT dylib 导出 `aev_aot_abi_version=1`、`aev_guest_state_size=256`、`aev_input_hash`；bridge create 逐项校验，并把 `aev_blocks`/`aev_block_count` 与翻译后 Program 的 AOT 块表逐项比较；同入口/同块地址但改动立即数的 mutated dylib 被 `ABI_MISMATCH` 拒绝。
- fixture：`spikes/p0-engine/testdata/callback/aev_callback_fixture.obj` SHA-256 `4f6929f7c71c82169295b6675517a839ae9533a85941e2632115bf0ef47328dc`；Windows 参考链接同一对象，`CASE alpha/beta/hidden` 与 Mac bridge 输出逐字一致（`reference_stdout.txt`）。
- 命令/结果（构建目录 `/Users/peterwind/Script/Systems/Aevotrace/.local/build-callback-b`）：
  - `cmake -S spikes/p0-engine -B ... -G Ninja` → 0；`cmake --build ...` → 0。
  - `ctest --test-dir ... --output-on-failure` → 4/4 PASS：原 `p0_engine_closed_loop`、`p0_core_fix_semantics`、`p0_engine_empty_aot` 不回归；新 `p0_callback_bridge` PASS。
  - `bash spikes/p0-engine/scripts/run_callback_bridge_test.sh ...` → `ALL CALLBACK CHECKS PASSED`；`FAILURES 0`；CASE 与 Windows 参考 diff 为空；JIT compile/exec `0x140001020` 实际发生；AOT `0x140001000/0x14000101c` 实际执行；mutated AOT 被拒绝。
  - 100 次连续回调：同一 bridge 实例，Guest 计数器 +100；首次调用 `jit_compiles=1`，后续 99 次 `jit_compiles=0`，每次 `jit_execs>0`；`total_jit_compiles=1`。
  - 负例：未知导入 `UNKNOWN_IMPORT`、非法内存 `MEMORY`、函数内非块边界/0/0xDEAD0000 `NO_BLOCK`、注入 JIT 失败 `JIT`、注入 RSP 不匹配 `INTERNAL`、重入/调用中 destroy `REENTRY`、错误线程 `WRONG_THREAD`、Host 返回失败/异常 `HOST`、步数上限 `STEP_LIMIT`、错误配置/结果尺寸 `INVALID_ARGUMENT`、坏路径 `LOAD`；helper 成功但 counter 指针非法时返回 `MEMORY` 且 pending 未提交。
- 产物哈希：fixture `4f6929...`；`libcallback_aot.dylib` `1c1f2319e219bd8a16cba57856fe913fcc9e423e823f132325e5c765676f0857`；`libaev_bridge.dylib` `ac7db34737a1756a57037cea4b69da3887ba1ee2c03a3b4ba8fa091c409ed974`；`aev_bridge_test` `f48f1d2c629cbca76d45bdb4eefcfcdf24bb051376d5f1f465b6958861785125`；Windows reference `7fe5b8bfbdb7002a52930f313d5d73b36944987debc7041c3f3c9f78e521e9da`。AOT dylib/ bridge dylib 均为 arm64 Mach-O；bridge 依赖 LLVM/Capstone/libSystem，无 Unicorn/pefile。
- Windows 参考：`build_msvc.ps1` 校验 committed object hash、比较 rebuilt `.aevtext` 27 条指令、链接 committed object；输出三组 CASE 与 `BUILD_RUN_OK`。证据在根 `.local/results/P0-CALLBACK-INTEGRATION-01/windows-reference-final.log`。
- 未完成/边界：本阶段无 GUI；无通用 PE/Win32、TLS/SEH、线程/模态、任意重入、Trace/失效、.app、性能。Guest 地址仍为 session-local 映射；`aev_bridge_set_test_fault` 只用于测试，GUI 不得调用。
- 后续：C 阶段在同一 bridge 上接 compat/native 最小窗口/输入/按钮/结果链。
