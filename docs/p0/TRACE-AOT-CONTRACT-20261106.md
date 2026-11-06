# P0 Trace → 补充 AOT → 新进程验证契约（试验 v1）

日期：2026-11-06。状态：compiler/binary/platform/reviewer 已会签（v1）。只冻结本轮 P0-TRACE-AOT-01 所需的有界实验边界，不是 P1 的完整 Trace/artifact/失效契约。

## 1. 范围与不承诺

本轮只做：

- 一个固定 COFF 函数（callback fixture）和一个自编有界 trace fixture，在本项目共享 bridge/JitRuntime 的实际执行点采集块级记录。
- 记录 → 离线选择实际执行且可翻译的块 → 共享 translator/emitter 生成新 AOT artifact/manifest → 新进程校验/执行。
- 准备集 A 的 IMUL 块由真实 JIT 发现后进入下一轮 AOT；独立集 B 的新分支目标首次真实 JIT，再进入新进程 AOT。
- 输入/entry/function/块字节/AOT all+selected table/ABI/artifact 错配明确拒绝；旧 artifact 保留。
- compat/native 使用补充后 callback artifact 复跑同一 GUI 输入；原 100 次、错误路径和四组 ctest 不回归。

明确不承诺：通用 PE/Win32、真实应用、线程/模态/任意重入、运行中自修改/写保护/同址重分配/并发失效、完整指令 Trace、持久化 JIT 缓存产品化、`.app`/签名/公证、性能或提速结论。关闭/重开进程不等于 live invalidation。

## 2. 身份与 ABI 版本

- 输入身份：完整 COFF 文件的 **SHA-256** 是持久化权威身份；现有 FNV-1a-64 仅作为内部快速校验，不能单独作为完整身份。
- Guest ABI：仍为 v1；`GuestState` 布局/大小不变。
- Callback ABI：仍为 v2；不修改现有字段/状态/调用约定。
- 新增 **Trace ABI v1** 和 **AOT artifact ABI v1**；两者只在 `include/aev_bridge.h`/`include/aev_guest.h` 的扩展中定义，不改变 callback ABI v2。
- Trace/artifact 必须记录 `input_sha256`、`input_fnv`、`entry`、`function`、`callback_abi`、`guest_abi`、`trace_abi`、`aot_artifact_abi`、目标三元组/翻译器 ABI。任一不匹配必须在执行/生成前失败。

## 3. Trace ABI v1

### 3.1 实际采集点

记录只由实际执行链产生：

1. AOT 块执行：AOT dylib 的 `aev_note_aot` 在每次 AOT 块入口记录。
2. JIT 编译/执行：共享 `JitRuntime` 在 ORC lookup 成功后记录 compile，在真正调用 JIT 函数前记录 exec；observer 不改变 dispatcher/缓存语义。
3. 间接/分发目标：bridge dispatcher 在 AOT 未覆盖而进入 JIT/未知目标/导入时记录目标 RIP。
4. HostImport：bridge 在真正调用 Host 回调时记录 import id；v1 不记录原始 a0..a3。
5. 调用结束/错误：bridge 记录状态、`error_pc`、Guest 错误码。

不得手填预期轨迹，不得用录制业务结果驱动执行。

### 3.2 瞬态 trace sink

`GuestState.reserved0` 在 trace 启用时临时保存一个 `AevTraceSink*`，调用结束/禁用时清零；AOT dylib 与 bridge 共享同一 `aev_guest.h` 定义：

```c
#define AEV_TRACE_ABI_VERSION 1u
typedef struct AevTraceSink {
    uint32_t abi_version;   /* 1 */
    uint32_t reserved;
    void (*on_block)(void *ctx, uint32_t source, uint64_t rip);
    void *ctx;
} AevTraceSink;
/* source: 0 = AOT block, 1 = JIT compiled block, 2 = JIT executed block */
```

`reserved0` 只由 bridge 在本次调用内写入；AOT 侧信任该指针并检查 `sink->abi_version`，ABI 不匹配时跳过。该指针是本次调用的瞬态 Host 指针，不写 trace、不跨调用、不持久化。回调不得抛出 C++/ObjC 异常、不得解引用 Guest 内存、不得修改 Guest 状态；bridge 在所有调用退出路径清零 `reserved0`。未启用 trace 时 `reserved0=0`，行为与原实现一致。

### 3.3 记录格式

JSON Lines UTF-8，一行一个对象；首行必须是 header：

```json
{"record":"header","trace_abi":1,"scenario":"A","input_sha256":"...","input_fnv":"0x...","entry":"0x140001000","function":"aev_guest_callback","aot_sha256":"...","callback_abi":2,"guest_abi":1,"aot_artifact_abi":1,"target":"arm64-apple-macosx14.0.0","translator_version":"aev-translate-v1","translator_abi":1,"trace_tool_version":"aev-trace-sink-v1","all_blocks":["0x140001000",...],"aot_selected":["0x140001000",...]}
```

事件示例：

```json
{"record":"aot","call":1,"rip":"0x140001000","block_sha256":"...","offset":0,"size":28}
{"record":"jit_compile","call":1,"rip":"0x140001020","block_sha256":"...","offset":32,"size":23}
{"record":"jit_exec","call":1,"rip":"0x140001020"}
{"record":"host_import","call":1,"id":1}
{"record":"dispatch_miss","call":1,"rip":"0xdead0000","status":4}
{"record":"call_end","call":1,"status":0,"error_pc":"0x0","guest_error":0}
```

规则：

- 所有地址用十六进制字符串；不保存 Host 函数/对象地址、Guest 内存基址/当前 identity-mapped 数据地址、ctypes/Python 指针。v1 的 `host_import` 只记录 import id，不记录原始参数（避免把可能是 Guest 指针的 a0..a3 持久化）。
- `offset` 是 **section-relative** 偏移：`fn.section_offset + (rip - fn.address)`；`block_sha256` 对 section data 的 `[offset, offset+size)` 取 SHA-256（不是 `fn.bytes[offset]`，因为 `fn.bytes` 从 symbol offset 开始）。`size` 从块首指令到 terminator 末字节（含）；块没有 terminator 时取下一个 leader，仍无下一个 leader 时取函数末尾（或 section 末尾）。bridge 与 supplement 必须共用同一 helper。
- `block_sha256` 是实际输入 COFF 中该 section-relative 块字节（offset/size）的 SHA-256；不是生成 ARM64 代码的哈希。
- 记录粒度是块级，不是指令级，也不是 Windows 原生 Trace；trace 不声称完整覆盖率。
- `header` 的 `all_blocks` 是本次加载程序的全部可翻译块，`aot_selected` 是当前 AOT 实际表；两者排序稳定。A trace 的 `all_blocks` 会包含 B 尚未执行的块；验收用**执行事件集合**判断“B 未在 A 出现”，不以 header all/selected 判断。
- trace 文件截断、未知 record、字段缺失/类型错误、header 不一致、block offset/size/hash 与输入不符，均视为无效；不得据此生成 artifact。测试覆盖 open/close/reopen、in-call open/close、不可写路径、destroy 时 flush/close、`reserved0` 清零。

### 3.4 生命周期 API

```c
AevBridgeStatus aev_bridge_trace_open(AevBridge *b, const char *path,
                                      const char *scenario,
                                      char *err, size_t err_cap);
AevBridgeStatus aev_bridge_trace_close(AevBridge *b);
```

- `trace_open` 只能在 create 后、call 前、owner/main 线程调用；已打开再次 open 或 `in_call` 中 open/close → `INVALID_ARGUMENT`/`REENTRY`。
- `trace_open` 写 header；文件不可写 → `LOAD`/`INVALID_ARGUMENT`，不进入 Guest。
- trace 启用时 bridge 在每次 call 前设置 `state.reserved0=&sink`，call 的所有退出路径清零；`trace_close` 必须 flush/close，`aev_bridge_destroy` 在 trace 打开时也先 flush/close；关闭后不能再记录。
- 这是实验诊断记录，不是完整 Trace 系统，也不改变正常执行结果。

## 4. AOT artifact / coverage v1

### 4.1 Coverage 是构建选择

- 每个可翻译基本块有一个 RIP。AOT 表是 **selected** 子集；未选入的合法块可真实 JIT；真正不支持的指令在翻译/构建期显式失败；非块边界/未知目标不能靠 JIT 假装修复。
- 删除“IMUL 永远只能 JIT”的硬编码语义：AOT emitter 必须能像 JIT emitter 一样发射受支持的 IMUL；是否选入 AOT 由 coverage 决定。
- 本轮 base coverage **必须**排除 callback fixture 的 `0x140001020`，以保留上一轮真实 JIT 发现场景；默认 translator 行为继续排除 IMUL 块，以保持原 4 ctest/77 checks 的 `_aev_blk_140001020`/`_aev_blk_140001042` 缺席和空表 IR 拼写。补充构建显式包含该已执行块。translator 保留现有 `--entry <name>`（函数名），并增加可重复的 `--aot-include-rip <hex>` / `--aot-exclude-rip <hex>`（或等价 coverage 参数）；同一 RIP 同时 include/exclude 必须报错。supplement 的 `--entry <name>` 用于函数选择，`--entry-addr <hex>` 用于校验 synthetic Guest PC（必须等于该函数入口）；所有 `--aot-*` RIP 都是 synthetic Guest PC，不是 COFF RVA。实现必须基于共享 `build_program`/`emit_aot_module`，不新建 emitter。
- `all_blocks` 必须包含所有可翻译块（含 JIT-only 块）；`aot_selected`/`aev_blocks` 只含本次选入 AOT 的块。

### 4.2 AOT dylib 必须导出并校验

除现有 `aev_aot_run`、`aev_aot_abi_version`、`aev_guest_state_size`、`aev_input_hash` 外，新增：

- `aev_input_sha256`：32 字节输入 SHA-256。
- `aev_artifact_abi_version`：1。
- `aev_entry_addr`：本 artifact 的入口 Guest RIP。
- `aev_function_name_sha256`：入口函数名的 SHA-256（32 字节），输入固定为函数名 UTF-8 字节、不含 NUL、无长度前缀。
- `aev_target_triple`：NUL 结尾的 LLVM target triple 字符串。
- `aev_translator_abi_version`：u64 翻译器/artifact 生成器 ABI 版本；v1 固定为 `AEV_TRANSLATOR_ABI_VERSION=1`。bridge/supplement/header 必须比较同一数值。
- `AEV_AOT_TARGET_TRIPLE`：本轮固定为 `arm64-apple-macosx14.0.0`；AOT artifact 与 bridge 期望值比较，不用宿主默认 triple。
- `aev_all_block_count` / `aev_all_blocks`：稳定排序的全部块 RIP（`uint64_t[]`）。
- `aev_block_count` / `aev_blocks`：现有 selected `AevBlock[]`。

新增只读查询（不改变 callback ABI v2）：

```c
#define AEV_ARTIFACT_INFO_ABI_VERSION 1u
typedef struct AevArtifactInfo {
    uint32_t struct_size;
    uint32_t abi_version;          /* AEV_ARTIFACT_INFO_ABI_VERSION */
    uint32_t aot_table_count;      /* aev_block_count，不是 per-call aot_blocks */
    uint32_t all_block_count;
    uint32_t artifact_abi_version; /* 1 */
    uint32_t translator_abi_version; /* 1 */
    uint64_t entry_addr;
    uint8_t input_sha256[32];
    uint8_t aot_sha256[32];
    char target_triple[48];        /* NUL-terminated */
} AevArtifactInfo;

AevBridgeStatus aev_bridge_artifact_info(const AevBridge *b,
                                         AevArtifactInfo *info,
                                         char *err, size_t err_cap);
```

`AevCallResult.aot_blocks` 是**每次调用的 AOT 块入口次数**，不是 AOT 表大小；GUI profile 必须用 `aot_table_count` 断言 4/5，不能把 per-call 次数当表大小。

bridge 在执行前必须：

1. 比较 `aev_input_sha256` 与 COFF 内容 SHA-256；比较 FNV/entry/function/`aev_entry_addr`/`aev_function_name_sha256`/`aev_target_triple`（固定 `arm64-apple-macosx14.0.0`）/`aev_translator_abi_version`/`aev_artifact_abi_version`/`aev_aot_abi_version`。
2. 比较 `aev_all_blocks` 与本次 `Program` 的全部块地址逐项相等（排序、去重；不得为空的 fixture 除外）。
3. 比较 `aev_blocks` 是 `aev_all_blocks` 的子集、无重复、已排序；对每个 selected `fn`，`dlsym("aev_blk_<rip>")` 必须等于该 `fn`，不能只检查非空；由 selected 集合推导每个 Program 块的 `jit_only`（未选入=可 JIT）。
4. 记录 AOT dylib SHA-256 到 trace header；manifest 只是辅助证据，不能代替上述实际符号/表校验。
5. 任一项不匹配 → `ABI_MISMATCH`/`LOAD`，不执行；旧 artifact 保留。上一轮 `5773...` AOT 是证据归档，不兼容新 artifact ABI，不得静默回退；本轮 base artifact 重新构建为 artifact ABI v1 并继续可跑。

### 4.3 Manifest

离线工具/构建脚本写 JSON manifest，至少含：input SHA-256/FNV、entry/function、all/selected block RIP、block bytes SHA-256、trace 文件 SHA-256、trace ABI/artifact ABI、目标三元组、工具版本。manifest 不取代 dylib 实际符号校验。

## 5. 离线补充工具 v1

新增离线入口 `aev-aot-supplement`（实现可用 Python 编排共享 `aev-translate`/emitter；也可用 C++，但必须复用共享 translator/emitter，不复制 ORC/JIT、不重编业务 C）：

```text
aev-aot-supplement --input <coff> --function <name> --entry <name> --entry-addr <hex>
  --trace <jsonl> [--trace <jsonl> ...]
  --out-ll <file> --out-header <file> --out-manifest <json>
```

行为：

1. 读取并校验所有 trace 的 header：相同的 input SHA-256/FNV、entry、function、callback/guest/trace/artifact ABI/translator 数值版本/target triple；拒绝截断/损坏/未知 record/不一致 header。header 的 `all_blocks` 必须与由输入 COFF/`build_program` 重建的全部块逐项相等（排序、去重），`aot_selected` 只作诊断/子集校验，不能作为 selection 的唯一依据。
2. 用 `build_program` 读取输入 COFF；每个 trace 的 block event 必须能在 Program 中找到，offset/size/block_sha256 必须与实际块字节一致。每个 trace 的所有 `call_end` 必须 `status==0`，且至少有一个最终 `call_end`；call 编号单调，失败/部分 trace（包括“先失败后成功”）不得生成 artifact。
3. selected = 所有 trace 中实际执行的 `aot`/`jit_compile`/`jit_exec` 块 RIP 的并集；至少应包含一个 JIT 发现块（若输入本来无 JIT，工具可生成同 coverage 的 artifact，但本轮验收 A/B 都要求有提升块）。
4. 使用共享 `emit_aot_module`（显式传入 selected 集合，或等价的临时 forced-AOT flags；不要复制 block 身份/jit_only 逻辑）生成新的 IR/header/manifest：`all_blocks` 全部，`selected` 为上述并集；不覆写 base artifact；IR/manifest 先写临时文件，全部成功后再 rename。工具只链接共享 translator/emitter，不链接 ORC/JIT。dylib 由构建脚本写入临时文件，校验 ARM64/otool/输入 SHA/selected 后再 rename；失败保留旧 artifact。
5. 任何校验失败不得生成“完整”artifact；旧 artifact 保持可复跑。header 表伪造/乱序/重复、失败 call_end、block 事件缺失均为负例。
6. 编译/链接由脚本调用 clang + 现有 `aot_runtime.o`/`aev_mem_aot.o`，不重编业务 C 源码。

## 6. 测试输入

### 6.1 A：callback fixture

- 继续使用 `spikes/p0-engine/testdata/callback/aev_callback_fixture.obj`（SHA-256 `4f6929f7...`）。
- base artifact 默认排除 `0x140001020`（IMUL 块）；首次执行 A 记录真实 JIT compile/exec。
- supplement 后新 artifact 包含该块；新进程 A 的该块 AOT 执行，`jit_compiles=0`，业务/helper/counter 与 Windows 一致。
- 同一补充 artifact 驱动 compat/native GUI 的同一 callback 输入；100 次、错误路径、真实控件 helper-after-fault 回归保持。

### 6.2 B：新增有界 trace fixture

- 新增 `spikes/p0-engine/testdata/trace/aev_trace_fixture.c/.obj`，MSVC `/O2 /GS- /Brepro`，固定 SHA-256；函数放在独立 `.aevtext` section，避免相邻 `.text` 污染 `all_blocks`/块哈希。
- 导出 `aev_trace_guest(unsigned int op, unsigned int x, GuestCtx *ctx)`；A 路径（`op != 0xA5A5`）不执行 B 分支，B 路径（`op == 0xA5A5`）包含独立基本块和 IMUL；helper 返回 op，counter 在后。
- Windows reference 链接同一对象，输出 A/B 固定结果；Mac base 先执行 A（不发现 B），再执行 B（真实 JIT 新块），supplement 后新进程 A/B 的 B 块 AOT 执行。
- 必须提交 `spikes/p0-engine/testdata/trace/{aev_trace_fixture.c,aev_trace_fixture.obj,aev_trace_ref.c,build_msvc.ps1,reference_stdout.txt,*.disasm.txt,*.manifest.json}`；Windows driver 同一对象、固定对象 SHA-256、A/B stdout，反汇编证明 B-only 块含 IMUL。B RIP 出现在 B 执行事件而不在 A 执行事件。
- B 是新控制流/新基本块，不是只改文本触发同一固定块。

### 6.3 GUI coverage profile 与证据固定

- `run_engine` 必须显式接收 `--coverage-profile base|supplemented`（或等价显式 expected 参数），不得根据观察到的计数自动推断。
  - base：`aot_table_count=4`、`total_jit_compiles=1`、`jit_execs>0`、loop `jit_cached=true`；证明原始 JIT 发现路径仍可跑。
  - supplemented：`aot_table_count=5`、`total_jit_compiles=0`、`jit_execs=0`、`jit_compiles=0`、loop `jit_cached=true`；证明 promoted 块不再 JIT。
  - 两 profile 都保持结果控件/hash/counter/错误路径断言；精确 44 项 = 42 个原有检查名称（其中 helper-after-fault 的 4 个名称保留，仅在同名检查内升级为事件驱动/`backend.get_text` 实际控件断言）+ 1 个同名位置替换（`loop_100_jit_executed` → `loop_100_jit_policy`）+ 1 个新增 `aot_table_count` = 44。 compat/native 各跑两 profile。
- supplemented GUI 必须绑定 supplement manifest、AOT 文件 SHA-256 和 selected 表中的 promoted RIP（callback `0x140001020`）；传入 base artifact 或 SHA 不匹配必须失败，不得 JIT 回退。
- 增加真实按钮 → helper 成功 → Guest counter 故障 → 使用 `backend.get_text(result_handle)` 读取实际控件文本不变 → 恢复的事件驱动测试；替换原直接 `_call_guest` 的 helper-after-fault 检查以保持 43 项，不使用 Host 业务重写。
- 增加 GUI artifact 拒绝负例：mutated/truncated AOT 或错误 selected table → 进程非零退出、`success=false`、`ABI_MISMATCH`/`LOAD`、无 Guest 结果提交、无 compat 回退。
- GUI 脚本使用 `set -euo pipefail`；supplement 构建失败不得留下 base artifact 被当作 supplemented 使用。

## 7. 验收矩阵

| 场景 | 必需证据 |
| --- | --- |
| A 首次 | base trace 中 AOT 块 + JIT compile/exec `0x140001020`；输入 SHA/块 SHA 绑定；业务/helper/counter |
| A 新进程 | 新 artifact SHA；promoted 块 AOT 事件；该块无 JIT compile；结果一致；旧 base artifact 保留 |
| B 首次 | base 执行 A 后 B 输入产生 A 未出现的 B 块；真实 JIT compile/exec；结果与 Windows B 一致 |
| B 新进程 | 新 artifact 中 B 块 AOT；无 JIT；结果一致 |
| 负例 | 修改 input SHA、entry/function、block bytes、all/selected table、ABI/runtime、trace 截断/损坏 → 明确失败；旧 artifact 仍可跑 |
| 回归 | 原 4 ctest（含 77/77 bridge）不回归；compat/native 各 44/44（42 个原有名称（helper-after-fault 4 个同名升级）+ 1 个同名替换（`loop_100_jit_executed`→`loop_100_jit_policy`）+ 1 个新增 `aot_table_count` = 44；base/supplemented JIT 计数按 profile 明确）；无 Unicorn |
| 双机 | Windows 同一固定对象 A/B 输出；记录命令/退出码/SHA |

“旧 artifact 仍可跑”指本任务新 translator 以默认 coverage 构建的 base artifact（未被 supplement 覆写）；上一轮 `5773...` dylib 仅作归档证据，不承诺被新 bridge 加载。

## 8. 会签

| 角色 | 结论 | 证据 |
| --- | --- | --- |
| compiler | SIGN-OFF：contract accepted | ses_e51e637fbffeOfm7GWiVYvsI4B |
| binary | SIGN-OFF：contract accepted | ses_e51e637f9ffeZkPwJwBAmmkllk |
| platform | SIGN-OFF：contract accepted | ses_e51e637f8ffeTMuaL4wgJYaUWr |
| reviewer | SIGN-OFF：contract accepted | ses_e51dc8c92ffeTaxv2iFaMEBQ6i |
