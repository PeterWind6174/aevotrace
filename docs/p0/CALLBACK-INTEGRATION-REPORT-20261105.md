# P0 共同回调 ABI 与双 GUI 最小集成：阶段报告

日期：2026-11-05/06。代码 HEAD：`afff569`；最终文档提交：本报告提交。基线：main `c91a3f7`（未合入、未 push、未删除 worktree）。结论：**有限 A/B/C 集成链通过独立 QA 与 reviewer 复核；P0 整体和首版仍未通过。**

## 1. 完成范围

同一条链路已经跑通：

```text
compat/native 控件事件
  -> Host (AppKit 主线程)
  -> 同一 libaev_bridge.dylib
  -> 固定 x64 COFF fixture 的 AOT/JIT Guest 回调
  -> Guest 计算 + HostImport
  -> Host 两阶段提交结果
  -> 返回 Host
```

- 同一 Guest 输入对象：`spikes/p0-engine/testdata/callback/aev_callback_fixture.obj`，SHA-256 `4f6929f7c71c82169295b6675517a839ae9533a85941e2632115bf0ef47328dc`。
- 同一执行桥：`libaev_bridge.dylib`；同一个 `JitRuntime` 由 legacy runner 与 bridge 共用。
- 两个 GUI 后端：`compat` 使用既有 `Compat*` 自绘控件；`native` 使用真实 `NSWindow`/`NSTextField`/`NSButton`，均在 AppKit 主线程。
- ENGINE 集成路径不加载/执行 Unicorn；旧 Unicorn 路径保留为显式历史对照。

## 2. 分阶段结果

### A — P0-CALLBACK-ABI-01

- 契约：`docs/p0/CALLBACK-ABI-20261105.md`，最终 callback ABI **v2**（Guest ABI 仍 v1）。
- 最小边界：RCX/RDX/R8/R9 四个整数/指针参数、RAX 返回、单层 Host→Guest→Host、主线程、session-local Guest 内存、checked load/store、显式 HostImport、状态/尺寸/输入哈希校验、明确退出原因。
- 明确不支持：任意重入、线程/模态、通用 PE/Win32、TLS/SEH、SIMD/浮点、TSO/原子、代码失效、性能结论。
- fixture：MSVC x64 `/O2 /GS- /Brepro`，`.aevtext` 86 字节、27 指令、5 块（4 AOT + JIT-only `0x140001020`），无 `.aevtext` 重定位；Windows 参考链接同一对象，`AEVTEXT_INSNS 27 MATCH`，CASE 一致。
- 会签：compiler/platform/reviewer 均已对最终 v2 提交 `SIGN-OFF`。

### B — P0-CORE-BRIDGE-01

- 从 `runner.cpp` 抽取共享 `JitRuntime`；新增 `include/aev_bridge.h`、`src/aev_bridge.cpp`、`src/aev_bridge_test.cpp`。
- AOT dylib 导出 `aev_aot_abi_version`、`aev_guest_state_size`、`aev_input_hash`；bridge 逐项校验输入 COFF 的 SHA/标准 FNV-1a-64、AOT 块表、入口和结构尺寸；mutated AOT 返回 `ABI_MISMATCH`。
- 覆盖：正常三输入、100 次重复调用、Host helper、未知导入、非法内存、函数内非块边界、0/0xDEAD0000、注入 JIT 失败、哨兵/RSP 不匹配、重入/调用中 destroy、错误线程、Host 返回失败/异常/非法状态、helper 成功后 Guest 故障、步数上限、错误配置/加载、导入 ID 校验。
- 结果：ctest **4/4 PASS**（原 ENGINE 三组 22/489/22 断言 + callback 77/77 checks）；callback 脚本 `ALL CALLBACK CHECKS PASSED`；真实 AOT/JIT 跨 `0x140001000/0x14000101c` 与 `0x140001020`。

### C — P0-UI-INT-01

- 新增 `harness/engine_bridge.py`、`engine_host.py`、`engine_scenario.py`、`run_engine.py`、`scripts/run_engine_gui.sh`；`UnsupportedApi` 抽到 `errors.py`，legacy 路径 import 回归通过。
- 自动场景：
  - alpha `Aevoβ中` op7 → Guest `3828036654`（Windows reference 一致）。
  - beta `macOS-47` op9 → Guest `614625053`（Windows reference 一致）。
  - 独立序列 `GUI-独立-输入-2026-11` op `0x1357` → Guest `3146212972`，独立 FNV oracle。
  - 同一实例 100 次按钮回调；计数器 110，`total_jit_compiles=1`，`total_jit_execs=924`；每击 AOT 4 块、JIT 命中缓存。
- 负例：直接和事件驱动的未知导入、非法内存、未支持重入、helper 成功后 Guest 故障；失败不提交结果控件；故障后恢复成功。
- 两后端各 **43/43** 自动检查、4 张截图；`backend_proof` 主线程、真实 AppKit/compat 类、无跨后端事件；`otool`/`sys.modules` 无 Unicorn/pefile；forced native failure 退出 1 且 `compat_events=0`。

## 3. 提交、构建与测试证据

| 项 | 值 |
| --- | --- |
| 基线 main | `c91a3f7` |
| A | `96ff890` |
| B code / report | `f023d78` / `e2e577b` |
| C | `037bae2` |
| 后续修正 | `f4322b2`, `9ee7430`, `484d5e2`, `07ce4f5`, `afff569` |
| 最终代码 HEAD | `afff569` |
| 最终构建 | `/Users/peterwind/Script/Systems/Aevotrace/.local/build-callback-final` |
| AOT dylib | `5773a3b61d9aada250bfb8b2b7680ebd84cc67292759a0a8a4a09450d3b0523e`（构建目录绑定） |
| bridge dylib | `d6bcdc1e374df88e95d10b9652cfa8936724854942a1e9283896f60011dcaeea`（构建目录绑定） |
| Windows object | `4f6929f7...`；`AEVTEXT_INSNS 27 MATCH`；`BUILD_RUN_OK` |
| QA-final | `.local/results/P0-CALLBACK-INTEGRATION-01/QA-final/REPORT.md`，HEAD `afff569` PASS |
| GUI summaries | `gui-final4/compat/summary.json`、`gui-final4/native/summary.json`、`gui-final4/native-failure/summary.json` |
| 证据索引 | `.local/results/P0-CALLBACK-INTEGRATION-01/EVIDENCE.md` |

最终 QA 的独立输入：`FinalQA-独立-2026-11-06` op `0x2468` → `0xA569A008`，helper/counter/AOT/JIT 均通过。

## 4. 模型/会话证据

- 主会话：`aevotrace-coordinator`，`opencode-go / deepseek-v4.1-flash / variant=max`。
- 子会话：explore、compiler、platform、reviewer、QA；实际会话元数据均为 `opencode-go / deepseek-v4.1-flash / variant=max`，parent/directory 记录在 `.local/results/P0-CALLBACK-INTEGRATION-01/session-*.json`。
- 未读取/复制凭据，未更改 provider/model/账号，未启动 `opencode run`/CLI worker。QA/reviewer 为只读审查；实施由 coordinator 在隔离 worktree 串行完成。
- reviewer 最终 `SIGN-OFF: accepted`；compiler/platform `SIGN-OFF: v2 committed accepted`。

## 5. 独立 QA / reviewer 结论

- QA-final：`PASS` at `afff569`；4/4 ctest、77/77 bridge、Windows reference、compat/native 43/43、独立输入、标准 FNV-1a-64、无 Unicorn/pefile。
- Reviewer：`SIGN-OFF: accepted`，无剩余 blocker；非阻断项见下。
- 仍待人工：Windows/macOS 物理键鼠、焦点、输入法、窗口外观/可用性验收。

## 6. 未测/未包含

- 通用 PE/DLL 装载、Win32 API 全集、TLS/SEH/unwind、真实应用、线程/模态/任意重入、原子/TSO、代码版本失效、独立 `.app` 打包、性能/持久化缓存、真实 ORC 编译失败强制注入。
- callback 侧 `guest_read/guest_write` 和“同输入、不同 AOT 块表”错配未单独测试。
- “helper 成功后 Guest 故障”的事件驱动路径代码正确，但现有测试主要直接经 `_call_guest` 触发；`probe_unknown_import_event` 的标签断言使用 `self.result_text` 而非控件实际文本；`otool` 执行失败目前不会使运行失败（只断言出现了 Unicorn/pefile）。这些为 reviewer 非阻断项。
- native 该最小链未创建 `NSMenu`，`backend_proof.main_menu_is_nsmenu=false`；要求的窗口/输入框/按钮/结果控件均为真实 AppKit。
- AOT/bridge 哈希受构建目录/install name/debug 路径影响；发布时必须绑定构建目录与 HEAD。
- 以上均不改变本轮有限链结论，也不构成 P0/首版完成。

## 7. 分支/工作区状态

- main：`c91a3f7`，未合入、未 push。
- task：`p0/callback-integration-01`；integration：`p0/integration-callback-01`；均在最终代码 `afff569` + 文档提交。
- worktree：`.worktrees/p0-callback-integration`、`.worktrees/p0-callback-integration-qa` 保留；旧 worktree 未复用。
- 未开始 Trace/版本失效下一阶段；未自行启动后续任务。

## 8. 下一轮建议

1. 先做代码版本/输入身份失效：同一输入不同代码版本、AOT 块表错配、缓存失效的负例。
2. 再扩 PE/DLL 装载和真实 Win32 子集，优先覆盖选定真实应用依赖。
3. 为 GUI 增加事件驱动的 helper-after-fault、真实控件文本 oracle、`otool` fail-closed；补人工键鼠/焦点/输入法验收。
4. 继续保留 callback 诊断日志与 Windows/Mac 同哈希对象对照，不把本轮调用日志称为完整 Trace 系统。
