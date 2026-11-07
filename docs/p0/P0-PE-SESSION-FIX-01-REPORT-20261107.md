# P0-PE-SESSION-FIX-01 报告：F1 observer 生命周期与 F2 HostImport 签名校验

日期：2026-11-07。状态：F1/F2 实施、顺序集成、独立 QA PASS、reviewer 无阻断；停在集成分支待外部复核。分支 `p0/pe-session-fix-01`，最终源码提交 `47b2b7a86f15ee67274fcf613a352e38791ec140`；基线 `5e79398902eedf4ee2e369136fa9ee1a460eea10`（产品源码 `c5adf4fc26c3f360c9931d3833aa45d96eedcd7a`，两者仅文档差异）。main 仍 `c91a3f7`，未合并、未 push、未清理工作树。

本报告只证明：固定 Windows x64 PE32+ DLL 的 PE session 在 macOS ARM64 共享 AOT/JIT 核心中，Python wrapper 的 observer 失败路径不再释放 C 仍注册的回调；HostImport 绑定按固定实现描述校验 registry 声明的 id/args/ABI，签名不匹配 fail closed。不证明通用 EXE、完整 Win32、真实第三方应用、运行中失效、独立 `.app` 或性能。

## 1. 版本、契约与会话

| 项 | 值 |
| --- | --- |
| 集成分支/worktree | `p0/pe-session-fix-01` / `.worktrees/p0-pe-session-fix-01` |
| 最终源码提交（QA 绑定） | `47b2b7a86f15ee67274fcf613a352e38791ec140` |
| 基线 | `5e79398`（`p0/pe-gui-01`） |
| 契约 | `PE-SESSION-CONTRACT-20261107.md` v1 rev2（HostImport 签名匹配、wrapper observer 生命周期；ABI/结构/状态码编号不变） |
| 既有 ABI | Guest 2/296、callback 3、artifact/translator 2、trace sink 1、PE trace file 4、session ABI 1；未改 |
| 固定输入 | `aevguest.dll` `1f82b679…52b0`、`aevhost.dll` `0eabd013…57d1`、`aevimports.txt` `d268d412…4d15` |
| main | `c91a3f7`（旧版本，未使用） |

实际会话（只读 `session_v2.model`，均 `opencode-go / deepseek-v4.1-flash / variant=max`；仅会话配置层，不声称审计 provider 出站请求）：

- 主控 coordinator `ses_e4b311c2cffdJYcN88U4skhtsd`。
- platform `ses_e4b29fd0affeYDDEEG7Rw0dI3U`（`.worktrees/p0-pe-session-fix-platform`）。
- compiler `ses_e4b29fd07ffee99y5B2I62Wmj4`（`.worktrees/p0-pe-session-fix-compiler`）。
- QA `ses_e4b07d921ffeon7hWLWRyNsk8C`（`.worktrees/p0-pe-session-fix-qa`）。
- reviewer `ses_e4ad18ddfffevkf0ofqugF2BeK`（`.worktrees/p0-pe-session-fix-review`，只读）。

未执行 `opencode run` 或 CLI 模型 worker；worker 未派生下一层。

## 2. 外部复核与基线确认

外部复核 `CODEX-PE-GUI-REVIEW-20261107` 独立复跑原主链路：7/7 ctest、session 233、PE 149、语义 791、base GUI 560/561、supplemented 596/597；固定 DLL 真正进入共享核心，100 次按钮同一实例，补充后 2534 AOT 块/零 JIT。但报告 F1/P1（observer 失败路径悬空回调）与 F2/P2（HostImport 只匹配 ID，未匹配 args/ABI），因此原轮不能视为最终验收通过。

Coordinator 用外部复核的基线独立构建（`libaev_pe_session.dylib` `553caefe…f11216`）复跑两个 probe：

- F1：非 owner clear 返回 13；未 pin 时旧 Python 回调被回收（alive=false），wrapper callback 数 0；C 仍保留旧注册。
- F2：registry `aevhost.dll|AevUpdate|1|1|win64-u64` create/call 均为 0，实际执行两参数实现；非 `win64-u64` ABI 由 registry parser 正确拒绝为 `LOAD=3`。

证据：`.local/results/P0-PE-SESSION-FIX-01/integration/baseline/`。

## 3. F1（P1）：observer 失败路径的 Python 生命周期

实施：platform 提交 `a4186bd`（`spikes/p0-ui/harness/pe_session.py`，+17/-4）与新增测试 `pe_session_wrapper_test.py`/shell 入口。

修复要点：

- `set_observer` 先做 handle 校验，再把新 observer/两个 callback/callback 列表作为局部候选构建。
- 只有 `aev_pe_session_set_observer` 返回 OK 后才替换 `self._observer` / `self._observer_callbacks`；失败（13/10/ABI/参数/handle/候选构造异常）保持旧绑定与 C 注册一致。
- 成功替换/清除/destroy 后释放旧对象；无额外保活列表；不吞 `WRONG_THREAD`/`REENTRY`，未改 C 层策略。

基线/修复后（final 引擎 + 真实 wrapper/dylib/DLL）：

- 旧 wrapper：`12 checks, 10 passed / 2 failed`，rc 1；失败点 `rejected clear keeps old wrapper binding`（`wrapper_callbacks=0`），检测到引用丢失后只在 owner 安全 clear/destroy，不以悬空指针调用 Guest。
- 修复后：`436/436`，rc 0；覆盖三种配置（call_end/host_import/both）、非 owner replace/clear=13、回调内 replace/clear=10、成功 replace/clear/destroy、stale/double destroy、候选构造 TypeError、重复 create/clear 与 GC 压力；业务 Oracle/`calls_total`/`host_import_calls` 逐步断言。
- 外部 observer probe 修复版：失败 clear 13、旧回调 alive=true、wrapper callback 数 2。

证据：`platform/REPORT.md`、`platform/fixed.log`、`platform/baseline-final.log`、`integration/wrapper-baseline.log`、`integration/observer_probe_fixed.log`。

## 4. F2（P2）：HostImport 实现与声明签名一致

实施：compiler 提交 `26c5cf7`（`pe_session_core.cpp`，+56/-12；`pe_session_test.py`，+116）。

修复要点：

- 显式固定实现描述 `aevhost.dll!AevUpdate id=1 args=2 abi=win64-u64`。
- 绑定时按 id 查实现，并校验声明的 args/ABI；不匹配时 `cleanup()` 后返回 `UNKNOWN_IMPORT=5`，诊断含声明与期望签名。
- 未知 ID 仍 5；非 `win64-u64` 仍由 registry parser 在更早阶段拒绝为 `LOAD=3`；runner 映射仍 `status=3 LOAD`、退出码不变。
- 不忽略参数、不补默认值、不假返回、不改 DLL 业务、不扩支持集、不改 ABI。

基线/修复后（同一真实固定 DLL、load base `0x180000000`）：

- 基线新增测试 `241/267`（26 fail）：args=0/1/3/4 均 create 0；8 次失败均被接受；runner args=1 返回 0 并执行 Guest；带 trace 的失败 create 实际创建 trace。
- 修复后 `267/267`（原 233 项保留）；args=0/1/3/4 → 5、诊断含 `declared args=N abi=win64-u64` / `expected args=2 abi=win64-u64`；args=2 正常且 HostImport 计数 1；未知 ID 5；ABI 负例 3 LOAD；8 次失败后 8 个 slot 仍可用；runner 非 expect rc 3/`status=3 LOAD`、`--expect-error` rc 0。
- Coordinator 用外部 host_binding probe 在集成构建复核：args=1 → 5（诊断完整），args=2 正常，错误 ABI 仍 3。

证据：`compiler/REPORT.md`、`compiler/baseline-clean.log`、`compiler/fixed-session.log`、`integration/f2-probe.log`。

## 5. 集成构建与回归（coordinator）

- 构建 `.local/build-pe-session-fix-int`；configure/build rc 0；`ctest` **8/8 PASS**（原 7 + `p0_pe_session_wrapper`），约 45 秒。
- `p0_pe_session` **267/267**（原 233 保留）、`p0_pe_session_wrapper` **436/436**、`p0_pe_dll` **149/149**、语义 **791 PASS/0 FAIL**、Trace/AOT **78 PASS**；原 6 个 ctest 均通过。
- 原断言未缩减；新增 wrapper ctest 由公共 CMake 注册（coordinator 单 owner）。

证据：`integration/configure-int-final.log`、`build-int-final.log`、`ctest-int-final.log`、`integration/INTEGRATION.md`。

## 6. 独立 QA

QA 会话 `ses_e4b07d921ffeon7hWLWRyNsk8C`（`opencode-go / deepseek-v4.1-flash / max`），在 detached `47b2b7a` 的独立 worktree 中从空目录全新构建；报告 `.local/results/P0-PE-SESSION-FIX-01/qa/REPORT.md`。

| 项 | QA 实测 |
| --- | --- |
| configure/build/ctest | rc 0 / 0 / 0；**8/8 Passed**（49.51 s） |
| `p0_pe_session` | **267/267**；用基线 `pe_session_test.py` 对 final dylib 独立复跑 **233/233**（原断言未缩减） |
| `p0_pe_session_wrapper` | **436/436**，`completed_all_configs=true`，三种配置 |
| `p0_pe_dll` / 语义 / Trace-AOT | **149/149** / **791 PASS, 0 FAIL** / **78 PASS** |
| F1 独立 probe / 基线 | **99/99 PASS**；旧 wrapper **rc 1，12 checks/2 fail**（旧回调被释放） |
| F2 独立 probe / 基线 | **40/40 PASS**；args=0/1/3/4 → 5、args=2 正常、8 次失败不泄漏 slot、runner rc 3/LOAD；外部基线 args=1 create 0 并实际执行 |
| GUI base compat/native | **560/561 checks**；load_count=1、calls 323、host 105、JIT **21/2534**、AOT 0 |
| Supplement | selected **21/25**、promoted **21**、demoted **0**；supp dylib `35cfa64c…`，实际 artifact 表与 manifest 一致 |
| GUI supplemented compat/native | **596/597 checks**；load_count=1、AOT 2534、JIT **0/0**；21 个 promoted 块均有实际 `aot` 事件，无 JIT 事件 |
| 旧六组 COFF GUI | **264/264 checks**（每组每后端 44/44、4 截图）；base JIT 1/920，supp/empty 0/0；forced failure 无回退 |
| Windows | **本轮未重跑 `ssh win`**；testdata `SHA256SUMS` 全部通过，复用哈希绑定的 106 行 reference 对照 |
| 反替代/架构 | 进程内 session、无 runner 子进程桥；无 Unicorn/pefile/guest_pe；组件 thin arm64；native AppKit 类/主线程，compat 无 native 事件 |

QA 构建哈希（全新目录，与集成构建不同属 LC_UUID/调试路径差异；行为以 QA 实测为准）：`libaev_pe_session.dylib` `8b6e45c3…17b16`、`aev-pe-runner` `28cf700f…a656`。COFF base 组使用本轮 final 构建新生成的 AOT（`83f41f3a…`，与旧轮 `1f13f40c…` 因构建路径/LC_UUID 不同），44/44 检查、JIT 1/920、AOT 表与旧轮一致；supp/empty 复用旧 artifact。QA 结论：**自动验证 PASS，无阻断；人工 GUI 交互与 Windows 重跑未覆盖。**

## 7. 只读审查

reviewer 会话 `ses_e4ad18ddfffevkf0ofqugF2BeK`（同模型，只读；未运行测试）审查 `5e79398..47b2b7a` 与 QA/实施证据，结论：**F1/F2 修复与契约 rev2 一致，无阻断，可进入外部复核**。

非阻断项：

1. `EVIDENCE-INDEX` 原“见下文”缺本轮章节（本报告提交时已补）。
2. 任务卡状态落后于 final commit/QA（本报告提交时已更新）。
3. 契约 rev2 会签表停留上一轮（本报告提交时已补 rev2 会签）。
4. `models.json` 未记录 reviewer（已补 reviewer 会话元数据）。
5. F1 未端到端覆盖“真实 C stale token（slot 复用后的旧 token）调用 set_observer”；核心 13/10/TypeError 路径已覆盖，列入下一轮加固，不阻断。
6. F2 按契约只校验 id/args/ABI，未强制固定 `module!symbol`；若后续意图禁止 registry 别名，需在契约/测试明确，列入下一轮。
7. COFF base 组使用本轮重建 AOT（行为等价，非旧 artifact 逐字节复用）；本报告已明确口径。

reviewer 未审查范围：未运行构建/测试、未审计 provider 出站请求、未重跑 Windows、未做人工 GUI/ASAN/TSAN、未逐行审查整个 GUI harness/`pe_session.cpp`/`pe.cpp`。

## 8. 哈希与证据

关键 SHA-256（完整清单见 `integration/hashes-final.txt` 与各实施报告）：

```text
libaev_pe_session.dylib  45ca46eb812708ccb3a12715ca119e47170c81e07b678a6cc167c303d2ad3198
aev-pe-runner            6b86241e6881d87917461d81d44595157645a54dee0841bb02ac2c1118fd519b
pe_session.py            bdf9e4912c93a891be312b70304b0243f7a6f94ed11c747314e227413328fed5
wrapper test             256f6cfc23afe5eac3d0925f431c1c9c4fa25e570e81a9761f6f1b1b842d830e
pe_session_test.py       8fd516fd84a3b2a10c929aba81cd8008c594df45e504a2973f5b8a6389eb6f16
aevguest.dll             1f82b67978dc13e1e31c1262497a9fe186d39f637949b94408c646d4b11152b0
aevhost.dll              0eabd013127ef5cc4e73c149aa0988b3d88dbb009d491c0c895e7a0d479757d1
aevimports.txt           d268d41269412b5de5656779902e3cf99da1482c58ada49347f170589bfe4d15
```

证据根：`.local/results/P0-PE-SESSION-FIX-01/{integration,compiler,platform,qa}/`。

## 9. Windows 参考

本轮仅改 Python wrapper 与 HostImport 绑定校验；固定 `aevguest.dll`/`aevhost.dll`/`registry` 及 Guest 输入/预期业务未变。Windows stateful reference（`reference_stateful_stdout.txt` `5c74…`）本轮 **未重跑**，复用上一轮已绑定哈希的参考；QA 已核对 `testdata/pe/SHA256SUMS` 全部通过，并用 106 行 reference 对 reset + 4 组 Compute + 5 个采样 repeat 做状态对照。未声称新的 Windows 原生运行结果。

## 10. 未测与限制

- 人工物理键盘/焦点/IME/窗口缩放/观感仍待用户；自动截图/AppKit 事件不等于人工验收。
- 未实现/未验证：通用 EXE/进程启动、完整 Win32/消息泵、TLS/CRT/SEH/COM、真实第三方应用、运行中代码失效/重装、跨 base artifact、持久 JIT 缓存产品化、独立 `.app` 分发/签名/公证、性能结论。
- F1 未做 ASAN/TSAN 正式泄漏证明；基线悬空风险以“检测后不在 owner 安全 clear/destroy 前调用 Guest”验证，未声称观察到 SIGSEGV。
- F2 未测试把其他 `module!symbol` 别名映射到 id=1 的 registry；契约只要求 id/args/ABI 匹配，名称用于诊断。
- 未在磁盘满/写入中途失败、generation 2^32 回绕、并发非 owner destroy 与 owner 查询的极端竞争下验证。
- reviewer 非阻断 backlog（不借本轮扩大重构）：F1 真实 C stale token 的端到端 set_observer 测试；F2 固定 `module!symbol` 别名边界（当前契约只要求 id/args/ABI）；COFF base 组严格旧 artifact 复用；C ABI 错误优先级、长导出名拒绝、trace `entry_exports` 排序等既有 backlog。

## 11. 上一轮状态修正

原 `P0-PE-GUI-01` 报告的“reviewer 无阻断、可进入外部复核”是该轮 reviewer 结论；外部复核发现 F1/F2 后，旧版本不再视为最终验收通过。已在 `docs/p0/P0-PE-GUI-01-REPORT-20261107.md` 与 `docs/p0/EVIDENCE-INDEX.md` 增加后续状态说明，保留历史结果，不抹除。

## 12. 结束状态

停在 `p0/pe-session-fix-01` 集成分支供外部复核；未合 main、未 push、未删除/归档旧工作树、未开启下一轮。P0 整体/首版仍未完成。
