# P0-PROMOTION-FIX-01 阶段报告：AOT 提升来源与 promoted_blocks

日期：2026-11-06（Asia/Shanghai）。结论：**本轮有界修复通过独立 QA 与 reviewer；P0 整体/首版仍未通过。**

## 1. 范围与版本

| 项目 | 值 |
| --- | --- |
| 基线 | `d0ddff8fbdf846798a034df78a675e5eca2d6444`（源码 `9c2aafe`） |
| 契约 | `22bc429` / `ce4431a` / `f3b4260`（provenance v1.1；compiler/binary/platform/reviewer 会签） |
| 实现源码 HEAD | `66a63fb187655e9e2874d13135e67089a0fbec35` |
| 实现分支 / worktree | `p0/promotion-fix-01` / `.worktrees/p0-promotion-fix` |
| 集成分支 / QA worktree | `p0/promotion-fix-integration` / `.worktrees/p0-promotion-fix-qa` |
| main | `c91a3f7`（未合入、未 push） |

本修复只处理 `promoted_blocks` 的 baseline provenance，不扩展 PE/Win32、真实应用、packaging、性能或运行中失效。

## 2. F1 根因与修复

外部审查 F1 在基线独立复现：`aot_supplement.py` 从新 translator 的默认 coverage 推导 `base_selected`，没有读取实际 `--base-aot` 的 selected 表。

修复：

- `--base-aot` 为必需参数；共享 `aot_artifact.py` 直接读取/校验实际 base dylib 的 input SHA/FNV、entry、function SHA、target、ABI、guest-state size、`aev_aot_run`、all/selected 表及 selected `fn` 符号。
- 每个 Trace 的 `aot_sha256` 必须等于实际 base SHA，`aot_selected` 必须与实际 base selected 表逐项相等；不同 base/coverage 的 Trace 混用拒绝。
- `new_selected` 仍为实际执行事件并集；最终 artifact selected 精确等于 `new_selected`。
- `promoted_blocks = sorted(new_selected - base_selected)`，允许空；`demoted_blocks = sorted(base_selected - new_selected)`。空 promoted 与空 selected 独立校验。
- Publisher 用实际 base/new dylib 和 manifest `base_aot_sha256`/`base_selected_blocks`/`selected_blocks`/`promoted_blocks`/`demoted_blocks` 交叉校验；`--promoted-rip` 只在同时属于 promoted 与 selected 时接受。Publisher 明确信任 supplement 的 COFF 身份，不冒充独立 COFF 验证器。
- GUI supplemented 必须加载实际 `--base-aot`，校验 manifest base provenance、实际 selected 表、promoted/demoted 差集；空 promoted 合法，`--promoted-rip` 默认未设置且只在显式声明时检查。
- `trace_aot_test.py` 的 selected 断言改为 `--selected-rip/--expect-selected`，不再把“在 selected 表”当作 promotion 证据。

## 3. 真实 base/new/promoted/demoted 集合

以下集合均由实际 Trace/实际 dylib 产生；`0x` 简写，RIP 十进制在 manifest/QA 原始 JSON 中。

| 场景 | base selected | new selected | promoted | demoted |
| --- | --- | --- | --- | --- |
| callback 默认首次 | `{1000,101c,1037,1042}` | `{1000,101c,1020,1037,1042}` | `{1020}` | `{}` |
| trace fixture 首次 B | `{1000,1026,102f,1038}` | `{1000,1015,1026,102f,1038}` | `{1015}` | `{}` |
| B-only 基线 + A+B 执行 | `{1000,1015,102f,1038}` | `{1000,1015,1026,102f,1038}` | `{1026}` | `{}` |
| 全 AOT 再补充 | `{1000,1015,1026,102f,1038}` | 同左 | `{}` | `{}` |
| full-5 仅 A（demoted） | `{1000,1015,1026,102f,1038}` | `{1000,1026,102f,1038}` | `{}` | `{1015}` |

- B-only 的 A body `0x140001026` 先真实 JIT compile/exec；`0x140001015` 在 base 已 AOT，未出现在 promoted。
- 全 AOT 再补充使用在 full-five artifact 上重新采集的 Trace，未复用旧四块 Trace；promoted/demoted 均为空。
- 新进程结果：callback `3828036654`；trace fixture A=3922、B=45320；counter/helper 正确；full/B-only 新进程 promoted 块零 JIT。

## 4. 命令与退出码

实现侧：

```text
cmake --build .local/build-promotion-fix -j 6                         rc 0
ctest --test-dir .local/build-promotion-fix --output-on-failure       rc 0, 5/5
run_trace_aot_test.sh .local/build-promotion-fix <source>             rc 0, 78 PASS/0 FAIL
GUI base / supplemented / empty-promoted (compat+native)              rc 0, 44/44 each
```

独立 QA（全新构建 `.local/build-promotion-fix-qa`）：

```text
configure + build (46/46)                                             rc 0
ctest 5/5                                                             rc 0, 35.91s
run_trace_aot_test.sh                                                 rc 0, 78 PASS/0 FAIL
独立 artifact/表/负例/执行敏感性                                      PASS
GUI base/supplemented/full-5 empty-promoted × compat/native           44/44 each
Windows 固定参考（未重编）                                             A=3922, B=45320, EXIT=0
```

QA 报告：`.local/results/P0-PROMOTION-FIX-01/QA/REPORT.md`（独立 36/36 对抗矩阵、独立 dylib 表读取、旧 artifact sentinel 保留）。实现证据：`.local/results/P0-PROMOTION-FIX-01/impl/final2/`。

## 5. 关键 SHA-256

输入：callback COFF `4f6929f7c71c82169295b6675517a839ae9533a85941e2632115bf0ef47328dc`；trace COFF `145800f4a4729c201aab2c94a74937d349e01f6ec7871658b6becd3773bd1205`。

独立 QA 新构建（`.local/build-promotion-fix-qa`）实际 artifact：

| artifact | SHA-256 |
| --- | --- |
| bridge | `61fb1c044f4602d157b72fde8c572b6b1daf4fc0f9564f1f7015386bd948027f` |
| callback base | `73e9d747e8011b37c9570b4b24688cc35f6cd62ab06d3e2dff7217a2d9a1dcd6` |
| callback first supplement | `7fb6e4c3e3e5ce49627da57e97af4e4e4f7b0fd78a354e8e62beed3ba1138b1f` |
| callback full-5 re-supplement | `5cd108e8027fe1c97006407754c25b65bb51c03187491dc492f04170c081705f` |
| trace base | `efc6b4d571cbd97cfbc127e2820619d4e9139f51d741a08125dcb57603c84f47` |
| trace first A+B supplement | `954ded1039d0fca3777f8ce628d3b88201cb5e71009a6eafe1f777a74bc08ed6` |
| trace full-5 re-supplement | `f15c3716dac0f309bd564c879d007a5e7321ab7602e9abe845c577d481a519ae` |
| B-only base | `71fb3d54dbf2882afe02358e64f6b6d2d5f52322951ef68bb7377d7f2e3c2758` |
| B-only A+B supplement | `eb6ef8742c8755c376b349f31c9839a63884aed67f34d83564583fc36f1615b9` |
| demoted (仅 A) | `dbb15c272b4da76420bf159b1bc7335d6e159d103d51a4f74eb29b1c9a048470` |

AOT dylib 的 Mach-O UUID/link 临时路径可能使不同构建 SHA 不同；每个 manifest 绑定本次实际 AOT SHA，QA 报告记录独立构建的完整哈希。

## 6. 独立 QA / reviewer

- **QA**：`ses_e5131d24dffevGf652d2pdtC18`，`opencode-go / deepseek-v4.1-flash / max`；结论 **PASS，无阻断项**。独立复现两条 F1 回归、36/36 负例、空 promoted GUI、执行敏感性、旧产物 sentinel 保留。报告见上。
- **reviewer**：`ses_e5131d24affe6y0lgQgeDeEuvb`，只读源码复核；结论 **`SIGN-OFF: accepted`**。非阻断备注：任务/索引文档状态由 coordinator 收尾、README “原子发布”措辞、部分 base 身份字段负例可由 QA 补强、publisher 存在 base 文件 hash/load 的 TOCTOU 小窗口。

## 7. 未测项与限制

- Windows 仅复用已验证固定对象/参考 EXE；fixture/机器码/ABI 未改，因此未重编。物理键盘/焦点/IME/窗口外观未人工验收。
- 未做 PE/Win32 通用支持、真实第三方应用、`.app`/签名/公证、打包、运行中代码失效、性能结论。
- Publisher 对 COFF 身份显式信任 supplement manifest；COFF 非循环校验由 supplement 和独立 QA 负责。
- Publisher 输出是备份/回滚安全，不是崩溃原子。

## 8. 分支状态

停在 `p0/promotion-fix-01`（源码头 `66a63fb`，文档 HEAD 为包含本报告的提交）与集成分支 `p0/promotion-fix-integration`。未合并 main、未 push、未删除/归档 worktree。P0 整体与首版未完成。
