# P0-PE-DLL-01 最小模块/地址/内存/artifact 契约（v3）

日期：2026-11-06。状态：v3 实施冻结候选；已吸收 binary/compiler/platform/reviewer 两轮会签意见。适用范围：Windows x86-64 PE32+ DLL（单个主模块 + 声明 HostImport）在 macOS ARM64 共享 AOT/JIT 核心中的有界装载与执行。不是 P1 全量 ABI；不覆盖 EXE 启动、DllMain/CRT/TLS、异常展开、完整 Win32、通用依赖图。

## 1. ABI 与取代关系

| 项 | 本轮值 | 说明 |
| --- | --- | --- |
| `AEV_GUEST_ABI_VERSION` | 2 | `GuestState` 增加受检查内存模型、故障字段与 transfer 字段；旧值 1 明确拒绝 |
| `AEV_GUEST_STATE_SIZE` | 296 | 37 个 `uint64_t`；`sizeof(GuestState)` 与导出符号双重校验 |
| `AEV_CALLBACK_ABI_VERSION` | 3 | 布局变化，不能沿用 callback ABI 2；COFF bridge 同步升 3 |
| `AEV_ARTIFACT_ABI_VERSION` | 2 | artifact 增加模块/映射/导入/host 绑定身份 |
| `AEV_TRANSLATOR_ABI_VERSION` | 2 | PE 前端/模块 manifest/块身份扩展 |
| `AEV_TRACE_SINK_ABI_VERSION` | 1 | bridge/AOT runtime 的进程内 trace sink，不变 |
| `AEV_TRACE_FILE_ABI_COFF` | 3 | COFF trace 文件 schema；版本字段同步到 3/2/2/2 |
| `AEV_TRACE_FILE_ABI_PE` | 4 | PE trace 文件 schema；增加模块身份 |
| `AevMemRegion` | 32 字节 | 32+8 对齐；见 §3.1 |

本契约**取代** `docs/p0/CALLBACK-ABI-20261105.md` 中 GuestState=256/Guest ABI=1/ABI 2 的旧值，`docs/p0/TRACE-AOT-CONTRACT-20261106.md` 中单一 trace ABI/旧 artifact ABI 的旧值，以及 `docs/p0/PROMOTION-PROVENANCE-20261106.md` 中 256/ABI1 的 publisher 断言。旧文档保留历史结论并加取代说明。所有 producer/consumer/test 必须同步；旧 ABI 产物明确拒绝。

## 2. 地址与身份分类

| 名称 | 定义 | 是否持久化 |
| --- | --- | --- |
| file offset | 原始 DLL 文件内偏移 | 仅解析诊断 |
| RVA | 相对 preferred image base 的模块内偏移 | 是；块身份 |
| preferred ImageBase | OptionalHeader.ImageBase | 是 |
| Guest load base | 本次映射实际选择的 Guest 基址，64 KiB 对齐 | 是；artifact 绑定 |
| Guest VA | `load_base + RVA`，或声明的 stack/thunk Guest 地址 | 是；块/入口/导出 |
| Host backing address | 承载 Guest 字节的 Host 内存地址 | 否；只在运行时 |
| Host ARM64 function address | AOT/JIT 生成函数地址 | 否；只在运行时表 |

规则：

1. Guest VA 只有通过 §3 的受检查内存模型才能换算成 Host backing address；不得 `(uintptr_t)guest_va` 直接解引用。
2. IAT 只写 Guest thunk VA；不得写 Host 原生函数指针。
3. `GuestState` 不持久化 Host 指针。`reserved1/2/3` 只在本次映射生命周期内保存区域表指针/数量/image base；每次 Guest 调用进入前重新填入，调用退出、destroy、trace 输出和 artifact 中均为 0/不出现。
4. artifact 不持久化 Host 指针或 Host AOT/JIT 函数地址；块表只存 Guest RIP 与 AOT 符号地址（符号地址由 dylib 加载时重定位）。

### 2.1 模块身份

- `module_kind = "pe32+amd64-dll"`（COFF 为 `"coff"`）
- `module_file_sha256` / `module_file_size`：原始 DLL 完整文件；绝不修改输入文件。
- `preferred_base`、`load_base`、`image_size`（SizeOfImage）
- `image_sha256`：按 PE 语义映射、完成 DIR64 relocation 与 IAT 绑定后的 `SizeOfImage` 字节 SHA-256。
- `import_binding_sha256`：规范化 import 列表 + thunk 表字节的 SHA-256；见 §4.3。
- `host_binding_sha256`：规范化 registry 全部声明的 SHA-256；见 §5.1。
- `entry_exports_sha256`：排序、长度前缀的 named export 名列表 SHA-256；见 §4.6。
- `thunk_base`、`import_count`、`relocs_stripped`
- `entry_exports`（排序）与 `entry_export`（主入口）；`entry_export` 的 SHA-256 写入既有 `aev_function_name_sha256`。
- `guest_abi`、`artifact_abi`、`translator_abi`、`target_triple`

`module_instance_id = (module_file_sha256, load_base, image_sha256, import_binding_sha256, host_binding_sha256, guest_abi)`。

### 2.2 导出、函数范围与块身份

- named export：`name`、`rva`、`guest_va = load_base + rva`、`ordinal`。ordinal-only export 不翻译，但在 `--info`/manifest 的 `ordinal_only_exports` 中列出；模块必须至少有一个 named export。
- 函数范围：`.pdata` RUNTIME_FUNCTION `[BeginAddress, EndAddress)`；翻译根必须正好等于 BeginAddress。Begin/End 必须在同一个 executable section 内；非 executable/跨 section 的 `.pdata` 明确拒绝。
- 尾部填充裁剪：先按 `.pdata` 原长度解码，再按**完整指令边界**裁剪尾部 `INT3`/`NOP`；仅当最后一个非填充指令是 `RET`/`JMP` 时裁剪。`code_size` 为裁剪后长度，`pdata_begin/pdata_end/pdata_size` 保留原值。裁剪后为 0 拒绝。
- 块身份：`rip`（Guest VA）、`rva`、`size`、`sha256`（**relocation + IAT 绑定后的 image 字节** `[rva, rva+size)`）。load base 改变则 rip/绝对字节改变，必须重建 artifact。
- 同一 artifact 不支持重绑定到另一 load base；基址不匹配在进入 Guest 代码前拒绝。

### 2.3 共享 Program 接口（禁止第二套 decoder/emitter）

- 构造一个合成 `CoffObject`：
  - `sections[0].data` = `SizeOfImage` 字节的 post-relocation/post-IAT image；`name=".pe_image"`；`virtual_size=SizeOfImage`；
  - `content_sha256` = 原始 DLL SHA-256；`content_hash` = 原始文件 FNV-1a；`content_size` = 原始文件大小（`CoffObject` 新增字段，`load` 时填充）；
  - 不伪造 COFF 符号表；函数由显式 `FunctionSpec` 给出。
- 新增 `FunctionSpec { std::string name; int section_number; uint64_t guest_va; uint32_t rva; uint32_t size; }` 和 `build_program_from_specs(CoffObject&&, const std::vector<FunctionSpec>&, const std::set<uint64_t>* external_jmp_targets, Program*, error)`。
- `GuestFunction.address = guest_va`，`section_number = spec.section_number`，`section_offset = rva`，`bytes = image[rva, rva+size)`。
- `block_identity` 继续按 `fn.section_number` 取 `program.object.section(...)` 并在 `section_offset` 取哈希；PE 固定 section 1/rva，COFF 使用真实 section/符号值。AOT/JIT/trace/manifest/artifact 全部一致。
- 现有 `build_program(CoffObject&&, names, addresses, ...)` 改为构造 `FunctionSpec`（section_number=sym->section_number，rva=sym->value，size=section.data.size()-sym->value）并调用同一 builder；COFF 行为保持不变。
- `external_jmp_targets` 仅 PE 传入全部函数 Guest VA；COFF 传 null（保持“JMP 目标必须在本函数内”的严格行为）。PE 的直接 `JMP` 目标若在集合内则允许跨函数尾跳，不插入本地 leader；`JCC` 目标仍必须在本函数内。

## 3. Guest 内存模型 v2

### 3.1 区域

`GuestState` 通过 `reserved1`（`AevMemRegion*`）和 `reserved2`（数量）引用运行时区域表；`reserved3` 记录 image base（诊断）。`reserved0` 继续是 trace sink。

```c
typedef struct AevMemRegion {
    uint64_t guest_base;   /* 区域起始 Guest VA，页对齐 */
    uint64_t size;         /* 区域大小，页对齐 */
    uint64_t host_base;    /* Host backing storage；仅运行时 */
    uint32_t prot;         /* 1=R, 2=W, 4=X */
    uint32_t reserved;     /* 0 */
} AevMemRegion;            /* sizeof == 32 */
```

- 区域互不重叠、页对齐、`size > 0`、`guest_base + size` 不溢出。
- 访问：检查 `len` 非 0、`addr+len` 不溢出；找到包含整个 `[addr,addr+len)` 的单一区域（跨区域拒绝）；检查 R/W/X；`host = host_base + (addr - guest_base)`。
- `aev_load`/`aev_store` 只经该算法；`aev_guest_require_exec(s, rip, len)` 供 dispatcher/AOT runtime 检查 X 权限并设置故障字段。不得用 `MAP_FIXED` 覆盖 Host 已有映射来凑地址。
- 兼容模式（仅既有 COFF 回归）：`reserved1==0 && reserved2==0 && mem_base/mem_size` 有效时按 RWX 单区域 identity 模式处理；`aev_guest_require_exec` 直接返回成功。PE 路径必须使用区域表；bridge/runner 不得把该模式用于 PE artifact。

### 3.2 GuestState v2 字段顺序

保留现有 32 个字段顺序，追加：

```
AEV_FIELD(mem_fault_kind)
AEV_FIELD(mem_fault_addr)
AEV_FIELD(mem_fault_size)
AEV_FIELD(mem_access)
AEV_FIELD(pending_transfer)
```

`pending_transfer`：0=none，1=call，2=jmp，3=ret，4=jcc。每个块 terminator 在写 `rip` 前写该字段；**AOT/JIT 块入口清零**（在 import 检查之后、执行块之前），import thunk 仅接受 1，否则拒绝（防止 `jmp [IAT]` 尾调用错误 pop 返回地址）。

### 3.3 区域布局（PE）

| 区域 | Guest 范围 | prot | 备注 |
| --- | --- | --- | --- |
| headers | `[base, base+align_up(SizeOfHeaders, page))` | R | 复制 `min(SizeOfHeaders, file_size)`，其余零填充；`align_up(SizeOfHeaders,page) <= 首个 section VA` |
| section N | `[base+VA, base+VA+align_up(effective_VirtualSize, SectionAlignment))` | characteristics 映射 | `effective_VirtualSize = VirtualSize ? VirtualSize : SizeOfRawData`；复制 `min(SizeOfRawData, effective_VirtualSize)`，其余零填充；W+X 拒绝 |
| thunk table | `[thunk_base, thunk_base+align_up(import_count*16, page))` | R+X | 每 16 字节：`[0]=自身 thunk VA`，`[8]=0`；不承载 Host 指针 |
| stack | `[stack_base, stack_base+align_up(stack_size, page))` | RW | `stack_size>=64 KiB`；`stack_top = stack_base+size`；`entry_rsp = stack_top - 0x108` |

`thunk_base = align_up(load_base + SizeOfImage, 0x1000)`；`import_count==0` 时 `thunk_base=0`，无 thunk 区域。`stack_base = align_up(max(thunk_base + thunk_size, load_base+SizeOfImage), 0x1000)`；与 image/thunk 不重叠。image 缓冲区在复制前全部零初始化；headers 尾部、节间空洞、VirtualSize 到对齐边界的 slack 都为零。`SectionAlignment % 0x1000 == 0`；`FileAlignment` 为 2 的幂且在 0x200..0x10000；`SizeOfHeaders % FileAlignment == 0`。

### 3.4 故障语义

- 失败设置 `error_code = AEV_ERR_MEMORY`（既有 dispatcher 停止语义），并记录：
  - `mem_fault_kind`：1=unmapped，2=read-permission，3=write-permission，4=exec-permission，5=range/overflow，6=invalid-size
  - `mem_fault_addr`、`mem_fault_size`、`mem_access`（1=read，2=write，3=exec）
  - `error_pc`：load/store 为故障指令 Guest VA；exec 为被尝试取指的 Guest VA。
- 生成代码在每个 helper 调用后检查 `error_code` 并跳到块尾 fault 出口；故障指令本身及同块后续指令不提交副作用。之前已完成块的副作用保留。
- Host import 失败：`AEV_ERR_HOST = 7`；不写 `rax`，`error_pc` 为 thunk VA，`rip/rsp` 保持失败时值。

## 4. PE 装载算法（有界）

### 4.1 上限

文件 <= 64 MiB；`SizeOfImage <= 256 MiB` 且为 `SectionAlignment` 倍数；`SizeOfHeaders <= 1 MiB`；section <= 96；data directory <= 16；export <= 4096；ordinal-only export <= 4096；`.pdata` 条目 <= 4096；relocation site <= 1,000,000；import descriptor <= 256；唯一 import <= 256；IAT slot <= 4096；函数 <= 64；总代码 <= 1 MiB；单函数 <= 256 KiB；导出/导入名 <= 512 字节；模块名 <= 256 字节。

### 4.2 解析与映射

1. 读文件，计算 SHA-256/FNV。解析 DOS/PE/optional/section headers；要求 `Machine=0x8664`、`Magic=0x20B`、DLL 标志、非 32 位；拒绝非 PE/非 DLL。
2. 校验 `SizeOfImage/SizeOfHeaders/SectionAlignment>=4KiB 且 %4KiB==0/FileAlignment`；headers 结束 <= 首个 section VA；section 表在文件内；每节 `VirtualAddress % SectionAlignment == 0`、`VirtualAddress >= SizeOfHeaders`、`VirtualAddress+align_up(effective_VirtualSize, SectionAlignment) <= SizeOfImage`、raw 范围在文件内；虚拟范围不重叠；拒绝 W+X、越界、重叠。
3. 拒绝：非空 TLS 目录、delay import、bound import、CFG（`GUARD_CF` 或 load config GuardFlags）、`AddressOfEntryPoint != 0`（DllMain 不执行）、ordinal import、API set（`api-ms-*`/`ext-ms-*`）、forwarder export、非 executable section 的 export root、跨 section 的 `.pdata`。
4. export：数组/字符串边界、ordinal 范围、重复名、forwarder 明确拒绝；至少一个 named export；ordinal-only 列表记录不翻译。
5. `.pdata`：12 字节整除、Begin<End、范围在 image 内、Begin/End 在同一 executable section、不重叠；翻译根/直接调用目标必须由 `.pdata` 覆盖且等于 BeginAddress。
6. relocation：只支持 `DIR64=10` 与 `ABSOLUTE=0`；每个 block `VA+SizeOfBlock` 在目录/文件内、`SizeOfBlock>=8`、`(SizeOfBlock-8)%2==0`；每个 site `site_rva+8 <= SizeOfImage` 且落在已映射 section；`delta = load_base - preferred_base`。`RELOCS_STRIPPED` 定义为 Characteristics bit 0x0001 或 relocation directory size==0；stripped 仅当 `load_base == preferred_base` 允许，manifest 标记 `relocs_stripped=true`；否则拒绝。
7. 映射：`std::vector<uint8_t> image(SizeOfImage, 0)`；headers 复制；每节复制 `min(SizeOfRawData, effective_VirtualSize)`；应用 DIR64 relocation；解析 import 并绑定 IAT。
8. import：by-name 普通 import；唯一 `(module,symbol)` 分配一个 16 字节 thunk，IAT 写 `thunk_base + index*16`；重复 IAT slot 共享同一 thunk；按 PE import descriptor/thunk 顺序编号；描述符/名称/ILT/IAT 全部边界检查；零终止。
9. 计算 `image_sha256`、`import_binding_sha256`、`host_binding_sha256`、`entry_exports_sha256`；构建区域表；返回不可变 `Module`。任何失败释放本次全部 Host backing，不产生半装载模块；溢出检查覆盖 `load_base+SizeOfImage`、`thunk_base+thunk_size`、`stack_base+stack_size`、`rva+size`、`site+8`。

### 4.3 身份哈希字节格式

所有整数 little-endian；字符串为 UTF-8 字节。比较/排序按字节序。

`import_binding_sha256`：

```
"AEVPEIMPORT1\n"
u32 count
for each unique import in PE descriptor/thunk order:
  u32 len(module) + module
  u32 len(symbol) + symbol
  u64 thunk_va
for each thunk i in order:
  u64 (thunk_base + i*16)
  u64 0
```

空表：header + u32 0，再没有 thunk 字节。

`entry_exports_sha256`：

```
"AEVPEENTRY1\n"
u32 count
for each named export sorted by name bytes:
  u32 len(name) + name
```

`host_binding_sha256`：

```
"AEVPEHOST1\n"
u32 count of all registry entries (duplicate keys rejected)
for each entry sorted by (module bytes, symbol bytes):
  u32 len(module) + module
  u32 len(symbol) + symbol
  u64 host_import_id
  u32 args
  u32 len(abi) + abi
```

空表：header + u32 0。registry 多余条目也进入哈希（保守绑定）；`registry_file_sha256` 另记于 manifest 作为证据，不进入 `module_instance_id`。

### 4.4 代码发现

- 根：CLI 声明的 named exports（至少一个）。
- 从根函数解码；`CALL/JMP` 直接立即数目标：当前函数内为本地控制流；同 image 可执行段且由 `.pdata` 覆盖则入队；可执行段但无 `.pdata` 覆盖拒绝；非可执行段拒绝。PE 的 `JMP` 可指向已发现函数起点（`external_jmp_targets`）；`JCC` 必须本地。
- 上限见 §4.1；超出拒绝。函数名导出名优先，其余 `sub_<rva-hex>`；重复名/地址拒绝。
- 复用 `build_program_from_specs`，不复制 decoder/emitter/块构建。
- 间接 `CALL/JMP` 目标若不在已发现 Program 的块边界：明确 `AEV_ERR_NO_BLOCK`，本轮不按需建函数。验收的 JIT 块是**已发现但未 AOT 的合法块**，不是任意未知目标。

### 4.5 明确拒绝清单

非 PE32+/AMD64、非 DLL、W+X、越界/重叠节/headers、非空 TLS/delay/bound import、DllMain、CFG、ordinal/API set import、forwarder export、非 executable/跨 section 的 export/pdata、未知 relocation、`.pdata` 缺失/重叠、直接目标越界、未声明/未知 HostImport、IAT/reloc/字符串越界、上限超出、artifact 模块/基址/ABI/image/import/host/entry/块表不匹配。

## 5. 导入 registry 与调用

### 5.1 registry 格式

声明文件为 `.aevimports` 文本（UTF-8）：

```
# module|symbol|host_import_id|args|abi
aevhost.dll|AevUpdate|1|2|win64-u64
```

- module/symbol 按 PE 导入字符串精确匹配；host_import_id 为 1..2^32-1，单次装载内唯一；args 为 0..4；本轮 abi 只接受 `win64-u64`（整数/指针，RAX 返回）。
- 重复 `(module,symbol)` 拒绝；每个 PE import 必须在 registry 声明，否则 translator/runner 在映射/执行前失败；registry 多余条目允许但进入 `host_binding_sha256`。
- runner 的 Host 表项签名固定为：
```c
typedef int (*AevPeHostImportFn)(void *ctx, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t *out_rax);
```
  返回 0=OK；非 0 映射 `AEV_ERR_HOST`，不写 `rax`；C++ 异常由 runner 捕获并映射 HOST。`args` 是声明的 Guest 参数个数（宿主只使用前 `args` 个；runner 仍传满四个）。
- translator 需要 `--registry <file>` 生成 artifact；runner 需要同一文件并在装载时比较 `host_binding_sha256`；不同 registry 明确拒绝。`host_import_id` 只是运行时诊断 ID，不是 Guest 可见身份。

### 5.2 thunk / IAT

- IAT slot 写入 `thunk_base + index*16`；dispatcher 只接受精确 thunk entry。
- 落在 `[thunk_base, thunk_base+import_count*16)` 但不是 entry 的 rip 一律 `AEV_ERR_BAD_TARGET`/`UNKNOWN_IMPORT`，绝不进入 AOT/JIT 执行 thunk 数据。
- thunk 只承载 Guest VA；Host 实现表在 runner 内存，按 thunk index/host_import_id 查找。

### 5.3 Guest → Host import

1. 要求 `pending_transfer == 1`（CALL）；否则 `AEV_ERR_BAD_TARGET`（尾 jmp 不支持）。
2. 经受检查内存读 `[rsp]` 返回地址，`rsp += 8`；失败按内存故障处理。
3. 调用 registry Host 实现，参数 `rcx/rdx/r8/r9`；成功写 `rax`、`rip=return`、`pending_transfer=0`；失败 `error_code=AEV_ERR_HOST`，不写 `rax`。
4. trace 记 `host_import` 事件（id + thunk index）。

### 5.4 Host → Guest export 调用

- `stack_top = stack_base + align_up(stack_size, page)`；`entry_rsp = stack_top - 0x108`（保证 `entry_rsp % 16 == 8` 且 `[entry_rsp, entry_rsp+0x28)` 在 RW stack 内）；`[entry_rsp] = AEV_RETURN_SENTINEL`；`[entry_rsp+8, entry_rsp+0x28)` 清零；参数 RCX/RDX/R8/R9，64 位；返回 RAX。
- 每次调用重置 RSP、哨兵、寄存器/错误/统计，并重新填入 `reserved1/2/3`；模块全局 image/thunk/stack 状态保留。
- 成功条件：`rip == sentinel && rsp == entry_rsp + 8 && error_code == 0`；否则 `AEV_ERR_INTERNAL`。
- 最多 4 个整数/指针参数；32 位参数由调用方零扩展；不支持浮点/向量/结构体/varargs/栈参数；遇到声明之外的输入由调用方/registry 边界拒绝，不声称运行时可检测任意 ABI 误用。

## 6. runner 接口与生命周期

### 6.1 CLI

```text
aev-pe-runner --dll <path> --registry <path> --load-base <hex>
              [--aot <dylib>] [--entry-export <name>]
              [--call <export>[:a0,a1,a2,a3]]...
              [--reload N] [--expect-error] [--trace <path>] [--scenario <name>]
              [--info] [--dump-image <path>] [--dump-blocks <path>]
              [--dump-relocations <path>]
```

- `--aot` 缺省为 JIT-only；存在时必须校验 artifact 全部身份。`--load-base` 与 artifact 的 `load_base` 必须一致；只给一个时以 artifact 为准并打印。`--entry-export` 必填（用于 trace/`aev_function_name_sha256`），且必须是 named export。
- `--call` 可重复；参数缺省 0。`--reload N` 对每次迭代重新 load→call list→destroy；每次使用新 handle generation。`--expect-error` 仅用于负例：任一 call 失败时进程仍返回 0，但 JSON `expected_error=true` 且必须包含失败细节；没有失败则返回非 0。
- 输出 JSON 到 stdout；stderr 只放诊断。JSON 包含：`status`/`status_name`/`error`、`module`（kind/file sha/size/preferred/load/image size/image sha/import/host/entry sha/thunk base/import count/relocs stripped）、`sections`、`exports`、`ordinal_only_exports`、`imports`、`regions`（只含 Guest base/size/prot）、`relocations`（count/delta/first/last）、`blocks`（rip/rva/size/sha256/aot_selected）、`aot`（sha/table count/all count）、`calls[]`（export/args/status/status_name/rax/guest_error/error_pc/mem_fault_kind/addr/size/access/aot_blocks/jit_blocks/jit_compiles/jit_execs/steps）、`host_imports`（id/thunk index/count）、`reload_iterations`、`stale_handle`。
- 输出只含 Guest VA/模块身份；不得输出 Host backing 或 Host AOT/JIT 函数地址。`--dump-image` 写 `SizeOfImage` 字节（QA 核对 relocation）；`--dump-blocks`/`--dump-relocations` 写 JSON。

### 6.2 handle 与状态码

- opaque handle 为 64 位 token：`slot+1 | (generation << 32)`；slot 0..7，generation 从 1 开始，回绕跳过 0。`slot=0` 永不分配，token 0 为无效。
- destroy 释放模块资源、清空 slot 数据并递增 generation；旧 token 的 `call`/`info` 返回 `STALE_HANDLE`，不解引用已释放内存。stale destroy 返回 `STALE_HANDLE`；slot 满/非法 slot 返回 `INVALID_ARGUMENT`。
- runner 状态码：0 OK、2 INVALID_ARGUMENT、3 LOAD、4 ABI_MISMATCH、5 UNKNOWN_IMPORT、6 MEMORY、7 JIT、8 STEP_LIMIT、9 STALE_HANDLE、10 REENTRY、11 HOST、12 INTERNAL。
- 生命周期：`load → resolve registry/imports → resolve export → call N 次 → destroy`。活动调用中 load/destroy/再次 call 返回 `REENTRY`；HostImport 回调中再次 call 同样 `REENTRY`。失败装载释放本次全部区域/dylib/JIT/registry。
- 单线程、单模块实例；不承诺运行中重装、并发失效、跨模块自动闭包。

## 7. artifact 绑定

AOT dylib 导出（C 类型；数组按 PE import 顺序）：

```c
const char     aev_module_kind[];              /* NUL 结尾，最长 32 字节 */
const uint64_t aev_module_file_size;
const uint64_t aev_preferred_base;
const uint64_t aev_load_base;
const uint64_t aev_image_size;
const uint8_t  aev_image_sha256[32];
const uint8_t  aev_import_binding_sha256[32];
const uint8_t  aev_host_binding_sha256[32];
const uint8_t  aev_entry_exports_sha256[32];
const uint64_t aev_thunk_base;
const uint64_t aev_import_count;
const uint64_t aev_import_thunks[aev_import_count]; /* thunk VA, import 顺序 */
const uint64_t aev_relocs_stripped;
```

保留既有 `aev_input_sha256`（= module_file_sha256）、`aev_input_hash`、`aev_entry_addr`、`aev_function_name_sha256`（= entry_export）、`aev_all_blocks[]`、`aev_blocks[]`、`aev_aot_abi_version`、`aev_guest_state_size`、`aev_artifact_abi_version`、`aev_translator_abi_version`、`aev_target_triple`、`aev_aot_run`。

- COFF artifact 使用 `module_kind="coff"`，module_file_size=COFF 文件大小，其余 module 字段为 0/空数组，`relocs_stripped=0`。COFF bridge 只接受 `"coff"`；PE runner 只接受 `"pe32+amd64-dll"`。
- PE artifact 绑定一个 `(module_file_sha256, load_base)`；runner 必须以 artifact 的 load_base 装载；模块/基址/ABI/image/import/host/entry exports/块表任一不一致，在进入 Guest 前失败。
- 原始 DLL、base artifact、supplemented artifact 都保留；发布用共享 `publish_outputs` 的暂存/回滚，不覆盖 base。输入文件从不修改。

## 8. Trace / provenance

### 8.1 Trace file ABI

- COFF：`trace_abi=3`，字段版本 `callback_abi=3, guest_abi=2, aot_artifact_abi=2, translator_abi=2`；schema 沿用 v1 的 header/events，旧 `trace_abi=1` 明确拒绝。
- PE：`trace_abi=4`，header：

```json
{"record":"header","trace_abi":4,"module_kind":"pe32+amd64-dll",
 "input_sha256":"<module file sha>","input_fnv":"0x...",
 "module_file_size":12345,"preferred_base":"0x180000000","load_base":"0x190000000",
 "image_size":...,"image_sha256":"<64 hex>","import_binding_sha256":"<64 hex>",
 "host_binding_sha256":"<64 hex>","entry_exports_sha256":"<64 hex>",
 "entry_exports":["AevDllCompute",...],"entry":"AevDllCompute","entry_addr":"0x...",
 "entry_function_sha256":"<64 hex>","offset_kind":"rva",
 "guest_abi":2,"aot_artifact_abi":2,"translator_abi":2,"callback_abi":3,
 "target":"arm64-apple-macosx14.0.0","translator_version":"aev-translate-v2",
 "trace_tool_version":"aev-pe-trace-v1","aot_sha256":"<base dylib sha or zeros>",
 "all_blocks":["0x..."],"aot_selected":["0x..."]}
```

事件沿用 `aot/jit_compile/jit_exec/host_import/dispatch_miss/call_end`；块事件包含 `rip`、`offset`（RVA）、`size`、`block_sha256`。`all_blocks` 为实际翻译程序全部块，`aot_selected` 为实际 base artifact 表。JIT-only 运行 `aot_sha256` 为 64 个 0、`aot_selected=[]`。

### 8.2 supplement / publisher

- 共享 `aot_supplement.py` 增加 `--input-kind {coff,pe} --input <file> [--load-base --export ... --entry-export --registry]`；不新增第三套补充器。
- PE 模式必须重新调用 translator 重建 post-relocation/IAT image，核对 header 的 module/file/preferred/load/image/import/host/entry exports identity、`all_blocks`、`aot_selected`、每块 `(rip,rva,size,sha256)`；不能直接信任 trace 哈希。`--base-aot` 必选；实际 base 的 `aot_sha256`/`selected` 逐项相等。
- promoted/demoted 由实际 base selected 与实际执行并集计算；发布用共享 `publish_aot.py`。PE manifest 必须包含 `module_kind/module_file_sha256/module_file_size/preferred_base/load_base/image_size/image_sha256/import_binding_sha256/host_binding_sha256/entry_exports_sha256/entry_exports/entry_export/thunk_base/import_count/imports/relocs_stripped`；publisher 校验 actual base/new artifact 的这些字段和 selected/all 表。
- COFF trace ABI 3 继续由同一工具处理；PE/COFF 混用拒绝。

## 9. 兼容迁移清单

| 消费者/文件 | 旧 | 新 | owner |
| --- | --- | --- | --- |
| `include/aev_guest.h`/fields.def | 32 字段/256/ABI1 | 37 字段/296/ABI2 | compiler |
| `include/aev_artifact.h` | artifact/translator 1，单一 trace ABI | 2/2，sink 1 + file 3/4 | compiler |
| `include/aev_bridge.h`/`aev_bridge.cpp` | callback 2/256 | callback 3/296，module_kind=coff 校验 | platform |
| `src/coff.hpp/cpp` | 无 content_size | `content_size` 填充 | compiler |
| `src/translator.*` | build_program only | `FunctionSpec`/`build_program_from_specs`/external jmp | compiler |
| `scripts/aot_artifact.py` | ABI1/256/旧字段 | ABI2/296 + module 字段 | compiler/qa |
| `scripts/aot_supplement.py`/`publish_aot.py` | COFF-only | COFF ABI3 + PE ABI4，新身份 | compiler/qa |
| `spikes/p0-ui/harness/engine_host.py`/`engine_bridge.py` | ABI1/256 | ABI3/296，等价断言 | platform |
| `run_trace_aot_test.sh` 负例 | 把 2 当非法 | 改成 1/3 等等价非法值 | qa |
| `aev_bridge_test.cpp` | 256 | `sizeof(GuestState)`/296 等价断言 | platform |

原 5 个 ctest、Trace/AOT 回归、六组 GUI 回归在全新构建重建 artifact 后运行；旧 ABI1 artifact/trace 明确拒绝。不能删除或弱化测试，只做等价断言替换。

## 10. PE AOT/JIT coverage 与反替代

- PE translator 默认所有已发现块 AOT；`--aot-include-rip`/`--aot-exclude-rip` 按 Guest RIP 显式选择。测试基线必须至少排除一个合法块，实际运行产生 `jit_compile`/`jit_exec`；supplement 后该块进入 selected，新进程只产生 `aot` 事件、零 JIT。
- 转换输入只依赖最终 DLL + registry/load base；不得要求原 COFF/PDB/C 源码，不得用 Host 重写业务函数、Unicorn/Rosetta 或预置机器码替代 DLL 实际字节。
- 测试必须核对 DLL/helper SHA、PE headers/sections/imports/exports/relocations、实际 image/relocation 字节、AOT/JIT 事件、HostImport 参数/返回值、全局状态与副作用；不能用实现者自述代替。
- Windows 系统 loader 跑同一固定 DLL；Mac 用同一 DLL 字节。Windows 参考不提供指令 Trace。
- `native` 不得静默回退 `compat`；PE 驱动 GUI 不在本轮，现有双 GUI 链保持。

## 11. 本轮明确未实现

通用 EXE/进程启动、完整 Win32/消息泵、真实第三方应用移植、TLS callbacks、DllMain/CRT、SEH/unwind、COM、线程、delay/API set/forwarder/ordinal import、同一 artifact 跨 base 复用、运行中重装/并发失效、持久 JIT 缓存产品化、打包/分发、性能结论。间接目标按需建函数/动态 `.pdata` 发现也不在本轮。
