# P0-TRACE-AOT-01：实际执行 Trace → 离线补充 AOT → 新进程验证

状态：进行中（契约 v1 已获 compiler/binary/platform/reviewer 会签；实现进行中）。

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

- 提交或 diff、命令/退出码、哈希/日志、A/B 两轮 AOT/JIT 证据、Windows/双 GUI、QA/reviewer、未覆盖项、分支状态。
- 不宣称 P0/首版完成；不自动合入 main/push/删除 worktree。
