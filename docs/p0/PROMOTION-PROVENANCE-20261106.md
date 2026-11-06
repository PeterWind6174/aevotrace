# P0-PROMOTION-FIX-01：AOT 提升来源最小契约（Trace/artifact provenance v1.1）

日期：2026-11-06。状态：待 compiler/binary/platform 复签、reviewer 复核。只修订 P0-TRACE-AOT-01 的 baseline/`promoted_blocks` 语义，不改变 trace/artifact ABI 1 的字段布局、执行并集规则或产品范围。

## 1. 问题与范围

P0-TRACE-AOT-01 的 `aot_supplement.py` 从新 translator 的默认 coverage 推导 `base_selected`，未使用实际 `--base-aot` 的 selected 表。因此在“已全 AOT 再补充”时错误报告 `0x140001015` 为新增，在 B-only baseline 时也错误报告 B 块，而不是实际新增的 A body `0x140001026`。

本修订只修复 provenance/消费者语义。`new_selected` 仍由已校验 Trace 的实际执行事件并集决定；不把默认/旧 coverage 擅自并入，也不为修正 metadata 改变执行并集。

## 2. 实际 base artifact 是权威基线

`aot_supplement.py` 必须要求单一明确的 `--base-aot <dylib>`（不再静默使用默认 coverage）：

1. 加载实际 dylib，读取并校验 `aev_input_sha256`、`aev_input_hash`（FNV，与本次输入/translator 一致）、`aev_entry_addr`、`aev_function_name_sha256`、`aev_target_triple`、`aev_aot_abi_version`、`aev_artifact_abi_version`、`aev_translator_abi_version`、`aev_guest_state_size`（必须等于 `sizeof(GuestState)=256`）、`aev_all_block_count`/`aev_all_blocks`、`aev_block_count`/`aev_blocks`、`aev_aot_run` 存在，以及每个 selected `fn` 与 `aev_blk_<rip>` 符号相等。
2. 实际 base 的输入 SHA/FNV/entry/function/target/ABI/guest-state 必须与本次输入和 translator 结果一致；all-block 表必须与 `Program` 输入一致；selected 表必须排序、去重、为 all 表子集、每项 `fn` 与符号一致。
3. Trace header 的 `aot_sha256` 必须等于实际 base dylib 的 SHA-256；header `aot_selected` 必须与实际 base selected 表**逐项相等**，不能只检查“排序子集”或诊断字段。旧 `TRACE-AOT-CONTRACT` §5.1 的“诊断/子集”措辞由本修订取代。
4. 所有 Trace 必须引用同一个 base SHA 和同一个 selected 表；不同 base/coverage 的 Trace 混合输入明确拒绝。本轮不实现多基线合并。
5. 未提供 `--base-aot`、base artifact 缺失/不可加载、任一身份/表不匹配，必须在生成输出前失败；不覆写旧 artifact。

## 3. 集合语义

定义：

- `all_selected`：输入 COFF/translator 的全部可翻译块。
- `base_selected`：实际 base artifact 的 selected 表（已验证）。
- `new_selected`：所有已校验 Trace 中实际执行的 `aot`/`jit_compile`/`jit_exec` 块 RIP 并集。
- `promoted_blocks = sorted(new_selected - base_selected)`。
- `demoted_blocks = sorted(base_selected - new_selected)`；这些块是 base 有、本次未执行，因此不进入新 artifact，必须记录而不是伪装成 promoted。
- 最终 artifact 的 selected 表必须等于 `new_selected`；未执行块必须 `jit_only=true`（既有 include/exclude 规则）。

`promoted_blocks` 可以合法为空。空 `new_selected` 仍是无效输入；空 promoted 不等于空 selected。仅当 `base_selected == new_selected` 时 coverage 相同；base 为严格超集时按执行并集丢弃未执行块（`demoted_blocks` 非空）；new 为严格超集时 `promoted_blocks` 非空。

manifest 记录：`base_aot_sha256`、`base_selected_blocks`、`selected_blocks`（new）、`promoted_blocks`、`demoted_blocks`、`all_block_count`、input/entry/function/target/ABI，以及既有 trace 文件哈希。集合字段统一为排序、去重的整数 RIP（JSON number），不混用 hex 字符串。报告必须同时列出实际 base/new/promoted/demoted 集合。

## 4. Publisher 消费者

`publish_aot.py` 必须要求 `--base-aot <dylib>`，并重复实际 base artifact 的身份/表校验（与 §2 同一 helper，至少包括 input SHA/FNV、entry、function SHA、target/ABI、guest-state、`aev_aot_run`、all/selected 表与 selected `fn` 符号）：

- 校验待发布 dylib 的实际 selected/all 表、输入/entry/function/target/ABI、selected `fn` 符号；manifest `selected_blocks` 必须逐项等于实际新 dylib selected 表，`base_selected_blocks` 必须逐项等于实际 base selected 表。
- 要求 manifest `base_aot_sha256` 等于实际 base dylib SHA-256；要求 `promoted_blocks == sorted(selected_blocks - base_selected_blocks)` 且 `demoted_blocks == sorted(base_selected_blocks - selected_blocks)`；缺字段或不一致拒绝。
- 允许 `promoted_blocks=[]`；不得要求存在 promoted RIP。
- 可选 `--promoted-rip <hex>` 仅作为额外断言：若提供，必须同时满足“在 manifest `promoted_blocks` 中且在 selected 表中”；不能把只 selected 或已属于 base 的块冒充 promoted。
- Publisher 不重新解析 COFF 来证明 input/entry/function；它信任已校验 supplement manifest 的 COFF 身份，但必须校验 base 与 new 两个实际 dylib 的符号/表及 base provenance。COFF 非循环验证由 supplement 与独立 QA 负责；报告不得把 publisher 说成独立 COFF 验证器。
- 任一项失败不发布，旧 dylib/manifest 保持不变。

## 5. GUI 消费者

消费者清单：`spikes/p0-ui/harness/run_engine.py`、`engine_host.py`、`spikes/p0-ui/scripts/run_engine_gui.sh`、`spikes/p0-engine/scripts/publish_aot.py`、`trace_aot_test.py`、`run_trace_aot_test.sh`、相关 README/报告。

`run_engine`/`engine_host` 的 supplemented profile：

- 必须显式提供 `--base-aot <dylib>`（或由脚本传入 `AEV_BASE_AOT`），加载/校验实际 base artifact，并要求 manifest `base_aot_sha256`/`base_selected_blocks` 与实际 base 逐项一致。
- 要求 manifest `selected_blocks` 与实际新 artifact selected 表逐项一致；`len(selected_blocks)==aot_table_count`；artifact `input_sha256`、`entry_addr`、target/ABI、`all_block_count`、AOT SHA 与 manifest/COFF 一致。
- 要求 `promoted_blocks ⊆ selected_blocks` 且 `promoted_blocks == sorted(selected_blocks - base_selected_blocks)`；`demoted_blocks == sorted(base_selected_blocks - selected_blocks)`；缺字段/伪造 promoted/伪造 base provenance 必须失败。
- 允许空 promoted；空 promoted 不等于空 selected。GUI 断言 `aot_table_count` 与 selected 表，不要求存在新增块。
- `--promoted-rip` 默认未设置；仅当显式提供时，才要求它同时属于 manifest `promoted_blocks` 和实际 selected 表。不能把原本已 AOT 的块标成新增来满足断言。
- base profile 可省略 base；若显式提供 `--promoted-rip`，仍断言该 RIP 不在实际 selected 表。
- 仍须保持 base/supplemented 各 44 项、真实控件 helper-after-fault、base artifact 拒绝、forced native failure 和跨后端一致性。新增空 promoted 正例和伪 promoted 负例，不增加 scenario check 名称。

`trace_aot_test.py` 不得再用 `--promoted-rip`/`--expect-promoted-selected` 表示“在 selected 表”；改为 `--selected-rip`/`--expect-selected`，promotion 断言由 manifest/publisher/GUI 的 `promoted_blocks` 负责。

## 6. 验收回归

1. **全 AOT 再补充**：使用实际全五块 artifact 作 base，采集**该 artifact 上的新 Trace**（不得复用四块 trace），零 JIT；补充后 `base_selected=new_selected=all`，`promoted=[]`、`demoted=[]`；发布/新进程运行零 JIT、业务/counter/helper 正确。另做 GUI supplemented 全五块、空 promoted、无 `--promoted-rip` 的 44/44 正例。
2. **B-only → 新 A 分支**：B-only base selected 明确为 `{0x140001000,0x140001015,0x14000102f,0x140001038}`（include 1015、exclude 1026 构建）；执行 A+B 输入，A body `0x140001026` 真实 JIT；补充后 `promoted=[0x140001026]`，不得含 `0x140001015`，且 `0x140001015` 仍在 base selected；新进程 A body AOT、零对应 JIT、结果/副作用正确。
3. **默认首次提升**：callback 默认 base 4→5 只提升 `0x140001020`；trace fixture 首次 B 发现仍为 `0x140001015`。
4. **身份/混合基线负例**：缺 base、错误 base SHA/FNV/ABI/target/entry/function/guest-state、缺 `aev_aot_run`、Trace `aot_sha256` 不符、header selected 与实际 base 不符、不同 Trace base/coverage 混用、manifest `base_aot_sha256`/`base_selected_blocks` 伪造、selected 截断、伪 promoted（selected 但属于 base）全部失败；不生成完整输出、旧产物保留。
5. **执行并集不回退**：B-only trace 不保留未执行块；空 promoted 与空 selected 独立验证；严格超集时 `demoted_blocks` 正确。
6. 保留原 5 个 ctest、Trace/AOT 负例、base/supplemented × compat/native 四组 44/44、100 次稳定性、真实控件故障不提交/恢复、无 Unicorn/回退。

## 7. 会签

| 角色 | 结论 | 证据 |
| --- | --- | --- |
| compiler | CHANGES REQUIRED 已修订，待复签 | ses_e515087ebffeC2uMH8UyWTirHI |
| binary | CHANGES REQUIRED 已修订，待复签 | ses_e515087e9ffe78klWhkhdHuS2D |
| platform | CHANGES REQUIRED 已修订，待复签 | ses_e515087e8ffeKDJ5NEvC6f45vJ |
| reviewer | CHANGES REQUIRED 已修订，待复核 | ses_e515087dcffe6Tu8uHzJUkUkyE |
