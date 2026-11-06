# P0-TRACE-AOT-01：实际执行 Trace → 离线补充 AOT → 新进程验证

状态：完成（原 `adf9a95` QA/reviewer PASS 为历史；F1 已由 P0-PROMOTION-FIX-01/source `66a63fb` 修复并通过独立 QA/reviewer；物理 GUI/IME 仍待人工验收；P0 整体未通过）。

## 输入与责任

- 阶段、需求 ID、前置任务：P0 收尾；R01/R03/R06/R08；前置 P0-CALLBACK-INTEGRATION-01（已接受）。
- owner / reviewer / QA：coordinator / reviewer / qa；compiler（lead）、binary（contributor）、platform（GUI 回归 contributor）。
- 基线 commit 或源码包哈希：`4bb69abee6239416956204c80da1e93ef95489d0`（文档 HEAD，源码 `afff569be1c3399241b3bc1edb609d8a5a584f3d`）；main `c91a3f7` 未合入。
- 分支 / worktree / Windows 工作目录：`p0/trace-aot-01` / `.worktrees/p0-trace-aot`；Windows `C:\Users\PeterWind\AevotraceP0\P0-TRACE-AOT-01`（仅在需要时创建）。
- 公共 ABI / artifact / 平台接口版本：callback ABI v2 保持不变；新增 trace/artifact v1、translator ABI 1；Guest ABI 仍 v1。

## 目标与范围

- 可观察的完成结果：
  1. 从实际 Guest 执行采集结构化记录，绑定输入 COFF SHA-256、函数/入口、块字节身份、AOT/JIT 来源、间接目标、HostImport 与错误；不是手填轨迹。
  2. 离线补充工具读取记录和输入，用共享 translator/emitter 生成新的 AOT artifact/manifest；不复制 dispatcher，不在 Mac 重编业务 C 源码。
  3. **新进程**加载新 artifact 执行准备集 A：被 JIT 发现的 IMUL 块实际 AOT 执行，A 不再为该块 JIT；业务/helper/counter 一致。
  4. 独立集 B 使用全新分支/目标：未在 A 出现，首次真实 JIT；离线补充后新进程 B 的该块 AOT 执行。
  5. input/entry/function/block bytes/all+selected AOT table/target/translator ABI/artifact 错配拒绝；旧 base artifact 不覆写。
  6. compat/native 用补充后 callback artifact 复跑同一 GUI 输入，42 个原有名称（helper-after-fault 4 个同名升级）+ 1 个同名替换（`loop_100_jit_executed`→`loop_100_jit_policy`）+ 1 个新增 `aot_table_count` = 44/44，100 次与错误路径不回归；Windows 用同一固定对象做行为对照。
- 允许修改的文件或组件：
  - `spikes/p0-engine/`：translator/AOT emitter、JitRuntime 只读 hook、bridge trace API、AOT 身份/块表、supplement 工具、测试/fixture/build。
  - `spikes/p0-ui/harness/` 与 `scripts/`：仅 engine 路径的 artifact 参数/断言、GUI 回归；不改 legacy Unicorn 业务。
  - `docs/`、`docs/tasks/`、测试输入与证据。
- 明确不包含的内容：通用 PE loader、全部 Win32、真实应用、线程/模态/任意重入、并发自修改/运行中失效、持久化 JIT 缓存产品化、独立 `.app`/签名/公证、性能优化或提速结论。不得用关闭/重开进程冒充 live invalidation。
- 需要的依赖及许可记录：LLVM/Capstone/CommonCrypto（macOS 系统）；无新增第三方二进制；所有新 fixture 自编、MSVC `/Brepro`。

## 验收

- 测试程序、输入、准备集和独立验证集：
  - A：现有 callback fixture（`4f6929f7...`），trace 中 IMUL 块由真实 JIT 编译/执行；离线 artifact 提升后新进程 AOT，`aot_table_count=5`、`jit_compiles=0`。
  - B：新增有界 `aev_trace_fixture`（同一 Windows COFF 对象），A 路径不执行 B 分支；B 输入首次 JIT，补充后新进程 AOT。Windows 参考链接同一对象并输出 A/B；提交 B 源码/对象/driver/构建脚本/stdout/反汇编/manifest。
  - 负例：SHA-256/entry/function/block bytes/AOT all/selected table/target/translator ABI/runtime/version 错配，trace 截断/损坏/失败 call_end，均为明确失败且旧产物保留。
- Windows 参考执行步骤：`ssh win` 编译同一 fixture 对象 + driver；输出 A/B 固定结果；记录对象/驱动 SHA-256、命令/退出码。
- macOS 目标执行步骤：fresh cmake/ninja；base trace 进程 → supplement → 新进程；ctest 4 组原回归 + 新 trace/AOT 测试；双 GUI 使用补充后 callback artifact。
- 预期业务输出、状态或副作用：A/B 结果与 Windows reference 一致；helper/counter/返回值一致；新进程 promoted 块 AOT、`jit_compiles` 对该块为 0；未覆盖合法目标仍能 JIT。
- GUI 自动/人工部分：compat/native 各跑 base 与 supplemented 两个 coverage profile，各 44/44（42 个原有名称，其中 helper-after-fault 4 个同名升级；1 个同名替换 `loop_100_jit_executed`→`loop_100_jit_policy`；1 个新增 `aot_table_count`）。base: aot_table_count=4/total_jit=1/jit_execs>0；supplemented: aot_table_count=5/total_jit=0/jit_execs=0/jit_compiles=0。supplemented 必须绑定 supplement manifest、AOT SHA、`aot_table_count=5` 和 promoted RIP `0x140001020`；传 base artifact 必须失败。含真实按钮 → helper 成功 → Guest 故障 → `backend.get_text` 实际控件文本不变 → 恢复；GUI artifact 拒绝负例必须 exit 1/无提交/无 compat 回退；人工键鼠/焦点/IME 待验收。
- 必须证明真实生效的路径：trace 来自 bridge/AOT/JIT 实际执行点；AOT/JIT 实际 ARM64 执行；新 artifact 在独立进程加载；无 Unicorn/Host 业务重写。

## 完成报告

- 提交或 diff：契约 `1f1fe00`；实现 `8325b64`（37 files, +1959/−46）；reviewer/QA 阻断修复 `823ede4`、`603a246`、`9c2aafe`。阶段报告 `docs/p0/TRACE-AOT-REPORT-20261106.md`。
- 命令、退出码、环境及配置：
  - `cmake -S spikes/p0-engine -B .local/build-trace-aot-final -G Ninja` exit 0；`cmake --build` exit 0（46/46）。
  - `ctest --test-dir .local/build-trace-aot-final --output-on-failure` exit 0，5/5 PASS（原 4 组 + `p0_trace_aot`）。
  - `bash spikes/p0-engine/scripts/run_trace_aot_test.sh ...` exit 0，`ALL TRACE/AOT CHECKS PASSED`。
  - 最终 GUI base：compat/native 各 44/44，`aot_table_count=4`、`total_jit_compiles=1`、`jit_execs=920`。
  - 最终 GUI supplemented：compat/native 各 44/44，`aot_table_count=5`、`total_jit_compiles=0`、`jit_execs=0`；base artifact 负例 exit 1、compat 事件 0。
  - Windows `ssh win` 同一对象构建运行 exit 0，A=3922、B=45320、`BUILD_RUN_OK`。
- 二进制哈希、日志和报告位置：
  - callback `4f6929f7…`；trace fixture `145800f4…`；实现侧最终构建 supplemental callback `e4661369…`、trace `463ac067…`、B-only `d28ee0f7…`、no-JIT `1014d327…`；独立 QA 在 `adf9a95` 重新构建的 AOT SHA 见 `QA-final3/REPORT.md`。
  - `.local/results/P0-TRACE-AOT-01/{ctest-final3.log,final3-trace-test.log,final3-gui-base.log,final3-gui-supp.log,final3-gui-summary.json,final3-hashes.txt,windows-trace-fixture-final.log}`。
- 正确性/性能结果：A、B 两轮均从真实 JIT 发现进入新进程 AOT；B-only/no-JIT 精确 selected 与 publisher/bridge 负例通过；独立 QA final3 为 48 PASS/0 FAIL、597/77 独立校验、GUI 44/44；未做性能测量。
- 审查问题及修复：契约经 compiler/binary/platform 三轮会签与 reviewer 复核；已修复块 offset/size、target/translator 标识、HostImport 仅 id、失败 trace 拒绝、GUI 44/44 profile 与真实控件断言。reviewer 在 `74fc21e` 的 trace 字段/调用序列/事件身份/发布原子性/selected 表绑定/负例缺口，以及 QA 在 `74fc21e` 的 `aot_sha256`/call_end 字段/JSON 错误处理缺口，已在 `823ede4`/`603a246` 修复并新增回归负例；需对新 HEAD 独立复跑。复核第二轮又修复 selected 精确执行并集/B-only/no-JIT、bool/float 类型、publisher 全表/fn/符号校验、`aev_block_count` 越界；Windows reference 现在始终链接提交的固定对象并打印 `COMMITTED_OBJECT_BUILD_RUN_OK`。
- 未测项、已知限制、需要用户操作的具体步骤：见阶段报告 §5；GUI 物理键鼠/焦点/IME 仍需人工验收；未做通用 PE、真实应用、`.app`、运行中失效或性能结论。QA final3 报告：`.local/results/P0-TRACE-AOT-01/QA-final3/REPORT.md`。
- 后续任务与接口变更：trace/artifact ABI v1、translator ABI 1；callback ABI v2 不变。独立 QA/reviewer 已通过；仍不合并 main、不 push、不删除 worktree。
- 不宣称 P0/首版完成；不自动合入 main/push/删除 worktree。
