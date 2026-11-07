# P0-PE-GUI-01 最小 PE session 契约（v1）

日期：2026-11-07。状态：实施冻结候选；rev1 吸收契约只读审查 B1–B7 的 ABI/语义澄清（不改变结构尺寸/状态码编号）。适用范围：一个固定 Windows x86-64 PE32+ DLL（含已声明 HostImport）在 macOS ARM64 共享 AOT/JIT 核心中的持续存活调用会话，供 `aev-pe-runner` 与 compat/native GUI 共同使用。不是 P1 全量 ABI，也不覆盖 EXE 启动、DllMain/CRT/TLS、SEH、完整 Win32、通用依赖或并发重装。

本契约只新增 session 层；既有 Guest ABI 2 / 296、callback ABI 3、artifact/translator ABI 2、trace sink ABI 1、PE trace file ABI 4 与 [PE-DLL v3](PE-DLL-CONTRACT-20261106.md) 的模块身份、内存、HostImport、artifact 校验保持不变。

## 1. 文件与版本

| 项 | 值 |
| --- | --- |
| 公共头 | `spikes/p0-engine/include/aev_pe_session.h` |
| `AEV_PE_SESSION_ABI_VERSION` | 1 |
| `AevPeSession` | `uint64_t` generation token；0 无效 |
| 最大参数 | 4（RCX/RDX/R8/R9 原始 64 位位模式） |
| 状态码 | 0 OK；2..14 见 §2 |
| 共享实现 | `src/pe_session_core.{hpp,cpp}`（模块/Program/JIT/AOT/HostImport/Trace/生命周期） |
| C ABI 薄包装 | `src/pe_session.cpp` → `libaev_pe_session.dylib` |
| CLI 消费者 | `src/pe_runner.cpp`（薄 JSON/退出码适配） |
| GUI 消费者 | `spikes/p0-ui/harness/pe_session.py` + `pe_host.py`（ctypes） |

`struct_size`/`abi_version` 规则：

- 输入结构（`AevPeSessionConfig`、`AevPeSessionObserver`）必须精确设置 `struct_size = sizeof(...)` 与 `abi_version = 1`；不匹配返回 `ABI_MISMATCH`。`flags`/保留字段非零返回 `INVALID_ARGUMENT`。
- 输出结构（`AevPeCallResult`/`AevPeModuleInfo`/`AevPeSessionCounts`/`AevPeExportInfo`/`AevPeBlockInfo`）由调用方预填 `struct_size = sizeof(...)` 与 `abi_version = 1`；库检查尺寸并覆盖 `abi_version`，尺寸不匹配返回 `ABI_MISMATCH`。零值或错误尺寸不被接受。
- `AevPeSessionAbiInfo` 是唯一例外：`struct_size` 可为 0（库填写自身尺寸）或精确值；其他值返回 `ABI_MISMATCH`。
- `err` 可为 NULL、`err_cap` 可为 0（不写缓冲）；非空时始终 NUL 终止，超长截断。成功调用清空错误缓冲。

## 2. 状态码与错误分类

```c
0  OK
2  INVALID_ARGUMENT
3  LOAD
4  ABI_MISMATCH
5  UNKNOWN_IMPORT
6  MEMORY
7  JIT
8  STEP_LIMIT
9  STALE_HANDLE
10 REENTRY
11 HOST
12 INTERNAL
13 WRONG_THREAD
14 TRACE
```

`create`/`load` 分类：

| 情况 | 状态 |
| --- | --- |
| 空指针/缺 load base 且无 AOT/有 AOT 但缺 entry export/非零保留字段 | `INVALID_ARGUMENT` |
| DLL 读取、PE 解析、映射、重定位、节权限、registry 文件、导出/`.pdata`/函数发现、Program 构建失败 | `LOAD` |
| AOT 文件缺失/不可读/`dlopen` 失败、文件哈希读取失败 | `LOAD` |
| AOT 已加载但 module kind/base/file/image/import/host/entry exports/thunk/all-selected 表/ABI/guest state 任一不匹配、旧 ABI、AOT 必需符号缺失 | `ABI_MISMATCH` |
| PE import 未在 registry 声明、registry 声明了无 Host 实现的 ID | `UNKNOWN_IMPORT` |
| JIT runtime 初始化失败 | `JIT` |
| create 时 trace 文件无法打开/写 header 失败 | `TRACE` |

runner 为保持既有 CLI 契约，对 session create/load 的**任何**失败（含 make/加载 AOT、AOT identity、未知 import、JIT init、trace open）统一以 `status=3 LOAD`、`status_name=LOAD` 和 `error` 文本输出；不把 4/5/6/7/14 泄漏到 runner JSON。`--expect-error` 仍返回 0，非 expect 返回 3。session ABI 向 C/GUI 消费者提供上述更细分类。runner 的既有 JSON 字段、`--reload`、stale handle 行为和退出码保持不变；既有 error 子串（`unknown import`、`ABI`、`identity`、`relocation`、`executable`、`AMD64`、`W+X`）保留。

`call` 分类：Guest `error_code` 按既有 `map_guest_status` 映射；未知导出/非法导出名 `INVALID_ARGUMENT`；未进入 Guest 的 reentry/stale/wrong-thread 分别返回对应状态；`error` 同时给出可读细节。失败结果不得被 GUI 提交为业务结果。

装载期拒绝（W+X/RWX、越界/重叠节、缺 `.pdata`、错误 relocation、非 executable export 等）属于 `LOAD`；`prot=0` 的 section 是合法装载，访问时在运行期产生 `MEMORY`/`mem_fault_*`，不得在装载期隐式提升为可读。

## 3. 生命周期、handle 与 owner 线程

1. `create → load → resolve imports/exports → JIT init → trace open → call N 次 → destroy`。
2. 每个 session 固定 owner 线程（create 调用线程；GUI 为 AppKit 主线程）。`info/counts/export/selected/block/call/trace/destroy/set_observer` 在非 owner 线程调用返回 `WRONG_THREAD`。
3. handle 为 `(slot+1) | (generation << 32)`，slot 0..7；generation 从 1 开始、回绕跳过 0。destroy 后旧 token 返回 `STALE_HANDLE`，不解引用已释放对象；不得使用裸指针作为 handle。slot 满时 `create` 返回 `INVALID_ARGUMENT`。`create` 的进程级 slot 表线程安全；每个 session 的后续 API 限 owner 线程。
4. 活动 call 中，同一 owner 线程再次 `call`、`destroy`、`trace_*`、`set_observer` 返回 `REENTRY`；只读查询（`info/counts/export/selected/all/block`）允许并读取快照/稳定元数据。HostImport/observer 重入同样如此。不同线程的调用先返回 `WRONG_THREAD`。
5. `set_observer` 拷贝 `struct_size/abi_version/两个函数指针/ctx`；调用方结构可临时，但函数指针和 ctx 必须保持到 `set_observer(NULL)` 或 destroy。`on_call_end` 的 result 指针仅在回调期间有效；`in_call` 持续到两个回调返回，因此回调内 destroy/call/trace/set_observer 均为 `REENTRY`。C++ 异常由库捕获；其他语言回调不得把自身异常传播过 C ABI。
6. 两个 session 相互隔离：独立模块镜像、Region、JIT cache、HostImport 计数和 trace；销毁一个不影响另一个。
7. 部分初始化失败不产生半 session；所有本次 Host backing/dylib/JIT/registry/trace 在失败路径释放。destroy 关闭 trace、释放模块/JIT/AOT dylib；重复 destroy 返回 `STALE_HANDLE`。
8. 不承诺运行中 reload/reset。GUI 的 Reset 是显式调用 `AevDllReset` 业务导出，不是 session reload；新窗口/新进程创建新 session，按 DLL 静态初始化状态开始。

## 4. 参数、返回与 Guest 内存

- `args[4]` 是 RCX/RDX/R8/R9 原始 64 位寄存器值。调用方对 `int` 参数按低 32 位零扩展到 64 位；Guest 的 32 位运算只使用低 32 位。
- `rax` 是原始 64 位 RAX；`rax_i32` 是低 32 位按有符号 `int32_t` 解释。GUI 业务结果显示以 `rax_i32` 为准，不能把 `0xFFFFFFFF...` 的高位当作不同业务结果。需要指针/64 位结果时使用 `rax` 并明确标注。
- Guest VA / Host backing / Host 函数地址继续分离。GUI 只能通过 session 导出调用访问 Guest 状态，不直接解引用 Guest 数值；`AevDllReadAt/WriteAt/CallPtr` 等导出仍经同一 PE 内存模型。
- `AevPeCallResult` 的 `mem_fault_*`、`error_pc`、`guest_error`、`steps`、JIT/AOT 计数与既有 runner 语义一致；`call_index` 是该 session 的累计 Guest 调用序号（1 起，成功进入 Guest 才增加；未进入为 0），**不因 trace open/close 重置**。trace 文件的 `call` 字段是独立序号，在每次 `trace_open` 后从 1 起。`total_*` 包含当前调用；失败调用不写 `rax` 语义（结果字段只作诊断）。
- `info.entry_export`/`export_info.name` 是 NUL 终止字符串；`AEV_PE_SESSION_MAX_NAME=512` 包含 NUL，超过 511 字节的 PE 名称在查询输出中截断，`create` 的 `entry_export` 必须能在 511 字节内表示。`module_kind`/`target_triple` 同样 NUL 终止并在固定缓冲内截断；当前固定模块类型/目标均远小于上限。
- 累计计数在 session 内：`calls_total` 只统计进入 Guest 的调用；`host_import_calls` 统计实际 HostImport 调用；`aot/jit_blocks/compiles/execs` 为累计值。`load_count` 每个 session 成功 load 后为 1，GUI 用它证明按钮操作没有偷偷重载。
- HostImport 继续使用 registry 声明的固定实现（当前 `aevhost.dll!AevUpdate`，id=1，args=2），不扩展为任意 Windows API。observer 只观察，不改变参数/返回值。

## 5. Trace 语义

- `create` 的 `trace_path` 非空等价于 load 后立即 `trace_open`；`scenario` 为 header 标签，空/NULL 默认 `"A"`。
- `trace_open` 截断文件、写完整 PE trace ABI 4 header（与 `aot_supplement.py` 的 `validate_header` 相同：`scenario/input_sha256/input_fnv/module_file_size/preferred_base/load_base/image_size/image_sha256/import_binding_sha256/host_binding_sha256/entry_exports_sha256/entry_exports/entry/entry_addr/thunk_base/import_count/offset_kind/guest_abi/aot_artifact_abi/translator_abi/callback_abi/target/translator_version/trace_tool_version/aot_sha256/all_blocks/aot_selected`），并把 call index 清零；call 编号仍为 1 起。
- 只有实际进入 Guest 的调用才写该 call 的块/host_import/call_end 事件；入口前失败（非法导出、stale、wrong-thread、reentry、sentinel/shadow 写入失败）不写 `call:0` 或重复 `call_end`。`call_end.status` 为 session 状态；失败调用如实记录。
- 用于 `aot_supplement.py` 的 trace 必须只含成功调用（该工具拒绝 `call_end.status != 0`）。GUI/测试若需要同一 session 的负例，应在 `trace_close()` 之后再执行负例，或用独立 session 采集干净 trace；不得把负例 trace 冒充补充输入。
- `trace_close` flush 并关闭；已关闭时返回 OK（幂等），不改变业务状态。trace 打开时再次 open 返回 `INVALID_ARGUMENT`。destroy 自动 close。flush 写入错误不属于 Guest 业务结果；调用方以 trace 文件完整性和 header/call_end 校验为准。
- trace 事件来自该 session 的实际 dispatcher/JIT/AOT 与 HostImport，不使用 Host 影子记录。Trace 只写 Guest VA/模块身份，不写 Host backing/函数地址。
- 重新 open 后 call index 从 1 开始；旧文件内容被截断。调用方需在打开期间完成采集。

## 6. 查询接口

`aev_pe_session_info` 返回模块身份（file/image/import/host/entry exports SHA、preferred/load base、image size、thunk base、import count、relocs_stripped、entry VA、stack/entry_rsp、region count、all/selected count、AOT present/SHA、load_count、module kind、entry export、target triple）。不返回 Host backing 地址。

`export_count/export_info/find_export` 返回 named export 的 name/RVA/VA/ordinal；ordinal-only export 不进入该列表（与 PE 契约一致）。

`selected_count/selected_rip` 读实际加载的 AOT dylib selected 表；`all_block_count/all_block_rip` 读实际 Program 块；`block_info` 按 Guest RIP 返回 `block_identity` 的 RVA/size/SHA 与 selected 标志。所有查询只读，不改变调用计数。

`counts` 返回 §4 的累计计数和 trace 状态；`trace_records` 为当前/最近一次 trace 已写**事件**数（不含 header；关闭后保留最近值，重新 open 清零）。这些计数由 session 核心维护，不是 GUI 影子计数。`trace_records` 是 stream 写入计数，文件可能仍在缓冲；比较文件行数前必须 `trace_close`/flush。活动 call 中的只读查询返回当时快照；不得在 GUI 中用 `calls_total`/`call_index` 代替 DLL 的 `AevDllCalls` 业务计数。

## 7. 共享执行组件边界

- 抽取到 `pe_session_core` 的内容必须包括：PE `Module` 装载/身份、寄存器区域表、`Program`/块身份、AOT dylib 校验、JIT runtime/dispatcher、HostImport 实现、TraceWriter、run_call、计数和销毁。
- `aev-pe-runner` 只保留 CLI 解析、JSON 格式化、dump 选项、`--reload`/`--expect-error` 和退出码映射；不得保留第二份 loader/dispatcher/JIT cache。
- `libaev_pe_session.dylib` 只做 C ABI 类型/错误缓冲/owner/generation 检查；不得复制执行逻辑。
- GUI 不得在每次按钮时启动 runner 进程、解析 runner JSON 作为执行桥、重写 DLL 业务，或使用 Unicorn/Rosetta 兜底。测试脚本可以调用 CLI；产品 GUI 必须进程内调用 session 库。

## 8. GUI 语义（platform）

- 一个窗口创建一个 session，持有到窗口关闭；控件至少包括业务参数输入（a/b/n、seed）、Compute、明确 Reset、结果、Guest State、Guest Calls、错误/状态显示。可增加只使用已冻结导出的负例控件（如 `AevDllReadAt` 非法地址）。
- 只有输入解析、调度、格式化和展示在 Host；Compute/Reset/State/Calls 的业务结果和全局状态来自实际 DLL 导出调用。State/Calls 不得用 Host 影子计数代替。
- 非法数字/越界整数先在 Host 拒绝，显示明确诊断且不进入 Guest。PE 调用失败时保留上次有效业务结果/状态显示，错误单独可见；成功后才提交新结果。
- 两个后端使用同一 DLL SHA、registry、load base、AOT、session 库和操作序列；compat 为独立自绘语义，native 为真实 AppKit 控件，不得静默切换。闭窗 destroy session；重开新 session 按 DLL 静态状态初始化。
- 至少一条错误经真实按钮/控件事件进入 PE session（例如 `AevDllReadAt(0xDEAD0000)`），日志标明测试专用错误参数；失败后下一次正常 Compute 必须按声明策略恢复。

## 9. 验收映射

| 要求 | 证据 |
| --- | --- |
| 同一持久实例 100 次按钮 | GUI 场景 clicks=100，session `load_count==1`，DLL Calls/State 递增，结果与独立 oracle/Windows stateful reference 一致 |
| 3+ 输入含独立输入 | Reset + 至少 3 组 Compute（含准备集外输入），每组 DLL State/Calls/结果对照 |
| base JIT | base profile trace 含实际 `jit_compile`/`jit_exec`；业务结果与 AOT 一致；flags 由既有语义测试覆盖，不由 session C ABI 暴露 `rflags` |
| 负例与补充 trace 分离 | base 负例在 `trace_close` 后执行或使用独立 session；用于 supplement 的 trace 只含成功调用，`call_end.status` 全 0 |
| Trace 补充 → 新进程 AOT | 同一 DLL/输入的 supplement artifact 新 GUI 进程零 JIT，promoted 块有 `aot` 事件且无 JIT |
| 故障不提交 | 真实按钮触发 MEMORY，结果/State/Calls 保持；后续正常操作恢复 |
| session 隔离/lifecycle | headless C ABI：两个 session、destroy/stale、wrong-thread、reentry、部分加载失败、权限故障 |
| 双后端 | compat/native 同库/DLL/artifact/操作；native 控件类/主线程证明；forced native failure 无 compat 回退 |
| 回归 | 原 6 ctest、PE 147、语义 791、Trace/AOT 78、六组 COFF GUI 44/44 保持；新 PE GUI 独立场景/oracle |

## 10. 明确不包含

通用 EXE/进程启动、完整 Win32/消息泵、任意嵌套/线程/模态、TLS/CRT/SEH、COM、通用依赖图、真实第三方应用、运行中代码失效/重装、同一 artifact 跨 base、持久 JIT 缓存产品化、独立 app 分发/公证/性能优化。边界外必须明确拒绝，不能以占位返回值掩盖。

## 11. 会签

| 角色 | 结论 | 证据 |
| --- | --- | --- |
| coordinator | 冻结 rev1（吸收审查 B1–B7 澄清）并完成集成；最终源码 `c5adf4f` | 本文件、`docs/tasks/P0-PE-GUI-01.md`、`docs/p0/P0-PE-GUI-01-REPORT-20261107.md` |
| compiler | 实现 `a37c836`/报告 `4f467b3`；coordinator 修正 AOT IO/ABI、slot full、step-limit，最终 `p0_pe_session` 233/233 | `.local/results/P0-PE-GUI-01/compiler/`、ctest |
| binary | 模块身份/装载语义会签通过（`ee468f2`） | `reference_stateful_stdout.txt`、Windows 构建 |
| platform | 实现 `3ee254a`/`950e38f`/`9fd4a67`；coordinator 补充 trace/negative 分离与实际 artifact 校验；最终 GUI QA PASS | `.local/results/P0-PE-GUI-01/gui-*`、QA 报告 |
| reviewer | 契约审查 CONDITIONAL（B1–B7）；最终实现审查**无阻断、可进入外部复核**（6 项非阻断见主报告） | `ses_e4cba4b71ffeVeld29oUOOSto8`、`ses_e4c388f23ffe1VV6V0hVDaksY5` |
