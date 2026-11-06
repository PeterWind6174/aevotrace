# P0 最小 Host↔Guest 回调契约（试验 v2 / 实现版）

日期：2026-11-05。状态：compiler/platform/reviewer 已会签（最终代码 HEAD `afff569`；后续仅文档提交）。本文件只冻结本轮 P0-CALLBACK-INTEGRATION-01 所需的最小边界，不是 P1 的完整 ABI、PE、TSO 或异常契约。

> **2026-11-06 取代说明（P0-PE-DLL-01）**：为引入受检查 PE 区域内存模型，`GuestState` 从 32 字段/256 字节升为 37 字段/296 字节，Guest ABI 升为 2，callback ABI 升为 3。本文第 2 节及下文的 256/ABI 1/ABI 2 数值仅代表历史版本；当前边界以 [PE-DLL-CONTRACT-20261106](PE-DLL-CONTRACT-20261106.md) 第 1、3 节为准。现有 COFF 回调语义（单线程、最多 4 参数、显式 HostImport、AOT 优先/JIT 回退）继续保留，但产物必须按新 ABI 重建，旧产物明确拒绝。

## 1. 范围与不承诺

本轮只支持：

- 单线程、单层 `Host → Guest → Host` 调用；调用发生在 AppKit 主线程（或测试主线程）。
- Windows x64 Guest 调用约定中的整数/指针参数（最多 RCX、RDX、R8、R9 四个），返回值放 RAX。
- 一个连续的 Guest 内存区和一段 Guest 栈；所有 Guest 访问经受检查的 `aev_load` / `aev_store`。
- 显式注册的 HostImport（Guest 地址 + 数字 ID + Host 回调），以及正常返回、内存错误、未知导入、JIT 错误、步数上限、未支持重入和错误线程的显式失败。
- AOT 优先、未覆盖块回退现有 ORC JIT；复用现有 dispatcher/helper，不复制第三套 ORC dispatcher。

明确不承诺：任意嵌套 Guest 重入、线程/模态/完整消息泵、通用 PE 装载、Win32 API 全集、SIMD/浮点、TLS/SEH/unwind、原子/TSO、代码版本失效、持久化 ABI、性能结论。集成路径不得加载或执行 Unicorn；历史 Unicorn 对照只能显式选择，失败时不得静默回退。

## 2. 身份、输入和产物

### 2.1 Guest 输入

本轮使用自编的最小 x64 回调 fixture，而不是旧 UI DLL：

- 源码：`spikes/p0-engine/testdata/callback/aev_callback_fixture.c`（B 阶段提交）。
- Windows 编译：MSVC x64，`/c /O2 /GS- /Brepro /W4`；函数放入独立 `.aevtext` section，section 只包含该函数。
- 固定输入对象：`spikes/p0-engine/testdata/callback/aev_callback_fixture.obj`。
- 固定 SHA-256（2026-11-05 实测）：`4f6929f7c71c82169295b6675517a839ae9533a85941e2632115bf0ef47328dc`。同一目录连续两次构建整对象哈希一致；跨目录重建因 `.debug$S`/`.chks64` 等元数据可能不同，以 `.aevtext` 指令流对照，Windows 参考始终链接提交的固定对象。
- 符号/函数：`aev_guest_callback`；synthetic Guest PC `0x140001000`；`.aevtext` 86 字节、27 条指令、5 个基本块。
- 指令盘点（Capstone/`dumpbin` 实测）：`push/pop`、`sub/add rsp`、`mov`（含 64 位寄存器）、`movzx`、`xor`、`inc`、`imul r32,r/m32,imm32`、`cmp`、`test`、`jcc`（`je/jb`）、`call qword ptr [r9]`、`ret`、`nop`。现有 translator 全部支持；`imul` 所在块 `0x140001020` 为 JIT-only，其余 4 个块为 AOT。
- 该对象没有 `.aevtext` 重定位；`.pdata`/`.chks64`/`.debug$S` 等元数据 section 不进入转换。对象由同一份固定字节在 Windows 链接、在 macOS 解析，源码不参与 Mac 转换输入。

选择理由：旧 `p0_gui_test.dll` 需要 26 个 Win32 导入、PE 装载、线程/模态/消息泵；ENGINE 当前没有 PE/重定位/导入能力。新增小 fixture 是计划允许的有界输入，旧 UI 测试保留为历史对照，不宣称通用 PE loader。

### 2.2 fixture 语义

```c
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned short u16;
typedef struct GuestCtx {
    u64 (*update)(u64 hash, u64 len, u64 op); /* GuestImport 地址，Guest 内存内 */
    u64 *counter;                            /* Guest 内存内计数器 */
} GuestCtx;

u32 aev_guest_callback(unsigned int op, const u16 *text, u64 len, GuestCtx *ctx);
```

Guest 行为：对 `text[0..len)` 计算 32-bit FNV-1a；调用 `ctx->update(hash, len, op)`；helper 固定返回 `op`（Windows C helper 与 Mac Host helper 相同）；成功返回后 `(*ctx->counter)++`；返回 `hash ^ (u32)helper_return`。业务结果、循环和计数器递增都由 Guest 机器码完成；Host 只复制输入、绑定导入、返回约定值、读取结果。helper 返回 `op` 使 Guest→Host 参数与 Host→Guest 返回值都进入最终结果；Host 不得返回计算好的 hash。计数器在 helper 成功返回后递增，未知导入/非法内存路径不提交该副作用。

Windows 参考输出（同一 `.obj`，helper 返回 op）：

```text
CASE alpha op=7 len=6 result=3828036654 counter=1
CASE beta op=9 len=8 result=614625053 counter=2
CASE hidden op=23130 len=7 result=651115151 counter=3
```

GitHub Windows 参考：同一个 `.obj` 与自编 `aev_callback_ref.c` 链接；`GuestCtx.update` 指向原生 C helper。Mac：同一个 `.obj` 由 `aev-translate` 生成 ARM64 AOT dylib，bridge 把 `GuestCtx.update` 写成 GuestImport stub 地址。两侧机器码相同，区别仅是运行时把函数指针绑定到原生地址还是 GuestImport 地址。

### 2.3 HostImport

- GuestImport 地址保留区：`0x140010000 + id * 0x10`，`id >= 1` 由 Host 提供并登记；bridge 校验 `guest_addr` 与 `id` 的对应关系、唯一性和范围。`host_import_id=0` 专门表示“无/未知”，合法导入不得使用 0。保留区不映射为 Guest 可读写内存，只作为 dispatcher 识别键。
- HostImport 回调签名（状态出参，不依赖跨 C 边界的异常）：

```c
typedef AevBridgeStatus (*AevHostImportFn)(void *host_ctx, AevBridge *bridge,
                                           uint64_t a0, uint64_t a1,
                                           uint64_t a2, uint64_t a3,
                                           uint64_t *out_rax);
```

`a0..a3` 对应 Guest 调用点的 RCX/RDX/R8/R9。fixture 的 helper 只使用 `a0..a2`，`a3` 是原始 R9（GuestCtx 指针），不是第四个 helper 参数；Host 回调可忽略 `a3`。回调返回 `AEV_BRIDGE_OK` 时必须提供非 NULL `out_rax` 并写入结果；`out_rax==NULL` 或返回其他状态时，bridge 以 `INVALID_ARGUMENT`/该状态（未知状态归一为 `AEV_BRIDGE_ERR_HOST`）停止，不写 RAX、不恢复 Guest。回调必须自行捕获语言层异常并返回状态；bridge 仍会在调用点用 `try/catch(...)` 做最后防线，映射为 `AEV_BRIDGE_ERR_HOST`。Host 函数指针、Python 对象地址和 ctypes 缓冲区地址不得写入 GuestState 或 Guest 内存持久保存；GuestCtx 中只允许 Guest 地址。

调用点语义：Guest `call [r9]` 已把返回地址压入 Guest 栈；dispatcher 在 JIT 查找前检查 GuestImport 注册表。命中时经检查内存读出 `[rsp]`、把 `rsp += 8`、调用 Host 回调；仅当回调返回 `OK` 时写 RAX 并把 `rip` 设为返回地址。未命中且地址落在保留区 → `AEV_BRIDGE_ERR_UNKNOWN_IMPORT`；保留区外未知目标 → `AEV_BRIDGE_ERR_NO_BLOCK`。两种情况都不得伪造 RAX 或继续执行 Guest。

## 3. 数据布局与 ABI 校验

`GuestState` 沿用现有 `aev_guest_fields.def`：32 个 `uint64_t`，256 字节，8 字节对齐；字段顺序不变。`aev_load`/`aev_store` 的地址在 v0 中是本 session 的 Guest 内存映射地址（session-local），不对跨 session/跨进程持久化作承诺；所有 Guest 地址仍必须经检查接口访问，Host 不直接解引用任意 Guest 值。

新增公开头 `spikes/p0-engine/include/aev_bridge.h`（B 阶段实现）必须包含：

```c
#define AEV_CALLBACK_ABI_VERSION 2u
#define AEV_BRIDGE_MAX_ARGS 4u
#define AEV_GUEST_SENTINEL 0xFFFFFFFFFFFFFF00ull

typedef struct AevBridge AevBridge;

typedef enum AevBridgeStatus {
    AEV_BRIDGE_OK = 0,
    AEV_BRIDGE_ERR_INVALID_ARGUMENT = 1,
    AEV_BRIDGE_ERR_ABI_MISMATCH = 2,
    AEV_BRIDGE_ERR_LOAD = 3,
    AEV_BRIDGE_ERR_NO_BLOCK = 4,
    AEV_BRIDGE_ERR_UNKNOWN_IMPORT = 5,
    AEV_BRIDGE_ERR_MEMORY = 6,
    AEV_BRIDGE_ERR_JIT = 7,
    AEV_BRIDGE_ERR_STEP_LIMIT = 8,
    AEV_BRIDGE_ERR_REENTRY = 9,
    AEV_BRIDGE_ERR_WRONG_THREAD = 10,
    AEV_BRIDGE_ERR_HOST = 11,
    AEV_BRIDGE_ERR_INTERNAL = 12
} AevBridgeStatus;

typedef struct AevBridgeConfig {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *coff_path;
    const char *function_name;
    uint64_t entry_addr;
    const char *aot_dylib_path;
    uint64_t guest_mem_size;
    uint64_t guest_stack_size;
    uint32_t trace;
    uint32_t dump_jit;
    uint32_t trace_aot;
    uint32_t require_main_thread;
    uint32_t reserved0;
} AevBridgeConfig;

typedef struct AevCallResult {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t status;
    uint32_t guest_error;
    uint64_t guest_rax;
    uint64_t guest_rip;
    uint64_t guest_rsp;
    uint64_t error_pc;
    uint64_t steps;
    uint64_t aot_blocks;          /* per-call delta */
    uint64_t jit_blocks;          /* per-call delta */
    uint64_t jit_compiles;        /* per-call delta */
    uint64_t jit_execs;           /* per-call delta */
    uint64_t host_import_id;
    uint64_t call_index;          /* 1-based; 0 means the call never entered Guest */
    uint64_t guest_mem_base;
    uint64_t guest_mem_size;
    uint64_t total_calls;
    uint64_t total_aot_blocks;
    uint64_t total_jit_blocks;
    uint64_t total_jit_compiles;
    uint64_t total_jit_execs;
} AevCallResult;
```

当前 v2：`sizeof(GuestState)=256`、`sizeof(AevCallResult)=160`、`sizeof(AevBridgeConfig)=80`。v1 是实现前的内部草案，从未发布；v2 把 destroy 状态、guest_info 的栈边界、abi_info 的 config_size、AevCallResult 的 total_jit_blocks 和 trace_aot 一起定稿，避免把不同布局当成同一 ABI。调用方必须用 `aev_bridge_abi_info` 返回的 ABI/尺寸初始化 `AevBridgeConfig.struct_size/abi_version` 和 `AevCallResult.struct_size/abi_version`；不匹配即 `INVALID_ARGUMENT`/`ABI_MISMATCH`。`trace_aot` 只控制 AOT 块日志，`trace` 控制 JIT IR，`dump_jit` 控制 JIT ARM64 dump；三者都只用于诊断。

入口/生命周期：

```c
int aev_bridge_abi_info(uint32_t *abi_version, uint32_t *guest_state_size,
                        uint32_t *call_result_size, uint32_t *config_size);
AevBridgeStatus aev_bridge_create(const AevBridgeConfig *cfg, AevBridge **out_bridge,
                                  char *err, size_t err_cap);
AevBridgeStatus aev_bridge_destroy(AevBridge *b);
int aev_bridge_guest_info(const AevBridge *b, uint64_t *base, uint64_t *size,
                          uint64_t *stack_base, uint64_t *stack_top);
int aev_bridge_register_import(AevBridge *b, uint64_t guest_addr, uint64_t import_id,
                               AevHostImportFn fn, void *host_ctx,
                               char *err, size_t err_cap);
int aev_bridge_guest_write(AevBridge *b, uint64_t guest_addr, const void *src,
                           uint64_t len, char *err, size_t err_cap);
int aev_bridge_guest_read(AevBridge *b, uint64_t guest_addr, void *dst,
                          uint64_t len, char *err, size_t err_cap);
int aev_bridge_call(AevBridge *b, uint64_t entry_addr,
                    uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                    AevCallResult *result, char *err, size_t err_cap);
/* Test-only fault injection; not used by the GUI path. mode=1: JIT failure at rip;
 * mode=2: pretend a sentinel return with rsp+8 (INTERNAL path). */
int aev_bridge_set_test_fault(AevBridge *b, uint32_t mode, uint64_t rip,
                              char *err, size_t err_cap);
```

`aev_bridge_destroy` 返回 `OK` 或在 `in_call` 时返回 `AEV_BRIDGE_ERR_REENTRY` 且不释放；成功后指针失效。`aev_bridge_set_test_fault` 只用于 B 的负例，GUI 不得调用。

输入身份：生成的 AOT dylib 还必须导出 `aev_input_hash`（输入 COFF 字节的 64-bit FNV-1a；translator 从同一对象计算并写入 IR）。bridge 在 create 时对同一 COFF 字节计算 FNV-1a 并与该符号比较；不一致 → `ABI_MISMATCH`。该哈希用于发现“版本相同但对象/产物错配”的意外复用，不是密码学签名；同时仍比较 AOT 块地址表。负例用同一入口/块地址但改动一个立即数的对象生成第二个 dylib，必须被拒绝。

`aev_bridge_create` 通过状态返回 `ABI_MISMATCH`/`LOAD`/`INVALID_ARGUMENT`；失败时 `*out_bridge=NULL`。`coff_path`/`function_name`/`aot_dylib_path` 在 create 时复制，调用方字符串无需跨生命周期存活。`cfg.entry_addr` 是唯一的 synthetic 入口；`function_name` 必须映射到该地址，`aev_bridge_call` 的 `entry_addr` 必须等于 `cfg.entry_addr`，否则 `INVALID_ARGUMENT`。create 必须验证该入口是已翻译的基本块、且不在导入保留区；生成的 AOT dylib 必须用同一地址构建。`guest_stack_size` 至少 64 KiB，Guest 栈、哨兵和 `[S+8,S+0x30)` 必须完整落在 `mem_base..mem_base+mem_size` 内，越界配置直接拒绝。栈固定占用内存顶部 `guest_stack_size` 字节：`stack_top = base+size`、`stack_base = stack_top-guest_stack_size`；`stack_base-base` 至少 64 KiB 作为 Host 数据区，Host 的文本/GuestCtx/计数器必须放在 `stack_base` 以下，不得与栈重叠；不足则 create 返回 `INVALID_ARGUMENT`。

`aev_bridge_guest_info` 在首次调用前返回 Guest 内存基址/大小以及栈基址/栈顶；Host 必须先通过它取得基址，再写文本、GuestCtx 和计数器。v0 的 Guest 地址就是该 session 的 Host 映射地址（与 `aev_mem.c` 的 `mem_base` 一致），不承诺跨 session/持久化；`guest_write/read` 和 `aev_load/aev_store` 都会检查范围，Host 不得直接解引用 Guest 值。`GuestCtx` 中的 `update`/`counter` 必须落在该范围内或导入保留区；`counter` 必须是 Guest 可写内存。

`struct_size`/`abi_version` 不匹配 → 在装载/调用前失败；`GuestState` 大小、`AevCallResult` 大小和 AOT dylib 导出的 `aev_aot_abi_version` 都必须匹配（Guest ABI v1；callback ABI v2）。AOT dylib 必须导出 `aev_aot_run`、`aev_blocks`、`aev_block_count`、`aev_aot_abi_version`、`aev_input_hash`；任一缺失即 `ABI_MISMATCH`。bridge 还必须把 dylib 的 `aev_blocks`/`aev_block_count` 与本次 `Program` 的 AOT 块地址集合逐项比较，并把 `aev_input_hash` 与 COFF 字节哈希比较；同版本但过期/错配的 dylib 不得执行（避免“版本相同、代码不同”）。

调用约定与栈：

- 入口 `RSP = S` 满足 `S % 16 == 8`（模拟 CALL 已压入返回地址）；`[S]` 写 `AEV_GUEST_SENTINEL`；caller shadow space 精确为 `[S+8, S+0x28)`，fixture 在入口把 RBX 暂存到 `[S+8]`。至少 `[S, S+0x30)` 必须可读写并落在 Guest 内存内。
- 参数映射 RCX/RDX/R8/R9；返回 RAX；Guest 必须保留 RBX/RBP/RDI/RSI/R12–R15（fixture 的 MSVC 序言/尾声已验证）。
- 成功返回的条件是 `rip == AEV_GUEST_SENTINEL && rsp == S+8`（Guest 的 `ret` 已弹出哨兵）；bridge 在下一次调用前把 `rsp` 重设为 S 并重写哨兵。仅对 `OK` 做这个平衡检查；错误路径保留实际 RSP，不把错误覆盖成 `INTERNAL`。若 `aev_aot_run` 报告 0（哨兵）但 `rsp != S+8`，映射为 `AEV_BRIDGE_ERR_INTERNAL`，保留实际 `guest_rsp`，不提交 RAX/GUI 结果。`OK` 时 `guest_rip` 为哨兵、`guest_error` 为 0；步数上限沿用 `aev_aot_run` 的 1,000,000 块并映射为 `STEP_LIMIT`。
- 每次进入 Guest 前重置：所有 GPR、RIP、RFLAGS、RSP、`steps`、`error_code`、`error_pc` 和 per-call 的 AOT/JIT 计数；设置 `mem_base`/`mem_size`/`trace_enabled`。跨调用保留：Guest 内存内容、JIT 缓存、导入注册表、`call_index`/累计计数。`AevCallResult` 的 `aot_blocks`/`jit_*` 是本次调用增量，`total_*` 是累计值；`guest_rip` 在 `OK` 时为哨兵，在失败时为停止位置。
- `call_index` 从 1 开始，仅在真正进入 Guest 时递增；重入/错误线程/参数错误返回 0。`aev_bridge_destroy` 和 `aev_bridge_register_import` 不得在 `in_call` 中调用；`register_import` 在 `in_call` 时返回 `AEV_BRIDGE_ERR_REENTRY`，`destroy` 在 `in_call` 时返回 `AEV_BRIDGE_ERR_REENTRY` 且不释放（调用方必须避免）。`aev_bridge_guest_read`/`guest_write` 允许在 HostImport 回调内使用（同一线程、只做范围检查，不重入 Guest），但不得用于绕过 Guest 的 `aev_load/aev_store`。
- 调用创建线程之外 → `AEV_BRIDGE_ERR_WRONG_THREAD`。`require_main_thread=1` 时还要求 `pthread_main_np()` 为真（macOS 进程主线程）；C 的 GUI 路径必须设置该位，B 的无 GUI 测试在测试进程主线程运行。`in_call` 期间再次 `aev_bridge_call` → `AEV_BRIDGE_ERR_REENTRY`，不修改活动调用状态。

## 4. 退出原因与故障副作用

| 情况 | 状态 | 证据字段 |
| --- | --- | --- |
| Guest 执行到哨兵返回且 `rsp==S+8` | `OK` | `guest_rax`, `call_index` |
| Guest 访问越界/非法内存 | `MEMORY` | `guest_error=AEV_ERR_MEMORY`, `error_pc` |
| Guest 调用保留区未注册地址 | `UNKNOWN_IMPORT` | `error_pc`, `host_import_id=0` |
| `find_function_at(rip)==NULL` 且不在导入保留区；或函数内但非基本块边界；或 `rip==0`/`0xDEAD0000` | `NO_BLOCK` | `error_pc`, `guest_error` 可为 0/BAD_TARGET |
| 块存在但 AOT 表缺块，或 `jit_only` 块 JIT 编译/查找失败 | `JIT` | `guest_error=AEV_ERR_JIT`, `error_pc` |
| 步数上限 | `STEP_LIMIT` | `guest_error=AEV_ERR_STEP_LIMIT` |
| Host 回调返回非 OK/抛出异常 | `HOST`（或回调返回的已知状态） | `host_import_id`，不写 RAX |
| 重入 / 错误线程 / ABI 不匹配 | `REENTRY` / `WRONG_THREAD` / `ABI_MISMATCH` | 无 Guest 副作用 |

故障规则：故障指令本身及其后的 Guest 指令不提交；此前已完成的 Guest 指令副作用保留（与现有引擎一致）。分类规则：create 时 AOT 版本/块表/输入哈希不匹配一律 `ABI_MISMATCH`；运行到缺失 AOT 块才是 `JIT`。`0`/`0xDEAD0000` 的 runner 专用特判保留在 runner wrapper，bridge 将其映射为 `NO_BLOCK`，不得让普通未知目标落入 JIT 错误。`BAD_TARGET` 在 bridge 结果中归一为 `NO_BLOCK`，并保留 `guest_error/error_pc` 供诊断。`call [r9]` 的返回地址压栈发生在 dispatcher 之前，因此派发失败时这 8 字节写入已经发生：未知导入时 RSP 停在压栈后的位置，Host 回调失败时 dispatcher 已把 RSP 加 8；RAX 和后续 Guest 都不提交，下一次调用重设 RSP=S 并重写哨兵。bridge/GUI 在收到非 `OK` 后不得更新结果控件或提交业务结果；GUI 采用两阶段提交：HostImport 只把结果写入 pending，`aev_bridge_call` 返回 `OK` 后才更新控件。fixture 把计数器递增放在 helper 返回之后，因此未知导入/非法内存不会增加计数器；另加“helper 成功但随后 counter 指针非法”的故障用例，验证 pending 结果被丢弃、旧控件文本不变。

Host 异常边界：v0 只承诺捕获 C++ 异常并映射为 `HOST`；Host 回调不得抛出 Objective-C 异常（GUI 的 Python/ctypes trampoline 必须捕获 Python 异常并返回 `HOST`）。bridge 在 C ABI 边界捕获内部 C++ 异常，任何异常都不得穿过 generated ARM64 代码或 C ABI。`HOST` 时 `guest_error` 可为 0，`error_pc` 记录导入调用点/最近 Guest RIP。

## 5. AOT/JIT 共用核心

- 从 `runner.cpp` 抽取 `JitRuntime`（初始化、IR patch、ORC 编译、缓存、dispatch、ARM64 dump），runner 与 bridge 共用同一实现；不新增第三套 ORC dispatcher，不改变现有 AOT/JIT 语义。
- bridge 的 dispatcher 顺序：先 HostImport 注册表；再检查 `Program` 的函数/基本块：`find_function_at==NULL` 且不在导入保留区 → `NO_BLOCK`；函数内但非块边界 → `NO_BLOCK`；块存在但运行期缺 AOT 表或 JIT 编译/查找失败 → `JIT`；正常 `jit_only` 块走 `JitRuntime`。不能把普通未知目标误报为 JIT，也不能把 create 期产物错配留到运行期。
- 每次调用在 `AevCallResult` 记录 per-call 与累计的 `aot_blocks/jit_blocks/jit_compiles/jit_execs`；日志记录 AOT rip、JIT compile/exec rip、HostImport id/参数。日志是本轮调用诊断，不是通用 Trace 系统。
- AOT dylib 由同一 fixture 对象生成，必须 `file`/`otool -hv` 显示 arm64 Mach-O；JIT 需实际编译并执行，不能只打印计数。
- C 的 GUI 路径使用 `require_main_thread=1`；bridge 创建/调用必须在进程主线程。`compat`/`native` 后端不得互相创建对方控件；每次运行记录实际 ObjC 类名、`NSThread.isMainThread()` 和跨后端事件计数。强制 native 失败场景必须非零退出且不创建 compat 控件。

## 6. 验证方案

### A（本任务）

- 提交本契约、fixture 选择与指令盘点、Windows/Mac 同一机器码方案、失败/生命周期边界。
- compiler/platform 会签；reviewer 检查调用/错误边界可验证、范围未扩成完整 P1。
- 不在 A 写 bridge 或 GUI 代码。

### B（P0-CORE-BRIDGE-01）

- 新增 `aev_bridge.h/.cpp`、共享 `JitRuntime`、bridge 测试与构建目标；fixture 对象/源码/Windows 参考驱动进入 `spikes/p0-engine/testdata/callback/`。
- 无 GUI 测试至少覆盖：正常返回（三组输入，含独立输入）、重复调用 100 次、Host helper 实际被调、未知导入、非法内存、未支持重入、错误线程、Host 回调返回失败/异常、helper 后 counter 指针非法（两阶段提交的故障用例）、普通未知目标/函数内非边界/`0`/`0xDEAD0000`（NO_BLOCK）、测试注入 JIT 失败（JIT）、哨兵但 RSP 不匹配（INTERNAL）、AOT 版本/块表/输入哈希不匹配、错误配置/结果结构尺寸（INVALID_ARGUMENT/LOAD）、步数上限（大长度循环或等价的受控输入）；检查 Guest 计算结果与计数器。
- 证明 AOT 块执行 ≥1、JIT 编译 ≥1、JIT 执行 ≥1；首次调用 `jit_compiles==1`，后续 99 次为 0 且 `jit_execs` 每次增加；错误后下一次调用恢复成功。`aev_bridge_set_test_fault` 的 JIT 失败是注入的桥接映射测试，不替代真实 ORC 编译/执行证据；真实 JIT 证据仍来自 `0x140001020`。原 `p0_engine_closed_loop`、`p0_core_fix_semantics`、`p0_engine_empty_aot` 三组 ctest 不回归。
- 测试输入身份负例：用同一入口/块地址但改动一个立即数的对象生成第二个 AOT dylib，bridge create 必须因 `aev_input_hash`/块表不匹配返回 `ABI_MISMATCH`，不得执行。
- Windows 参考用同一 `.obj` 链接并运行，输出与 Mac 对照；记录对象/.text/驱动/参考输出哈希、命令、退出码。检查 Host 数据不重叠 `[stack_base, stack_top)`，并验证 callee-saved 寄存器与 `[S+8]` 的 RBX 保存路径。
- 证据必须绑定最终对象：当前最终 `.aevtext` 为 86 字节、27 条指令、5 块（4 AOT + JIT-only `0x140001020`）。早期 70 字节/23 指令的临时反汇编只作为选型过程，不进入最终报告。

### C（P0-UI-INT-01）

- 复用 compat/native 后端和同一 bridge dylib；同一 fixture 哈希与同一入口。先把 `UnsupportedApi`/共享常量抽到不导入 Unicorn 的模块，再让两个后端只依赖该模块。Host 创建窗口、输入框、按钮、结果区；按钮/文本事件把 UTF-16 输入写入 Guest 内存并调用 Guest；Guest 调 HostImport；Host helper 只把 pending 结果写入 Host 状态，`aev_bridge_call` 返回 `OK` 后才更新结果控件。
- `native` 用真实 AppKit 控件并在主线程；`compat` 用既有 compat 后端；两者不得静默互退。集成入口 `run_engine.py` 不导入 `guest_pe`/`unicorn`/`pefile`；旧 Unicorn 模式必须显式 `--legacy-unicorn` 并保留为历史对照。运行时断言 `sys.modules` 中无 `unicorn`/`guest_pe`，并对 `libaev_bridge.dylib` 执行 `otool -L` 检查无 Unicorn/pefile 依赖；记录 `backend_proof`（实际 ObjC 类名、`NSThread.isMainThread()`、跨后端事件为 0）。Host 文本/GuestCtx/计数器必须位于 `stack_base` 以下，配置检查保证至少有 64 KiB 数据区。
- 至少三组输入/操作序列，其中一组为未参与准备的独立输入；同一 session 连续 ≥100 次回调，验证 Guest 计数器、JIT 缓存和错误后恢复；正常链记录 AOT/JIT 实际执行；未知导入/非法内存/未支持重入显式失败，失败后不更新结果控件。增加一次故障后成功恢复正常路径，以及一次 helper 成功但随后 Guest 故障、旧控件文本保持不变的用例。
- 自动事件与物理键鼠/焦点/输入法验收分开；无交互桌面时由用户按明确步骤验收。强制 native 失败场景必须非零退出且不创建 compat 控件。

## 7. 已知限制与未决项

- v0 Guest 地址是 session-local 映射，不承诺跨进程/持久化；P1 再冻结独立 Guest 地址空间。
- 无通用 PE/Win32、TLS/SEH/unwind、线程、模态、原子/TSO、代码失效、SIMD/浮点；不支持 Objective-C 异常跨回调边界。
- 不做性能比较；100 次回调只是稳定性 smoke。
- 旧 UI DLL 的 Unicorn 路径仍是非产品历史对照；本轮不把其指令计数当作新核心证据。
- `compat` 仍是 P0 语义 harness，不是最终兼容运行库；D3 选型未决。
- Windows 可见窗口/物理输入仍需人工验收；自动截图/日志不能替代。

## 8. 会签记录

| 角色 | 结论 | 证据 |
| --- | --- | --- |
| compiler | SIGN-OFF：v2 committed accepted（HEAD afff569） | ses_e5452516effeU230iYUz7Sqarq |
| platform | SIGN-OFF：v2 committed accepted（HEAD afff569） | ses_e5452516cffeTcnkrlZmejylWj |
| reviewer | SIGN-OFF：accepted（最终代码 HEAD afff569） | ses_e53bdd5a4ffencHGIFWSPUb7fE |

修订摘要（v2）：create 改为状态返回；HostImport 改为状态+`out_rax`；导入 ID 由 Host 提供并校验；增加 `guest_info`（含栈基址/栈顶）；明确 RSP=S+8、shadow `[S+8,S+0x28)`、哨兵+错误 RSP=INTERNAL；明确 reset/preserve、per-call/累计计数（含 `total_jit_blocks`）、`call_index`；明确 NO_BLOCK/JIT 分类和 AOT 块表+输入哈希比对；两阶段提交；错误 RSP 残留；C++/ObjC 异常边界；Unicorn 依赖抽取和 backend proof；允许回调内 guest_read/write；fixture helper 固定返回 op；增加测试用 JIT/INTERNAL 故障注入；create/destroy 状态可观察；`trace_aot`/`config_size` 纳入 v2。
