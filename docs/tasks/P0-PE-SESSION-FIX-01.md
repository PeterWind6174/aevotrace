# P0-PE-SESSION-FIX-01：PE session observer 生命周期与 HostImport arity 修复

状态：完成（F1/F2 实施、独立 QA、reviewer 与外部复核均通过；源码 `47b2b7a` 已经 `ce9c11b` 统一到 main）。收口与历史工作树归档见 [合并记录](../p0/INTEGRATION-20261107.md)。下文派发/实施过程中的分支和“停在集成分支”约定为历史记录，已由用户本次统一合并授权取代。

## 输入与责任

- 阶段/需求：P0 高风险验证收口；R01/R04/R06/R08；P0-03/P0-04 的 PE session 正确性分项。外部复核 `CODEX-PE-GUI-REVIEW-20261107` 的 F1/P1 与 F2/P2。
- 基线：`5e79398902eedf4ee2e369136fa9ee1a460eea10`（`p0/pe-gui-01`，工作树干净）；最终产品源码 `c5adf4fc26c3f360c9931d3833aa45d96eedcd7a`，两者之间只有文档变化。main 仍 `c91a3f7`（旧版本），不从 main 开始实现。
- 本轮集成分支/worktree：`p0/pe-session-fix-01` / `.worktrees/p0-pe-session-fix-01`。
- 契约：[PE-SESSION-CONTRACT-20261107](../p0/PE-SESSION-CONTRACT-20261107.md) v1 rev2；header `spikes/p0-engine/include/aev_pe_session.h` 不变。
- 既有 ABI：Guest 2/296、callback 3、artifact/translator 2、trace sink 1、PE trace file 4、`AEV_PE_SESSION_ABI_VERSION=1`；本轮不改变编号/布局/结构尺寸。
- owner：coordinator（契约/任务卡/公共 CMake/集成/报告）；platform（`spikes/p0-ui/harness/pe_session.py` 与 wrapper 专项测试，F1）；compiler（`spikes/p0-engine/src/pe_session_core.cpp` 与 session C ABI/runner 专项测试，F2）；QA（独立最终构建/回归）；reviewer（只读）。
- 模型：所有主控/实施/QA/审查会话必须核对 OpenCode Go / DeepSeek V4.1 Flash / Max；只使用桌面端原生会话/子 agent，不执行 `opencode run` 或 CLI 模型 worker；worker 不递归派生。
- 实际会话（`session_v2` 只读查询，`opencode-go / deepseek-v4.1-flash / variant=max`）：
  - 主控 `ses_e4b311c2cffdJYcN88U4skhtsd`（coordinator）。
  - platform `ses_e4b29fd0affeYDDEEG7Rw0dI3U`，已 `session_move` 到 `.worktrees/p0-pe-session-fix-platform`。
  - compiler `ses_e4b29fd07ffee99y5B2I62Wmj4`，已 `session_move` 到 `.worktrees/p0-pe-session-fix-compiler`。
  - QA `ses_e4b07d921ffeon7hWLWRyNsk8C`，已 `session_move` 到 `.worktrees/p0-pe-session-fix-qa`（detached 最终提交）。
  - reviewer `ses_e4ad18ddfffevkf0ofqugF2BeK`，已 `session_move` 到 `.worktrees/p0-pe-session-fix-review`（只读）。

## 并行隔离

| 任务 | 分支/worktree | 构建/证据 | 文件 owner | 状态 |
| --- | --- | --- | --- | --- |
| 契约/集成 | `p0/pe-session-fix-01` / `.worktrees/p0-pe-session-fix-01` | `.local/build-pe-session-fix-int`；`.local/results/P0-PE-SESSION-FIX-01/integration/` | 契约、任务卡、公共 `CMakeLists.txt`、报告、证据索引 | 完成；最终源码 `47b2b7a` |
| F2 compiler | `p0/pe-session-fix-compiler-01` / `.worktrees/p0-pe-session-fix-compiler` | `.local/build-pe-session-fix-compiler`；`.local/results/P0-PE-SESSION-FIX-01/compiler/` | `spikes/p0-engine/src/pe_session_core.cpp`、`spikes/p0-engine/scripts/pe_session_test.py`（必要时 `pe_dll_test.py` runner 负例） | 完成 `26c5cf7`，已合入 `2228cba` |
| F1 platform | `p0/pe-session-fix-platform-01` / `.worktrees/p0-pe-session-fix-platform` | `.local/build-pe-session-fix-platform`；`.local/results/P0-PE-SESSION-FIX-01/platform/` | `spikes/p0-ui/harness/pe_session.py`、新增 `spikes/p0-ui/scripts/pe_session_wrapper_test.py` / `run_pe_session_wrapper_test.sh` | 完成 `a4186bd`，已合入 `188f97d` |
| QA | `.worktrees/p0-pe-session-fix-qa`（detached 最终提交） | 独立 `.local/build-pe-session-fix-qa`；`.local/results/P0-PE-SESSION-FIX-01/qa/` | 只读最终源码，不修改产品文件 | **PASS（自动验证，无阻断）** |
| reviewer | 独立只读会话 | 无构建/无写入 | 只读最终提交 | **无阻断；7 项非阻断（见报告）** |

同一 ABI/CMake/dispatcher/公共构建文件单 owner；coordinator 最后顺序集成并注册新的 wrapper ctest。platform 不修改 engine 公共文件，compiler 不修改 wrapper；QA/reviewer 不修改源码。

## 实施与集成记录

| 部分 | 提交/分支 | 结果 |
| --- | --- | --- |
| 契约 rev2/任务卡 | `8e06355`（`p0/pe-session-fix-01`） | 冻结 F1/F2 错误分类、所有权与验收 |
| F2 compiler | `26c5cf7`（`p0/pe-session-fix-compiler-01`） | 显式实现描述 + id/args/ABI 校验；基线新增测试 241/267（26 fail），修复后 267/267；原 233 项通过 |
| F1 platform | `a4186bd`（`p0/pe-session-fix-platform-01`） | 候选局部构建、成功后才替换；真实 wrapper 基线 12 checks/2 fail，修复后 436/436 |
| coordinator 顺序集成 | `2228cba`、`188f97d`（`p0/pe-session-fix-01`） | F2 先合入并独立构建 ctest 7/7；F1 后合入并注册 `p0_pe_session_wrapper`，ctest 8/8、wrapper 436/436 |
| 最终源码提交 | `47b2b7a`（`p0/pe-session-fix-01`） | QA 绑定此提交全新构建：ctest 8/8、session 267/267、wrapper 436/436、PE 149/149、语义 791/0、Trace/AOT 78 |
| 独立 QA | `ses_e4b07d921ffeon7hWLWRyNsk8C` | **PASS（自动验证，无阻断）**；F1 probe 99/99、F2 probe 40/40；GUI base 560/561→supp 596/597 零 JIT；COFF 264/264 |
| reviewer | `ses_e4ad18ddfffevkf0ofqugF2BeK` | **无阻断**；7 项非阻断（证据索引/任务卡/契约会签/模型记录、F1 真实 stale token、F2 module/symbol 别名、COFF base 口径） |

## QA/reviewer 结论摘要

- QA 在 `47b2b7a` 上全新构建：configure/build/ctest rc 0，8/8；`p0_pe_session` 267/267（原 233 项用基线测试文件独立复跑 233/233）；wrapper 436/436；PE 149/149；语义 791/0；Trace/AOT 78。F1 基线旧 wrapper 12 checks/2 fail，修复后 436/436 + QA probe 99/99；F2 基线 args=1 被接受并实际执行，修复后 40/40。GUI base 560/561、supplemented 596/597、load_count=1、AOT 2534/零 JIT；COFF 264/264。Windows 本轮未重跑，复用哈希绑定 reference。
- reviewer 只读审查 `47b2b7a`：F1/F2 修复与契约 rev2 一致，未发现阻断；测试使用真实 wrapper/dylib/DLL 且基线失败可信。非阻断项：证据索引/任务卡/契约会签/模型记录需同步；F1 缺真实 C stale token 端到端测试；F2 未强制固定 `module!symbol`（契约只要求 id/args/ABI）；COFF base 组使用本轮重建 AOT（行为等价）。

## 冻结的错误分类与所有权契约（F2/F1）

- F2 绑定：固定实现描述为 `aevhost.dll!AevUpdate`，id=1、args=2、ABI=`win64-u64`。绑定时验证声明 id/args/ABI 与实现描述一致；不匹配 fail closed。
- F2 分类：registry 文件解析失败（含非 `win64-u64` ABI）仍为 `LOAD=3`（既有 parser 防护，保留负例）；解析合法但无匹配实现（未知 ID 或签名不匹配）为 session `UNKNOWN_IMPORT=5`，诊断包含声明与期望签名；runner 的 create/load 任何失败保持既有 `status=3 LOAD` 和退出码。
- F2 禁止：忽略参数、补默认值、假返回值、改写 DLL 业务、扩大 HostImport 支持集。
- F1：wrapper 先将新 observer/callback/context 作为局部候选构建；旧 `_observer`/`_observer_callbacks` 强引用保持到 C ABI 明确成功替换、清除或成功 destroy。C ABI 返回错误、候选构造异常或 handle 校验异常时，旧 wrapper 绑定与 C 注册保持一致。
- F1 禁止：吞掉 `WRONG_THREAD=13`/`REENTRY=10`、修改 C 层 owner/reentry 策略、靠禁用 observer 解决、把旧 callback 转存到无限增长的保活列表。
- 所有权：C ABI/core 由 compiler 单 owner；Python wrapper 由 platform 单 owner；公共 CMake 与契约由 coordinator 单 owner，按顺序集成。

## 目标与范围

可观察结果：

1. F1：非 owner 线程替换/清除 observer 返回 13 后，旧 Python 回调仍被 wrapper 强引用，回 owner 后真实 Guest call 仍进入旧回调；新回调不生效。
2. F1：在 `on_call_end`/`on_host_import` 内替换/清除返回 10，旧绑定与业务结果/计数不变；成功替换仅新回调运行、成功清除无回调；成功 destroy/重复创建/清除/gc 压力下无保活泄漏或悬空指针。
3. F2：同一真实 `aevguest.dll` 在 registry 声明 args=0/1/3/4 时 create 失败 `UNKNOWN_IMPORT=5`（诊断含签名），args=2 正常；未知 ID 仍拒绝；失败不泄漏 slot/半 session，随后有效 session 可创建；runner 映射 `LOAD=3`。
4. 原 7 个 ctest、session 原 233、PE 原 149、语义 791、Trace/AOT 78 断言不缩减；新增测试只增不减。
5. 最终源码提交上独立 QA 复跑 compat/native base → 实际 GUI Trace → 离线 supplement → 新进程 supplemented，以及原六组 COFF GUI、forced native failure。

允许修改：上述 owner 文件；`docs/p0/PE-SESSION-CONTRACT-20261107.md`、本任务卡、本轮报告/证据索引、`docs/p0/EVIDENCE-INDEX.md` 追加、engine 公共 `CMakeLists.txt`（仅 coordinator 注册 ctest）。

明确不包含：通用 EXE/进程启动、完整 Win32/消息泵、TLS/CRT/SEH/COM、真实第三方应用、运行中代码失效/重装、跨 base artifact 复用、`.app` 打包/签名/公证、性能优化；不改 ABI 编号/布局；不修 `parse_exports` 排序（已排序）、长导出名拒绝、C ABI 错误优先级、trace `entry_exports` 排序等 backlog。

## 验收

### F1（platform 实施，QA 独立复验）

使用真正的 `harness.pe_session.PeSession` + 实际构建的 `libaev_pe_session.dylib` + 固定 `aevguest.dll`：

1. 非 owner 的替换、清除均返回 13；旧回调弱引用存活且 wrapper 仍持有强引用；回到 owner 后 `AevDllCompute` 仍进入旧回调，新回调未生效。
2. 在 `on_call_end` 与 `on_host_import` 内尝试替换/清除均返回 10；旧绑定保持，后续 Guest 调用正常且业务结果不变。
3. 成功替换后仅新回调运行；成功清除后无回调；成功 destroy、重复创建/清除、gc 压力下无保活泄漏或悬空指针。
4. 覆盖只有 `on_call_end`、只有 `on_host_import`、两者都有三种配置；异常路径不改变业务结果或计数。
5. 测试纳入常规 headless 回归（新增 ctest），不只放 `.local` 手工 probe；测试不能用额外变量长期保留旧 callback 来掩盖 wrapper 丢失引用。

### F2（compiler 实施，QA 独立复验）

同一真实固定 DLL、JIT-only、load base `0x180000000`：

1. registry args=0/1/3/4 均 create 失败 `UNKNOWN_IMPORT=5`，诊断含期望 `args=2` 与实际声明；不执行 Guest、不产生 HostImport 调用。
2. args=2 正常 create/call，业务结果与独立 oracle/Windows reference 一致。
3. 未知 ID 仍拒绝为 `UNKNOWN_IMPORT=5`（不得改成 LOAD 或吞掉）。
4. 非 `win64-u64` ABI 负例仍由 registry parser 拒绝，create 返回 `LOAD=3`（保留既有防护）。
5. 失败不泄漏 session slot/半 session；连续 8 个失败后仍能创建有效 session，随后有效 session 可正常 compute。
6. `aev-pe-runner` 对上述签名不匹配仍输出 `status=3`/`status_name=LOAD` 和既有退出码；`--expect-error` 行为不变。
7. 固定 DLL/helper 二进制与 registry 合法内容不变；AOT artifact 身份/绑定哈希语义不变。

### 回归（QA 独立构建最终提交）

- 原 7 个 ctest 全部通过；`p0_pe_session` 在原 233 项基础上只增不减；`p0_pe_dll` 149、语义 791、Trace/AOT 78 保持；新增 wrapper ctest 通过。
- compat/native base GUI（真实 JIT）→ 实际 GUI Trace → 离线 supplement → 新进程 supplemented：load_count=1、100 次按钮状态、错误后恢复、2534 AOT/零 JIT 等按实际结果核对；变化需解释。
- 原六组 COFF GUI 相关回归；forced native failure 无 compat 回退。
- Windows reference：若仅 wrapper/Host 绑定变更且固定 DLL/helper 未变，可复用已绑定哈希的 reference 并明确“本轮未重跑”；Guest 输入/预期业务/Windows 参考代码变化时须 `ssh win` 重跑并绑定哈希。
- 真实机器码执行、共享 PE core、C ABI 薄层、两种 GUI 后端保持；不得引入 runner 子进程桥、Unicorn 或 Host 业务副本。

### 证据

- F1/F2 测试须在旧源码（5e79398）上可靠失败、修复后通过；记录确切版本、命令、退出码、配置和日志。
- 实施一份证据（`.local/results/P0-PE-SESSION-FIX-01/{compiler,platform}/`），QA 一份证据（`.../qa/`）；不删除/覆盖已有轮次证据或 worktree。
- 记录 DLL/dylib/manifest/trace/AOT 哈希与所有实际会话模型元数据；不声称检验不可见的 provider 请求。

## 完成报告

- 提交/diff、分支/worktree、实际模型会话记录。
- 命令、退出码、环境、配置、构建目录。
- 旧版本失败/修复后结果；DLL/dylib/manifest/trace 哈希；块表。
- 独立 QA/reviewer 结论与未测项。
- 停在本轮集成分支，供外部复核；不自行合 main/push/清理工作树/开启下一轮。
