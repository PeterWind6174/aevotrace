# P0-PE-GUI-01 最小 PE session 契约（v1）

日期：2026-11-07。状态：实施冻结候选。适用范围：一个固定 Windows x86-64 PE32+ DLL（含已声明 HostImport）在 macOS ARM64 共享 AOT/JIT 核心中的持续存活调用会话，供 `aev-pe-runner` 与 compat/native GUI 共同使用。不是 P1 全量 ABI，也不覆盖 EXE 启动、DllMain/CRT/TLS、SEH、完整 Win32、通用依赖或并发重装。

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

所有 `struct_size` 必须等于本实现编译出的 `sizeof`；所有输出结构的 `abi_version` 由库填写，调用方传入的值被覆盖。`create` 的 `cfg->struct_size`/`cfg->abi_version` 必须精确匹配，否则 `ABI_MISMATCH`。`flags`/保留字段非零返回 `INVALID_ARGUMENT`。

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
| AOT 缺失/不可加载、module kind/base/file/image/import/host/entry exports/thunk/all-selected 表/ABI/guest state 任一不匹配、旧 ABI | `ABI_MISMATCH` |
| PE import 未在 registry 声明、registry 声明了无 Host 实现的 ID | `UNKNOWN_IMPORT` |
| JIT runtime 初始化失败 | `JIT` |
| create 时 trace 文件无法打开/写 header 失败 | `TRACE` |

runner 为保持既有 CLI 契约，装载前失败仍以 `status=3 LOAD` 和 `error` 文本输出；session ABI 向 C/GUI 消费者提供上述更细分类。runner 的既有 JSON 字段、`--expect-error`、`--reload`、stale handle 行为和退出码保持不变。

`call` 分类：Guest `error_code` 按既有 `map_guest_status` 映射；未知导出/非法导出名 `INVALID_ARGUMENT`；未进入 Guest 的 reentry/stale/wrong-thread 分别返回对应状态；`error` 同时给出可读细节。失败结果不得被 GUI 提交为业务结果。

## 3. 生命周期、handle 与 owner 线程

1. `create → load → resolve imports/exports → JIT init → trace open → call N 次 → destroy`。
2. 每个 session 固定 owner 线程（create 调用线程；GUI 为 AppKit 主线程）。`info/counts/export/selected/block/call/trace/destroy/set_observer` 在非 owner 线程调用返回 `WRONG_THREAD`。
3. handle 为 `(slot+1) | (generation << 32)`，slot 0..7；generation 从 1 开始、回绕跳过 0。destroy 后旧 token 返回 `STALE_HANDLE`，不解引用已释放对象；不得使用裸指针作为 handle。
4. 活动 call 中，同一 owner 线程再次 `call`、`destroy`、`trace_*`、`set_observer` 返回 `REENTRY`。HostImport/observer 重入同样如此。不同线程的调用先返回 `WRONG_THREAD`。
5. 两个 session 相互隔离：独立模块镜像、Region、JIT cache、HostImport 计数和 trace；销毁一个不影响另一个。
6. 部分初始化失败不产生半 session；所有本次 Host backing/dylib/JIT/registry/trace 在失败路径释放。destroy 关闭 trace、释放模块/JIT/AOT dylib；重复 destroy 返回 `STALE_HANDLE`。
7. 不承诺运行中 reload/reset。GUI 的 Reset 是显式调用 `AevDllReset` 业务导出，不是 session reload；新窗口/新进程创建新 session，按 DLL 静态初始化状态开始。

## 4. 参数、返回与 Guest 内存

- `args[4]` 是 RCX/RDX/R8/R9 原始 64 位寄存器值。调用方对 `int` 参数按低 32 位零扩展到 64 位；Guest 的 32 位运算只使用低 32 位。
- `rax` 是原始 64 位 RAX；`rax_i32` 是低 32 位按有符号 `int32_t` 解释。GUI 业务结果显示以 `rax_i32` 为准，不能把 `0xFFFFFFFF...` 的高位当作不同业务结果。需要指针/64 位结果时使用 `rax` 并明确标注。
- Guest VA / Host backing / Host 函数地址继续分离。GUI 只能通过 session 导出调用访问 Guest 状态，不直接解引用 Guest 数值；`AevDllReadAt/WriteAt/CallPtr` 等导出仍经同一 PE 内存模型。
- `AevPeCallResult` 的 `mem_fault_*`、`error_pc`、`guest_error`、`steps`、JIT/AOT 计数与既有 runner 语义一致；`call_index` 在成功进入 Guest 时从 1 开始，未进入 Guest 为 0。
- 累计计数在 session 内：`calls_total` 只统计进入 Guest 的调用；`host_import_calls` 统计实际 HostImport 调用；`aot/jit_blocks/compiles/execs` 为累计值。`load_count` 每个 session 成功 load 后为 1，GUI 用它证明按钮操作没有偷偷重载。
- HostImport 继续使用 registry 声明的固定实现（当前 `aevhost.dll!AevUpdate`，id=1，args=2），不扩展为任意 Windows API。observer 只观察，不改变参数/返回值。

## 5. Trace 语义

- `create` 的 `trace_path` 非空等价于 load 后立即 `trace_open`；`scenario` 为 header 标签。
- `trace_open` 截断文件、写 PE trace ABI 4 header（与 runner 相同字段：module/file/image/import/host/entry exports/entry/thunk/import count、all_blocks、aot_selected、aot_sha256），并把 call index 清零；call 编号仍为 1 起。
- `trace_close` flush 并关闭；已关闭时返回 OK（幂等），不改变业务状态。trace 打开时再次 open 返回 `INVALID_ARGUMENT`。destroy 自动 close。
- trace 事件来自该 session 的实际 dispatcher/JIT/AOT 与 HostImport，不使用 Host 影子记录。Trace 只写 Guest VA/模块身份，不写 Host backing/函数地址。
- 重新 open 后 call index 从 1 开始；旧文件内容被截断。调用方需在打开期间完成采集。

## 6. 查询接口

`aev_pe_session_info` 返回模块身份（file/image/import/host/entry exports SHA、preferred/load base、image size、thunk base、import count、relocs_stripped、entry VA、stack/entry_rsp、region count、all/selected count、AOT present/SHA、load_count、module kind、entry export、target triple）。不返回 Host backing 地址。

`export_count/export_info/find_export` 返回 named export 的 name/RVA/VA/ordinal；ordinal-only export 不进入该列表（与 PE 契约一致）。

`selected_count/selected_rip` 读实际加载的 AOT dylib selected 表；`all_block_count/all_block_rip` 读实际 Program 块；`block_info` 按 Guest RIP 返回 `block_identity` 的 RVA/size/SHA 与 selected 标志。所有查询只读，不改变调用计数。

`counts` 返回 §4 的累计计数和 trace 状态；`trace_records` 为当前/最近一次 trace 已写事件数（关闭后保留最近值，重新 open 清零）。这些计数由 session 核心维护，不是 GUI 影子计数。

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
| base JIT | base profile trace 含实际 `jit_compile`/`jit_exec`；结果/flags 与 AOT 一致 |
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
| coordinator | 冻结候选，待实现/复核 | 本文件与 `docs/tasks/P0-PE-GUI-01.md` |
| compiler | 待会签 |  |
| binary | 待会签 |  |
| platform | 待会签 |  |
| reviewer | 待审查 |  |
