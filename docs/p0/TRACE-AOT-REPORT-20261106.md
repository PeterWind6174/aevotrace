# P0-TRACE-AOT-01 阶段报告：实际执行 Trace → 离线补充 AOT → 新进程验证

日期：2026-11-06（Asia/Shanghai）。状态：**实现与自动验证通过；独立 QA/reviewer 尚未复核最终代码 HEAD；P0 整体与首版仍未通过。**

本轮是从已接受的 callback 集成分支继续的有界实验，不是通用转换器、完整 PE/Win32 支持、完整运行中代码失效或性能结论。

## 1. 版本与基线

| 项目 | 值 |
| --- | --- |
| main | `c91a3f7`（本轮未合入、未 push） |
| 已接受 callback 基线 | `4bb69abee6239416956204c80da1e93ef95489d0`（文档 HEAD；源码 `afff569be1c3399241b3bc1edb609d8a5a584f3d`） |
| 本轮分支 / worktree | `p0/trace-aot-01` / `.worktrees/p0-trace-aot` |
| 契约提交 | `1f1fe0043503b740a645ed7c3fe1f5ea760b37a2`（trace/artifact/coverage v1，compiler/binary/platform/reviewer 会签） |
| 实现提交 | `8325b64c4c182da6e6c9400bf97118e1b31219b2` |
| 最终干净构建 | `.local/build-trace-aot-final`（Ninja，46/46 targets） |
| 最终自动测试 | CTest 5/5 PASS；独立 trace/AOT 脚本 PASS |

## 2. 本轮实现

- **身份**：完整输入 COFF 的 SHA-256 成为持久化权威身份；FNV 仅保留为内部快速校验。AOT artifact 导出输入 SHA-256、入口地址、入口函数名 SHA-256（UTF-8、无 NUL/长度前缀）、target triple、translator/artifact ABI、全部块与 selected 块表。
- **块身份**：`offset` 为 section-relative（`fn.section_offset + (rip - fn.address)`），`size` 覆盖到 terminator 末字节；无 terminator 时取下一 leader，仍无则取函数末尾。块哈希取实际 COFF section 字节，不从 `fn.bytes[offset]` 误取；bridge 与 supplement 共用实现。
- **真实记录**：AOT dylib 的 `aev_note_aot` 通过 `GuestState.reserved0` 的瞬态 `AevTraceSink` 记录 AOT 块；共享 `JitRuntime` 在 ORC lookup 成功后记录 JIT compile、在真正调用 JIT 函数前记录 exec；bridge 记录 dispatch miss、HostImport id（v1 不记录原始参数）、call_end/错误。trace 启用时 `reserved0` 仅在调用内有效，所有退出路径清零；trace open/close 限制在 owner 线程且不能在调用中操作，destroy 时自动 flush/close。
- **coverage 与 AOT**：删除“IMUL 只能 JIT”的硬编码；AOT emitter 可发射受支持的 IMUL。translator 新增可重复 `--aot-include-rip` / `--aot-exclude-rip`（同一 RIP 冲突报错），默认 coverage 仍排除 IMUL 块以保留旧回归；AOT artifact 新增全部/selected 块表。bridge 在加载时比较实际 dylib 的 SHA-256、输入/FNV/entry/function/target/translator/artifact/guest ABI，检查 `aev_all_blocks` 与 `Program` 完全一致，selected 表排序去重、`fn` 必须等于 `dlsym("aev_blk_<rip>")`，再据 selected 反推每块 `jit_only`。
- **离线补充**：`scripts/aot_supplement.py` 校验 trace header 与事件（输入 SHA/FNV、entry、function、ABI/target、header 全部块与 COFF 重建结果一致、selected 为排序子集、每个 trace 的 call_end 全部 status 0、块 offset/size/hash 与 COFF 一致、host_import 仅 id），以实际执行的 AOT/JIT 块并集为 selected，调用共享 `aev-translate` 生成新 IR/header/manifest；IR/manifest 先写临时目录再 rename，不覆盖 base artifact，不重编业务 C 源码，不复制 ORC dispatcher。
- **新 fixture**：`testdata/trace/aev_trace_fixture.c/.obj`，函数在独立 `.aevtext` section；A 路径（`op != 0xA5A5`）不执行 B 分支，B 路径有独立基本块和 IMUL，B RIP 只在 B 执行事件出现。
- **GUI**：`run_engine` 增加显式 `--coverage-profile base|supplemented`；base 期望 `aot_table_count=4`、总 JIT compile 1、JIT exec > 0；supplemented 期望 5、0、0，并绑定 supplement manifest、AOT SHA-256 与 promoted RIP `0x140001020`。新增 `aot_table_count` 检查；精确 44 项 = 42 个原有检查名称（helper-after-fault 4 个同名升级为真实控件断言）+ 1 个同名位置替换（`loop_100_jit_executed` → `loop_100_jit_policy`）+ 1 个新增 `aot_table_count`。GUI 脚本增加 supplemented 下拒绝 base artifact 的负例。
- **测试**：新增 `p0_trace_aot`，覆盖 trace API 生命周期、callback A 的 JIT→补充→新进程、trace fixture A/B 的新分支 JIT→补充→新进程，以及 trace/artifact 负例；保留原 4 组 ctest。

## 3. 自动验证结果与证据

### 3.1 构建与 ctest

```text
cmake -S spikes/p0-engine -B .local/build-trace-aot-final -G Ninja   exit 0
cmake --build .local/build-trace-aot-final                          exit 0 (46/46)
ctest --test-dir .local/build-trace-aot-final --output-on-failure   exit 0
  1/5 p0_engine_closed_loop   Passed  1.04s
  2/5 p0_core_fix_semantics   Passed 14.32s
  3/5 p0_engine_empty_aot     Passed  0.58s
  4/5 p0_callback_bridge      Passed  5.51s
  5/5 p0_trace_aot            Passed  3.44s
  100% tests passed out of 5; Total 24.89s
```

独立运行 `bash spikes/p0-engine/scripts/run_trace_aot_test.sh .local/build-trace-aot-final "$PWD/spikes/p0-engine"` 退出码 **0**，输出 `ALL TRACE/AOT CHECKS PASSED`。

### 3.2 A：callback fixture

- 输入 SHA-256：`4f6929f7c71c82169295b6675517a839ae9533a85941e2632115bf0ef47328dc`。
- base：业务结果 `guest_rax=3828036654`；`jit_compiles=1`（`0x140001020`）；AOT 表 4。
- 补充：AOT 表 5；`guest_rax=3828036654`；`jit_compiles=0`、`jit_execs=0`；trace 中 `0x140001020` 出现 `aot` 事件且没有 `jit_compile`。补充 dylib SHA-256 `50efd83c155bffb759c4aadb8690d61520006d8b347cde749acbdc568c244d66`。
- 旧 base artifact SHA-256 `9d4aaa4ae160ba2951d93894b2a45af8bc4e4e23a1521e5e22cc043f1dbb6b54`，补充后重跑仍可执行且哈希未变。

### 3.3 B：新增 trace fixture（准备集外分支）

- 输入对象 SHA-256：`145800f4a4729c201aab2c94a74937d349e01f6ec7871658b6becd3773bd1205`。
- 块布局：`0x140001000` size 21、B-only IMUL `0x140001015` size 17、A body `0x140001026` size 9、helper `0x14000102f` size 9、counter/return `0x140001038` size 20。
- base A：`guest_rax=3922`，`jit_compiles=0`；A 执行事件中不含 `0x140001015`。
- base B：`guest_rax=45320`，真实 JIT `0x140001015`（`jit_compiles=1`），证明新分支首次被动态发现。
- 补充 A/B 新进程：A `3922`、B `45320`；两者 `jit_compiles=0`、`jit_execs=0`；AOT 表 5；B trace 中 `0x140001015` 为 `aot` 且没有对应 `jit_compile`。补充 dylib SHA-256 `7dfdac40b1c1b9439eb21697505de6c38a651171f5efb21d724c51c2c8441130`。
- 说明：trace header 的 `all_blocks` 按契约包含全部可翻译块，因此 A 的 header 也会列出 B 块；验收以执行事件集合判断“B 未在 A 出现”。

### 3.4 负例

以下均明确失败（退出码非 0），且未生成完整输出；base artifact 仍可复跑：

- 输入 SHA、块 SHA、header all table、乱序表、translator ABI、host_import 参数、截断文件、失败 `call_end`；
- base artifact 传入 supplemented profile（`aot_table_count` 4≠5，退出 1）；
- mutated 输入 AOT、错误 all 表、错误 selected `fn` 指针。

### 3.5 trace API 生命周期

`TRACE_API_OK`：重复 open 返回 `INVALID_ARGUMENT(1)`；调用中 open 返回 `REENTRY(9)`；不可写路径失败；close 幂等；close 后调用不再增长 trace；destroy 时 trace 自动 flush/close。

### 3.6 GUI（最终构建）

- base profile：compat/native 各 **44/44**；`aot_table_count=4`；`total_jit_compiles=1`；`jit_execs=920`；AOT SHA `9d4aaa4a…`。
- supplemented profile：compat/native 各 **44/44**；`aot_table_count=5`；`total_jit_compiles=0`；`jit_execs=0`；AOT SHA `50efd83c…`，与 manifest 绑定。
- 真实按钮 → helper 成功 → Guest counter 故障 → `backend.get_text` 实际控件文本不变 → 恢复：4 项检查通过。
- supplemented 下 base artifact 负例：退出 1，`success=false`，错误 `supplemented profile expects 5 AOT blocks, got 4`，无 Guest 提交、compat 事件 0。
- forced native failure：退出 1，compat 事件 0、native 事件 5。
- 截图各后端 4 张，位于 `.local/results/P0-TRACE-AOT-01/final-gui-{base,supp}/{compat,native}/`；本轮 coordinator 未逐张人工查看，未代替物理键鼠/焦点/IME 验收。

### 3.7 Windows 参考

通过 `ssh win` 用同一提交的对象与 driver 构建运行，退出码 0：

```text
CASE A op=1 x=7 result=3922 counter=1
CASE B op=42405 x=9 result=45320 counter=2
FIXTURE_SHA256 145800f4a4729c201aab2c94a74937d349e01f6ec7871658b6becd3773bd1205
BUILD_RUN_OK
```

这是 Windows 原生**行为对照**，不是指令级 Trace；固定对象 SHA-256 与提交 `.obj` 一致。

## 4. 哈希与证据位置

| 对象 | SHA-256 |
| --- | --- |
| callback 输入 COFF | `4f6929f7c71c82169295b6675517a839ae9533a85941e2632115bf0ef47328dc` |
| trace fixture 输入 COFF | `145800f4a4729c201aab2c94a74937d349e01f6ec7871658b6becd3773bd1205` |
| base callback AOT dylib | `9d4aaa4ae160ba2951d93894b2a45af8bc4e4e23a1521e5e22cc043f1dbb6b54` |
| supplemented callback AOT dylib | `50efd83c155bffb759c4aadb8690d61520006d8b347cde749acbdc568c244d66` |
| base trace AOT dylib | `611059ad517440edbd9650316e70e052f828ac0b1aac947b01a0f0329eb82892` |
| supplemented trace AOT dylib | `7dfdac40b1c1b9439eb21697505de6c38a651171f5efb21d724c51c2c8441130` |
| `libaev_bridge.dylib` | `d2fb4faae649ee65bf2f36d0a628206d31092376c7473529d4b59b7b2822ed33` |

原始证据（根 `.local/results/P0-TRACE-AOT-01/`）：

- `ctest-final.log`、`build-final.log`、`final-trace-test.log`；
- `final-gui-base.log`、`final-gui-supp.log`、`final-gui-summary.json`、`final-gui-hashes.txt`；
- `final-callback_base.jsonl`、`final-trace_base_A.jsonl`、`final-trace_base_B.jsonl`、`final-callback_supp.json`、`final-trace_supp.json`；
- `windows-trace-fixture.log`。

## 5. 未测项与已知限制

- 未做通用 PE loader、完整 Win32 API、真实 EXE/DLL 应用、独立 `.app`、签名/公证。
- 未做线程/模态/任意嵌套重入、运行中自修改/写保护/同址重分配/并发失效；关闭重开进程不算 live invalidation。
- trace 为块级，不是指令级或 Windows 原生 Trace；不声称完整覆盖率。
- target triple 固定为 `arm64-apple-macosx14.0.0`；不承诺移植到其他 macOS/架构。
- 未做性能测量，不提出提速或开销结论。
- GUI 自动路径通过，但物理键鼠、窗口焦点、IME 和人工视觉验收仍需用户/QA 完成。
- 上一轮 `5773...` AOT dylib 仅归档，不与新 artifact ABI 兼容；本轮“旧 artifact 可跑”指新 translator 的 base artifact。
- `AevCallResult.aot_blocks` 仍是每次调用的 AOT 入口次数，不是 AOT 表大小；表大小由 `AevArtifactInfo.aot_table_count` 提供。

## 6. 待独立验证

本报告由实现侧 coordinator 编写，**不能替代独立 QA/reviewer**。下一步需在最终代码 HEAD `8325b64` 上：

1. QA 独立全新构建、复跑 5 组 ctest、trace/AOT 脚本、base/supplemented 双 GUI、Windows 参考，并检查截图与哈希；
2. reviewer 只读审查契约覆盖、ABI/身份校验、负例是否可绕过、旧 artifact 保留、GUI profile 断言；
3. 合并 QA/reviewer 结论；若有阻断则修复后重跑。

## 7. 分支状态

停在 `p0/trace-aot-01`，未合入 main、未 push、未删除 worktree。上一轮 callback worktree/证据保留。
