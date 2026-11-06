# P0-PROMOTION-FIX-01：AOT 提升来源最小契约（Trace/artifact provenance v1.1）

日期：2026-11-06。状态：待 compiler/binary/platform 会签、reviewer 检查。只修订 P0-TRACE-AOT-01 的 baseline/`promoted_blocks` 语义，不改变 trace/artifact ABI 1 的字段布局、执行并集规则或产品范围。

## 1. 问题与范围

P0-TRACE-AOT-01 的 `aot_supplement.py` 从新 translator 的默认 coverage 推导 `base_selected`，未使用实际 `--base-aot` 的 selected 表。因此在“已全 AOT 再补充”时错误报告 `0x140001015` 为新增，在 B-only baseline 时也错误报告 B 块，而不是实际新增的 A body `0x140001026`。

本修订只修复 provenance/消费者语义。`new_selected` 仍由已校验 Trace 的实际执行事件并集决定；不把默认/旧 coverage 擅自并入，也不为修正 metadata 改变执行并集。

## 2. 实际 base artifact 是权威基线

`aot_supplement.py` 必须要求单一明确的 `--base-aot <dylib>`（不再静默使用默认 coverage）：

1. 加载实际 dylib，读取并校验 `aev_input_sha256`、`aev_entry_addr`、`aev_function_name_sha256`、`aev_target_triple`、`aev_aot_abi_version`、`aev_artifact_abi_version`、`aev_translator_abi_version`、`aev_all_block_count`/`aev_all_blocks`、`aev_block_count`/`aev_blocks`，以及每个 selected `fn` 与 `aev_blk_<rip>` 符号相等。
2. 实际 base 的输入 SHA/entry/function/target/ABI 必须与本次输入和 translator 结果一致；all-block 表必须与 `Program` 输入一致；selected 表必须排序、去重、为 all 表子集、每项 `fn` 与符号一致。
3. Trace header 的 `aot_sha256` 必须等于实际 base dylib 的 SHA-256；header `aot_selected` 必须与实际 base selected 表**逐项相等**，不能只检查“排序子集”。
4. 所有 Trace 必须引用同一个 base SHA 和同一个 selected 表；不同 base/coverage 的 Trace 混合输入明确拒绝。本轮不实现多基线合并。
5. 未提供 `--base-aot`、base artifact 缺失/不可加载、任一身份/表不匹配，必须在生成输出前失败；不覆写旧 artifact。

## 3. 集合语义

定义：

- `all_selected`：输入 COFF/translator 的全部可翻译块。
- `base_selected`：实际 base artifact 的 selected 表（已验证）。
- `new_selected`：所有已校验 Trace 中实际执行的 `aot`/`jit_compile`/`jit_exec` 块 RIP 并集。
- `promoted_blocks = sorted(new_selected - base_selected)`。
- 最终 artifact 的 selected 表必须等于 `new_selected`；未执行块必须 `jit_only=true`（既有 include/exclude 规则）。

`promoted_blocks` 可以合法为空：当 base 已经覆盖本次执行并集时，输出仍是同 coverage 的合法 artifact，但不得标记任何块为“本轮新增”。`selected_blocks` 与 `promoted_blocks` 是不同字段；消费者不得用 selected 表成员替代 promoted 证据。

manifest 记录：`base_aot_sha256`、`base_selected_blocks`、`selected_blocks`（new）、`promoted_blocks`、`all_block_count`、input/entry/function/target/ABI，以及既有 trace 文件哈希。报告必须同时列出实际 base/new/promoted 三个集合。

## 4. Publisher 消费者

`publish_aot.py` 必须要求 `--base-aot <dylib>` 并重复实际 base artifact 的身份/表校验：

- 校验待发布 dylib 的实际 selected/all 表、输入/entry/function/target/ABI、selected `fn` 符号。
- 从 manifest 读取 `selected_blocks`、`promoted_blocks`，要求它们与实际 dylib/实际 base 的差集一致：
  `promoted_blocks == sorted(selected_blocks - base_selected)`。
- 允许 `promoted_blocks=[]`；不得要求存在 promoted RIP。
- 可选 `--promoted-rip <hex>` 仅作为额外断言：若提供，必须同时满足“在 promoted_blocks 中且在 selected 表中”；不能用只 selected 的块冒充 promoted。
- 任一项失败不发布，旧 dylib/manifest 保持不变。

## 5. GUI 消费者

`run_engine`/`engine_host` 的 supplemented profile：

- 从 supplement manifest 读取 `promoted_blocks`（可为空）和 `selected_blocks`，并用实际 `AevArtifactInfo`/selected 表交叉验证；每个 promoted RIP 必须在实际 selected 表中。
- 允许空 promoted；空 promoted 不等于空 selected。GUI 断言 `aot_table_count` 与 selected 表，不要求存在新增块。
- 若显式提供 `--promoted-rip`/`--expect-promoted-selected` 之类选项，只能用于“本轮新增”断言；不能把原本已 AOT 的块标成新增来满足断言。
- 仍须保持 base/supplemented 各 44 项、真实控件 helper-after-fault、base artifact 拒绝、forced native failure 和跨后端一致性。

## 6. 验收回归

1. 全 AOT artifact 再补充：`base_selected=new_selected=all`，`promoted=[]`，新进程零 JIT、结果/副作用正确。
2. B-only baseline（selected 4，无 A body）执行 A+B 输入：A body `0x140001026` 真实 JIT；补充后 `promoted=[0x140001026]`，不含已 AOT 的 `0x140001015`；新进程 A body AOT、零对应 JIT。
3. 默认首次 callback：`promoted=[0x140001020]`；trace fixture 首次 B：`promoted=[0x140001015]`。
4. 身份/混合基线负例：缺 base、错误 base SHA/ABI/target/entry/function、Trace aot_sha/selected 不符、不同 Trace base 混用、manifest promoted 伪造；全部失败且旧产物保留。
5. 原 5 个 ctest、trace/AOT 负例、base/supplemented × compat/native 四组 44/44、100 次稳定性、无 Unicorn/回退。

## 7. 会签

| 角色 | 结论 | 证据 |
| --- | --- | --- |
| compiler | 待会签 | |
| binary | 待会签 | |
| platform | 待会签 | |
| reviewer | 待检查 | |
