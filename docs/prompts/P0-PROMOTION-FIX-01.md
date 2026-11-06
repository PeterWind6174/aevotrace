# 给 aevotrace-coordinator：修正后续轮次的 AOT 提升来源

你继续担任 Aevotrace coordinator。本轮只执行 P0-PROMOTION-FIX-01，修复外部审查发现的 promoted_blocks 基线错误、完成独立 QA/reviewer 和报告，然后停止供外部复核。

## 1. 输入与版本

- 先读 AGENTS.md、四份项目必读文档和 docs/OpenCode弹性调度.md。
- 审查报告：`/Users/peterwind/Script/Systems/Aevotrace/.local/results/CODEX-TRACE-AOT-REVIEW-20261106/REPORT.md`。
- 复现证据：同目录 `promotion-provenance.json`、`repeat-all/`、`from-B-only/`、`independent.py`、`all.jsonl`、`B-only.jsonl`。先实际复现，不只接受文字结论。
- 待修复分支 `p0/trace-aot-01` / `p0/integration-trace-aot-01`，HEAD `d0ddff8fbdf846798a034df78a675e5eca2d6444`，最终源码 `9c2aafe9fe93369bf18679c0a4407bebded3af0a`。main 仍是旧 `c91a3f7`，不含 callback 与 Trace/AOT 集成；从明确的待修复版本创建本轮独立分支/worktree，不从旧 main 开始。
- 根目录新增/更新的 `.opencode/agents/aevotrace-coordinator.md`、`docs/OpenCode弹性调度.md`、`docs/OpenCode协作方案.md` 和本提示词是用户最新调度授权。先检查实际 diff，把这些范围内的文件变更带入本轮工作分支；它们可能尚未提交，不能以工作树内旧副本否定最新授权，不拷贝其他未审查修改或整份旧 main docs 覆盖新分支。
- 使用 TASK_TEMPLATE 创建任务，更新上一轮状态为“外部复核发现 F1，待修复”，保留原 QA/reviewer 的历史版本结论。

## 2. 修复目标与契约

`scripts/aot_supplement.py` 当前从新 translator 的默认 coverage 推导 base_selected，导致真实基线为已补充/自定义 coverage 时 promoted_blocks 错误。必须先由 coordinator 协调 compiler/binary/platform 对来源与消费者的最小契约更新，再改实现。

本轮优先采用**单一明确、经验证的实际 base artifact**：

1. new_selected 仍为已校验 Trace 中实际执行事件的并集，不擅自并入默认或旧 coverage。
2. base_selected 来自实际 base artifact 的 selected 表，并验证输入/入口/函数/ABI/目标等与本次输入一致，以及 Trace 的 aot_sha256/aot_selected 与该实际 artifact 对应。不能只相信未与实际产物核对的 header 或 manifest，也不能仅比较 AOT 文件 SHA 后仍使用默认覆盖。
3. promoted_blocks = new_selected - base_selected。可以合法为空；报告/manifest 记录实际基线身份、覆盖和差集语义。
4. 对缺少基线、多个不同基线/coverage 的 Trace，明确拒绝或定义显式模式；本轮可限定单基线并清晰拒绝多基线，不为这次修复引入完整增量合并系统。不得静默猜测默认 baseline。
5. 检查 publisher、GUI、测试和报告中的消费者，区分“某块已在 selected 表内”与“本轮新提升”。支持空 promoted 的合法发布/验证，不得给已经 AOT 的块贴上新提升标签来满足 --promoted-rip 等断言。CLI 如需调整，更新所有调用方和契约。
6. 复用现有 identity/table 校验，必要时抽取小型公共 helper；避免再复制一套校验器或扩大重构范围。保持无 Host 业务重写、无 Unicorn、机器码真实转换执行。

允许修改：supplement/publisher 及必要的共享身份校验、测试/构建调用、相关 GUI 消费者与任务/契约/报告。排除 PE、Win32、新 ISA、打包、性能、运行中失效和其他独立功能。

## 3. 必须验收

1. **全 AOT 再补充**：实际运行已覆盖全部五块的 artifact，零 JIT；使用该实际 artifact 作基线再补充，promoted_blocks 必须 `[]`。新产物在新进程中业务、counter/helper 正确，零 JIT；publisher 不得要求伪造一个 promotion。
2. **B-only → 新 A 分支**：B-only 基线没有 `0x140001026`，先由真实执行触发 A body 的 JIT，再补充。promoted_blocks 恰为 `[0x140001026]`，不能含原本已 AOT 的 B 块 `0x140001015`。新进程中 A body 实际 AOT，零对应 JIT，结果/副作用正确。
3. **首次默认基线回归**：callback 的默认基线到补充仍只提升 `0x140001020`；trace fixture 初次 B 发现仍正确记录 `0x140001015`。
4. **身份负例**：错误 base 输入/版本、Trace SHA 不符、伪造 header selected、受支持范围外的混合基线明确失败，不生成完整输出、不破坏旧产物。
5. **执行并集不回退**：新 selected 仍等于实际执行并集，不为修正 promoted 而保留未执行的默认块；空 promoted 不等于空 selected，两个概念独立验证。
6. 保留原 5 个 ctest 与 Trace/AOT 负例；base/supplemented × compat/native 四组 GUI 各44项（如必要调整列明对应关系，不删弱断言）。保留实际结果控件故障不提交、恢复、无 Unicorn/跨后端回退与100次稳定性。
7. QA 使用指定最终源码提交、新构建/结果目录和自己的准备集外输入。Windows 参考仅在 Guest fixture/机器码/ABI 改动时重新构建；没有此类改动可说明沿用已验证固定对象对照，不无意义重测。

## 4. 会话与调度

只通过 OpenCode 桌面端/原生会话，核对每个实际会话使用 Go / DeepSeek V4.1 Flash / Max；不启动 opencode run/CLI 模型 worker，不改全局账号/密钥。

用户已授权主控按独立任务需要增开现有角色会话，无需再次请示：当前六类角色足够本次任务。建议一个 compiler 实施者，binary 会签，独立 QA 和只读 reviewer；QA 可按行为/产物分两个会话。常态最多两个实施任务，满足契约/文件owner/独立worktree/build/cache/资源条件才扩至最多四个；此单点修复无需用满。worker 不递归派生，主控统一调度。桌面模型/隔离/额度不满足时减少并发或串行，不转 CLI 或换模型。

## 5. 交付与停止

提供代码提交、最小契约变更、三类正例的真实 base/new/promoted 集合、输入/Trace/AOT/manifest SHA-256、实际命令/退出码、负例与旧产物保留证据、全套必要回归和最终源码版本上的独立 QA/reviewer 结论。明确剩余限制，不能以此次修复宣布 P0/首版完成。

完成后停在本轮集成分支供外部复核；不自行合入 main、push、删除或归档工作树，不自动启动 PE 等后续实现。
