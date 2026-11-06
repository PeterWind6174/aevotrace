# P0-PROMOTION-FIX-01：修正 AOT 提升来源与 promoted_blocks 语义

状态：完成（source `66a63fb`；QA PASS、reviewer SIGN-OFF；物理 GUI/IME 待人工验收；P0 整体未通过）。

## 输入与责任

- 阶段、需求 ID、前置任务：P0 收尾；R06/R03；前置 P0-TRACE-AOT-01（外部复核发现 F1）。
- owner / reviewer / QA：coordinator / reviewer / qa；compiler（实施 lead）、binary（基线身份会签）、platform（GUI 消费者会签）。
- 基线 commit 或源码包哈希：`d0ddff8fbdf846798a034df78a675e5eca2d6444`（源码 `9c2aafe9fe93369bf18679c0a4407bebded3af0a`）；已接受 callback 基线 `4bb69ab`；main `c91a3f7` 未合入。
- 分支 / worktree / Windows 工作目录：`p0/promotion-fix-01` / `.worktrees/p0-promotion-fix`；Windows 目录仅在 fixture/机器码/ABI 变化时使用。
- 公共 ABI / artifact / 平台接口版本：callback ABI v2、Guest ABI v1 不变；trace/artifact ABI v1 的 provenance 最小修订（v1.1），不引入 live invalidation 或新 artifact 格式。

## 目标与范围

- 可观察的完成结果：
  1. `aot_supplement.py` 的 `base_selected` 来自实际 `--base-aot` artifact 的 selected 表，并与输入/入口/函数/target/ABI、Trace header `aot_sha256`/`aot_selected` 核对；不再从默认 translator coverage 猜测。
  2. `promoted_blocks = sorted(new_selected - base_selected)`，允许空；`new_selected` 仍等于已校验 Trace 实际执行事件并集，最终 artifact selected 等于 `new_selected`。
  3. publisher、GUI、测试和报告区分“已选入 AOT”与“本轮新增”，支持空 promoted 的合法发布/验证；不得用假 promoted RIP 满足断言。
  4. 缺 `--base-aot`、错误 base artifact、Trace 与实际 base 不符、混合不同 base/coverage 的输入明确失败；旧产物保留。
  5. 保持默认首次提升（callback `0x140001020`、trace fixture `0x140001015`）、身份负例、执行并集规则、5 个 ctest、base/supplemented × compat/native 四组 GUI 各 44 项。
- 允许修改的文件或组件：`spikes/p0-engine/scripts/{aot_supplement.py,publish_aot.py,trace_aot_test.py,run_trace_aot_test.sh}`、必要的共享身份/table 小 helper、`spikes/p0-ui/harness/{run_engine.py,engine_host.py}`、相关 README/contract/task/report。
- 明确不包含：PE/Win32、新 ISA、packaging、性能、live invalidation、真实应用、线程/模态/重入；不改执行并集选择规则来掩盖 metadata 错误。
- 需要的依赖及许可记录：仅现有 LLVM/Capstone/CommonCrypto 与现有测试工具；无新增第三方二进制。

## 验收

- 测试程序、输入、准备集和独立验证集：
  - 全 AOT 再补充：实际全五块 artifact 为 base，Trace 实际执行全部/无 JIT；`promoted_blocks=[]`，新进程业务/counter/helper 正确、零 JIT。
  - B-only → 新 A 分支：B-only artifact（selected 4，无 `0x140001026`）执行 A+B 输入，A body 首次真实 JIT；补充后 `promoted=[0x140001026]`，不含已 AOT 的 `0x140001015`；新进程 A body AOT、零对应 JIT、结果/副作用正确。
  - 默认首次提升：callback base 4→5 只提升 `0x140001020`；trace fixture 首次 B 发现仍为 `0x140001015`。
  - 负例：缺 base artifact、错误 base 输入/SHA/ABI/target/entry/function、Trace `aot_sha256` 不符、header selected 与实际 base 不符、多 Trace 不同 base/coverage、publisher manifest 伪造 promoted；明确失败、无完整输出、旧产物保留。
  - 执行并集：B-only trace 不保留未执行块；空 promoted 与空 selected 独立验证。
- Windows 参考执行步骤：本轮 fixture/机器码/ABI 不变，可沿用已验证固定对象参考；不无意义重编。若改动则重新构建并记录哈希。
- macOS 目标执行步骤：全新 cmake/ninja；CTest 5/5；trace/AOT 脚本；四组 GUI；publisher 实际 dylib 校验。
- 预期业务输出、状态或副作用：A/B 与 Windows 固定对象参考一致；counter/helper/结果一致；promoted 集合与 base/new 差集一致。
- GUI 自动/人工部分：base/supplemented × compat/native 各 44/44；真实控件 helper-after-fault、base artifact 拒绝、forced native failure、无 Unicorn/回退；物理键鼠/焦点/IME 仍待人工。
- 必须证明真实生效的路径：Trace 来自实际 AOT/JIT 执行点；base selected 来自实际 dylib；new selected 来自实际执行并集；新 artifact 在独立进程加载执行。

## 完成报告

- 提交或 diff：基线 `d0ddff8`/源码 `9c2aafe`；契约 `22bc429`/`ce4431a`/`f3b4260`；实现 `2b9587c`、`17ec718`、`fc39482`、`b88cadf`、`66a63fb`（最终源码 HEAD）。阶段报告 `docs/p0/PROMOTION-FIX-REPORT-20261106.md`。
- 命令、退出码、环境及配置：实现侧 build rc 0（46/46）、CTest 5/5、trace/AOT 78 PASS/0 FAIL；独立 QA 全新 `.local/build-promotion-fix-qa` configure/build rc 0、CTest 5/5（35.91s）、trace/AOT 78 PASS/0 FAIL、36/36 对抗负例；GUI base/supplemented/空 promoted × compat/native 各 44/44；Windows 固定参考复用 A=3922/B=45320/EXIT=0。
- 二进制哈希、日志和报告位置：输入 callback `4f6929f7…`、trace `145800f4…`；QA 独立 build bridge `61fb1c04…`；实际 base/new artifact 哈希与集合见阶段报告 §5；QA 报告 `.local/results/P0-PROMOTION-FIX-01/QA/REPORT.md`；实现证据 `.local/results/P0-PROMOTION-FIX-01/impl/final2/`。
- 正确性/性能结果：callback 默认 promoted `[0x140001020]`；trace 首次 B `[0x140001015]`；B-only→A+B promoted 仅 `[0x140001026]`；全 AOT 再补充 promoted/demoted `[]`；demoted 场景 `[0x140001015]`；新进程业务/counter/helper 正确、零 JIT。未做性能测量。
- 审查问题及修复：F1 由外部审查发现并在基线复现；v1.1 经 compiler/binary/platform/reviewer 会签，修复后再经独立 QA PASS 与 reviewer accepted。
- 未测项、已知限制、需要用户操作的具体步骤：见阶段报告 §7；物理键鼠/焦点/IME、真实应用、PE/Win32、打包/签名、live invalidation、性能未做。
- 后续任务与接口变更：trace/artifact ABI 1 字段布局不变；manifest 增加 base/new/promoted/demoted provenance；`--base-aot` 成为 supplement/publisher/GUI supplemented 必需；callback ABI v2、Guest ABI v1 不变。不自动合入 main/push/删除 worktree。
