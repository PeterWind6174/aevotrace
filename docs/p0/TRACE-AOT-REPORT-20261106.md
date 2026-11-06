# P0-TRACE-AOT-01 阶段报告：实际执行 Trace → 离线补充 AOT → 新进程验证

日期：2026-11-06（Asia/Shanghai）。状态：**实现、reviewer/QA 阻断修复与自动验证通过；独立 QA 尚未复核最终代码 HEAD `9c2aafe`；P0 整体与首版仍未通过。**

本轮是从已接受的 callback 集成分支继续的有界实验，不是通用转换器、完整 PE/Win32 支持、完整运行中代码失效或性能结论。

## 1. 版本与基线

| 项目 | 值 |
| --- | --- |
| main | `c91a3f7`（本轮未合入、未 push） |
| 已接受 callback 基线 | `4bb69abee6239416956204c80da1e93ef95489d0`（文档 HEAD；源码 `afff569be1c3399241b3bc1edb609d8a5a584f3d`） |
| 本轮分支 / worktree | `p0/trace-aot-01` / `.worktrees/p0-trace-aot` |
| 契约提交 | `1f1fe0043503b740a645ed7c3fe1f5ea760b37a2`（trace/artifact/coverage v1，compiler/binary/platform/reviewer 会签） |
| 实现提交 | `8325b64c4c182da6e6c9400bf97118e1b31219b2`；reviewer/QA 阻断修复 `823ede4`、`603a246`、`9c2aafe` |
| 最终干净构建 | `.local/build-trace-aot-final3`（Ninja，46/46 targets） |
| 最终自动测试 | CTest 5/5 PASS；独立 trace/AOT 脚本 PASS |

## 2. 本轮实现

- **身份**：完整输入 COFF 的 SHA-256 成为持久化权威身份；FNV 仅保留为内部快速校验。AOT artifact 导出输入 SHA-256、入口地址、入口函数名 SHA-256（UTF-8、无 NUL/长度前缀）、target triple、translator/artifact ABI、全部块与 selected 块表。
- **块身份**：`offset` 为 section-relative（`fn.section_offset + (rip - fn.address)`），`size` 覆盖到 terminator 末字节；无 terminator 时取下一 leader，仍无则取函数末尾。块哈希取实际 COFF section 字节，不从 `fn.bytes[offset]` 误取；bridge 与 supplement 共用实现。
- **真实记录**：AOT dylib 的 `aev_note_aot` 通过 `GuestState.reserved0` 的瞬态 `AevTraceSink` 记录 AOT 块；共享 `JitRuntime` 在 ORC lookup 成功后记录 JIT compile、在真正调用 JIT 函数前记录 exec；bridge 记录 dispatch miss、HostImport id（在栈参数读取成功、实际调用 Host 前；v1 不记录原始参数）、call_end/错误。块事件字段按契约使用 `offset`/`size`/`block_sha256`，不持久化 Host 指针。trace 启用时 `reserved0` 仅在调用内有效，所有退出路径清零；trace open/close 限制在 owner 线程且不能在调用中操作，destroy 时自动 flush/close。
- **coverage 与 AOT**：删除“IMUL 只能 JIT”的硬编码；AOT emitter 可发射受支持的 IMUL。translator 新增可重复 `--aot-include-rip` / `--aot-exclude-rip`（同一 RIP 冲突报错），默认 coverage 仍排除 IMUL 块以保留旧回归；AOT artifact 新增全部/selected 块表。bridge 在加载时比较实际 dylib 的 SHA-256、输入/FNV/entry/function/target/translator/artifact/guest ABI，检查 `aev_all_blocks` 与 `Program` 完全一致，selected 表排序去重、`fn` 必须等于 `dlsym("aev_blk_<rip>")`，再据 selected 反推每块 `jit_only`；GUI 通过 bridge 查询实际 selected 表，而不是只信 manifest。
- **离线补充**：`scripts/aot_supplement.py` 校验 trace header 与事件（输入 SHA/FNV、entry、function、ABI/translator/target、header 全部块与 COFF 重建结果一致、selected 为排序子集、call 编号单调且每个 call 必须先 call_end、每个 trace 的 call_end 全部 status 0、所有块事件的 offset/size/hash 与 COFF 一致、`aot` 事件必须属于 header selected、`jit_*` 事件不得属于 header selected、host_import 仅 id），以实际执行的 AOT/JIT 块并集为 selected，调用共享 `aev-translate` 生成新 IR/header/manifest。输出先在目标目录暂存，再带备份/回滚发布，不覆盖 base artifact，不重编业务 C 源码，不复制 ORC dispatcher。新建 dylib 由 `scripts/publish_aot.py` 在临时路径链接后校验实际导出表/输入 SHA/ARM64 slice/promoted RIP，再原子发布；失败保留旧产物。
- **新 fixture**：`testdata/trace/aev_trace_fixture.c/.obj`，函数在独立 `.aevtext` section；A 路径（`op != 0xA5A5`）不执行 B 分支，B 路径有独立基本块和 IMUL，B RIP 只在 B 执行事件出现。
- **GUI**：`run_engine` 增加显式 `--coverage-profile base|supplemented`；base 期望 `aot_table_count=4`、总 JIT compile 1、JIT exec > 0；supplemented 期望 5、0、0，并绑定 supplement manifest、AOT SHA-256 与 promoted RIP `0x140001020`。新增 `aot_table_count` 检查；精确 44 项 = 42 个原有检查名称（helper-after-fault 4 个同名升级为真实控件断言）+ 1 个同名位置替换（`loop_100_jit_executed` → `loop_100_jit_policy`）+ 1 个新增 `aot_table_count`。GUI 脚本增加 supplemented 下拒绝 base artifact 的负例。
- **测试**：新增 `p0_trace_aot`，覆盖 trace API 生命周期、callback A 的 JIT→补充→新进程、trace fixture A/B 的新分支 JIT→补充→新进程，以及 trace/artifact 负例；保留原 4 组 ctest。

## 3. 自动验证结果与证据

### 3.1 构建与 ctest

```text
cmake -S spikes/p0-engine -B .local/build-trace-aot-final3 -G Ninja  exit 0
cmake --build .local/build-trace-aot-final3                         exit 0 (46/46)
ctest --test-dir .local/build-trace-aot-final3 --output-on-failure  exit 0
  1/5 p0_engine_closed_loop   Passed  1.10s
  2/5 p0_core_fix_semantics   Passed 14.01s
  3/5 p0_engine_empty_aot     Passed  0.63s
  4/5 p0_callback_bridge      Passed  5.12s
  5/5 p0_trace_aot            Passed  8.51s
  100% tests passed out of 5; Total 29.38s
```

独立运行 `bash spikes/p0-engine/scripts/run_trace_aot_test.sh .local/build-trace-aot-final3 "$PWD/spikes/p0-engine"` 退出码 **0**，输出 `ALL TRACE/AOT CHECKS PASSED`。该脚本还覆盖 B-only trace 的精确执行并集、no-JIT 同 coverage 补充、publisher 拒绝坏 dylib。

### 3.2 A：callback fixture

- 输入 SHA-256：`4f6929f7c71c82169295b6675517a839ae9533a85941e2632115bf0ef47328dc`。
- base：业务结果 `guest_rax=3828036654`；`jit_compiles=1`（`0x140001020`）；AOT 表 4。
- 补充：AOT 表 5；`guest_rax=3828036654`；`jit_compiles=0`、`jit_execs=0`；trace 中 `0x140001020` 出现 `aot` 事件且没有 `jit_compile`；实际 selected 表包含 `0x140001020`。补充 dylib SHA-256 `e4661369ab3b6e95ee22800f2c4316c81b96e635e53237a6f896116139c8fefc`。
- 旧 base artifact SHA-256 `a608aa9dec2635d2095de7fbc7522b38b860dc147b170bb6044ee85d7fd016a8`，补充后重跑仍可执行且哈希未变；不同构建的 Mach-O UUID 可能使 AOT dylib SHA 不同，GUI 运行时以 manifest 中的本次 SHA 绑定。

### 3.3 B：新增 trace fixture（准备集外分支）

- 输入对象 SHA-256：`145800f4a4729c201aab2c94a74937d349e01f6ec7871658b6becd3773bd1205`。
- 块布局：`0x140001000` size 21、B-only IMUL `0x140001015` size 17、A body `0x140001026` size 9、helper `0x14000102f` size 9、counter/return `0x140001038` size 20。
- base A：`guest_rax=3922`，`jit_compiles=0`；A 执行事件中不含 `0x140001015`。
- base B：`guest_rax=45320`，真实 JIT `0x140001015`（`jit_compiles=1`），证明新分支首次被动态发现。
- 补充 A/B 新进程：A `3922`、B `45320`；两者 `jit_compiles=0`、`jit_execs=0`；AOT 表 5；B trace 中 `0x140001015` 为 `aot` 且没有对应 `jit_compile`；实际 selected 表包含 `0x140001015`。补充 dylib SHA-256 `463ac0671780921bd37c98ed1745402e22dc589578ef082729ec9b84ad16b512`。
- B-only 补充：只用 B trace 时生成 selected 4（entry、B、helper、counter/return，排除未执行的 `0x14000101c`），新进程 B 结果 45320、零 JIT；证明 selected 是实际执行并集，不是默认 coverage 的并集。dylib SHA-256 `d28ee0f7535c4462455562d6944e0205562efc8f31ca76b8d10058a130f20b04`。
- no-JIT 输入：只用 A trace（本来就是同 coverage、无 JIT）生成的 artifact selected 4、promoted 为空，新进程 A 结果 3922、零 JIT；dylib SHA-256 `1014d3270f13cfc0b8ad6d0ec89aa1867dd1b1941ef3accbf06885bee99147da`。
- 说明：trace header 的 `all_blocks` 按契约包含全部可翻译块，因此 A 的 header 也会列出 B 块；验收以执行事件集合判断“B 未在 A 出现”。

### 3.4 负例

以下均明确失败（退出码非 0），且未生成完整输出；base artifact 仍可复跑：

- 输入 SHA、`aot_sha256`（缺失/全 0）、块 SHA、header all table、乱序/重复表、entry/function/target、callback/guest/artifact/translator ABI、selected 重复、`aot` 事件不在 selected、`jit_*` 事件属于 selected、`jit_exec` 块身份缺失、call 编号非单调/事件交错、host_import 参数、截断/非对象/非法 JSON、`call_end` 非 0/字符串 status/缺少 `error_pc`/`guest_error`；
- base artifact 传入 supplemented profile（`aot_table_count` 4≠5，退出 1）；
- mutated 输入 AOT、错误 all 表、错误 selected `fn` 指针、selected 表重复、错误 target、错误 artifact/guest ABI；
- `publish_aot.py` 缺少 promoted RIP 时拒绝并保留旧 dylib；所有负例均未生成完整输出。

### 3.5 trace API 生命周期

`TRACE_API_OK`：重复 open 返回 `INVALID_ARGUMENT(1)`；调用中 open 返回 `REENTRY(9)`；不可写路径失败；close 幂等；close 后调用不再增长 trace；destroy 时 trace 自动 flush/close；块事件含 `offset` 且不含 `section_offset`；调用内 `reserved0` 非零、调用返回后为 0。

### 3.6 GUI（最终构建）

- base profile：compat/native 各 **44/44**；`aot_table_count=4`；`total_jit_compiles=1`；`jit_execs=920`；AOT SHA `a608aa9d…`；实际 selected 表不含 `0x140001020`。
- supplemented profile：compat/native 各 **44/44**；`aot_table_count=5`；`total_jit_compiles=0`；`jit_execs=0`；AOT SHA `e4661369…`，与 manifest 绑定；实际 selected 表包含 `0x140001020`。
- 真实按钮 → helper 成功 → Guest counter 故障 → `backend.get_text` 实际控件文本不变 → 恢复：4 项检查通过。
- supplemented 下 base artifact 负例：退出 1，`success=false`，错误 `supplemented profile expects 5 AOT blocks, got 4`，无 Guest 提交、compat 事件 0。
- forced native failure：退出 1，compat 事件 0、native 事件 5。
- 截图各后端 4 张，位于 `.local/results/P0-TRACE-AOT-01/final2-gui-{base,supp}/{compat,native}/`；本轮 coordinator 未逐张人工查看，未代替物理键鼠/焦点/IME 验收。

### 3.7 Windows 参考

通过 `ssh win` 用提交对象与同一 driver 构建运行，退出码 0：

```text
CASE A op=1 x=7 result=3922 counter=1
CASE B op=42405 x=9 result=45320 counter=2
FIXTURE_SHA256 145800f4a4729c201aab2c94a74937d349e01f6ec7871658b6becd3773bd1205
REBUILT_SHA256 c4d571e27c6594ef84320a0397823f867cd89bf684cea467a1f143f1e2a96dc8
REBUILT_MATCH false
COMMITTED_OBJECT_BUILD_RUN_OK
```

这是 Windows 原生**行为对照**，driver 始终链接提交的固定对象，不是指令级 Trace。当前 VS 18.11 重新编译的字节与提交对象不同（`.aevtext` 指令序列相同），因此不宣称 Windows 对象字节级可复现；行为身份以提交对象 SHA-256 为准。

## 4. 哈希与证据位置

| 对象 | SHA-256 |
| --- | --- |
| callback 输入 COFF | `4f6929f7c71c82169295b6675517a839ae9533a85941e2632115bf0ef47328dc` |
| trace fixture 输入 COFF | `145800f4a4729c201aab2c94a74937d349e01f6ec7871658b6becd3773bd1205` |
| base callback AOT dylib | `a608aa9dec2635d2095de7fbc7522b38b860dc147b170bb6044ee85d7fd016a8` |
| supplemented callback AOT dylib | `e4661369ab3b6e95ee22800f2c4316c81b96e635e53237a6f896116139c8fefc` |
| base trace AOT dylib | `e8484435de3cebc07bdf8876a13ed0c73391593da0f7cd292b5521ef824997b7` |
| supplemented trace AOT dylib | `463ac0671780921bd37c98ed1745402e22dc589578ef082729ec9b84ad16b512` |
| `libaev_bridge.dylib` | `1a4f3555c9e224f96d2efd703f3d6d232bfc46cd2dc194f57cab1d995352b397` |

原始证据（根 `.local/results/P0-TRACE-AOT-01/`）：

- `ctest-final3.log`、`build-final3.log`、`final3-trace-test.log`；
- `final3-gui-base.log`、`final3-gui-supp.log`、`final3-hashes.txt`；
- `windows-trace-fixture-final.log`；
- 供 QA/reviewer 读取的实现侧结果：`.local/build-trace-aot-final3/results-trace-aot/`。

## 5. 未测项与已知限制

- 未做通用 PE loader、完整 Win32 API、真实 EXE/DLL 应用、独立 `.app`、签名/公证。
- 未做线程/模态/任意嵌套重入、运行中自修改/写保护/同址重分配/并发失效；关闭重开进程不算 live invalidation。
- trace 为块级，不是指令级或 Windows 原生 Trace；不声称完整覆盖率。
- target triple 固定为 `arm64-apple-macosx14.0.0`；不承诺移植到其他 macOS/架构。
- 未做性能测量，不提出提速或开销结论。
- GUI 自动路径通过，但物理键鼠、窗口焦点、IME 和人工视觉验收仍需用户/QA 完成。
- 上一轮 `5773...` AOT dylib 仅归档，不与新 artifact ABI 兼容；本轮“旧 artifact 可跑”指新 translator 的 base artifact。
- `AevCallResult.aot_blocks` 仍是每次调用的 AOT 入口次数，不是 AOT 表大小；表大小由 `AevArtifactInfo.aot_table_count` 提供。

## 6. 独立验证状态

- reviewer 在 `74fc21e` 返回 `CHANGES REQUIRED`：trace 字段名、call 单调性、块事件身份/source 一致性、发布原子性、GUI promoted RIP 实际表绑定、负例/lifecycle 覆盖。上述项已在 `823ede4`/`603a246` 中处理；复核又发现 selected 未精确按执行并集、bool/float 类型、publisher 全表/fn/符号校验、`aev_block_count` 越界，已在 `9c2aafe` 修复，并新增对应负例；需 reviewer 对新 HEAD 复核。
- QA 在 `74fc21e` 返回 FAIL（核心功能通过）：`aot_supplement.py` 仍接受非单调 call、缺失/全 0 `aot_sha256`、缺失 `call_end.error_pc/guest_error`、字符串 status，以及 malformed/non-object JSON 抛未捕获异常。修复后新增 `--base-aot` 绑定、header/`call_end` 字段/类型校验、显式 JSON 错误和回归负例；需 QA 对新 HEAD 重跑。
- 新 HEAD 的独立 QA/reviewer 结论出来前，本报告不能视为最终通过。

本报告由实现侧 coordinator 编写，**不能替代独立 QA/reviewer**。下一步需在最终代码 HEAD 上：

1. QA 独立全新构建、复跑 5 组 ctest、trace/AOT 脚本、base/supplemented 双 GUI、Windows 参考，并检查截图与哈希；
2. reviewer 只读审查契约覆盖、ABI/身份校验、负例是否可绕过、旧 artifact 保留、GUI profile 断言；
3. 合并 QA/reviewer 结论；若有阻断则修复后重跑。

## 7. 分支状态

停在集成分支 `p0/integration-trace-aot-01`（worktree `.worktrees/p0-trace-aot-qa`）与本轮实现分支 `p0/trace-aot-01`（worktree `.worktrees/p0-trace-aot`）。实现源码头为 `9c2aafe`；文档 HEAD 为本报告所在提交。未合入 main、未 push、未删除 worktree。上一轮 callback worktree/证据保留。
