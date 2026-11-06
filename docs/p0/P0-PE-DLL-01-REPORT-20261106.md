# P0-PE-DLL-01 报告：最小 PE32+ DLL 装载与共享 AOT/JIT 执行

日期：2026-11-06。状态：**本轮有界任务 PASS，P0 整体未完成**。分支 `p0/pe-dll-01`，HEAD `f864d97aa1b3bd58c678744f17f1ffb4e254fedb`。基线 `aed3b46`；main 仍 `c91a3f7`，未合入、未 push、未删除工作树。

本报告只证明：一个固定自编 Windows x64 PE32+ DLL（含全局数据、DIR64 指针、一个声明 HostImport）在 macOS ARM64 共享 AOT/JIT 核心中的有界装载、重定位、导入绑定、执行、Trace→离线补充→新进程 AOT，以及原有 COFF/GUI 回归。不证明通用 EXE、完整 Win32、真实应用、TLS/CRT/SEH/COM、打包或性能。

## 1. 契约与 ABI

契约：[PE-DLL-CONTRACT-20261106](PE-DLL-CONTRACT-20261106.md) v3（提交 `7d55333`，binary/compiler/platform/reviewer 会签）。关键值：

| 项 | 值 |
| --- | --- |
| Guest ABI | 2，`GuestState` 37×8=296 字节 |
| callback bridge ABI | 3（COFF GUI 链同步升级） |
| artifact / translator ABI | 2 / 2 |
| trace sink ABI | 1；trace file ABI COFF=3、PE=4 |
| 地址分类 | file offset / RVA / preferred base / Guest load base / Guest VA / Host backing / Host ARM64 函数地址 |
| PE 内存 | 区域表 `AevMemRegion`，R/W/X 检查；Guest VA 与 Host 地址解耦；不用 `MAP_FIXED` |
| artifact 绑定 | module file SHA、load base、image SHA、import/host binding、entry exports、thunk 表、all/selected 表、ABI |
| 跨基址复用 | 不支持；基址错配在进入 Guest 前拒绝 |
| COFF 兼容 | identity-mapped 单区域仅保留给既有 COFF 回归；PE 不使用 |

旧 ABI1/256 产物明确拒绝；`docs/p0/CALLBACK-ABI-20261105.md`、`TRACE-AOT-CONTRACT-20261106.md`、`PROMOTION-PROVENANCE-20261106.md` 已加取代说明。

## 2. 固定 fixture

源码/构建：`spikes/p0-engine/testdata/pe/`（`aevguest.c`、`aevhost.c`、`.def`、`build_msvc.ps1`、`SHA256SUMS`）。Windows 参考机 `ssh win`，VS 18 Community，MSVC 19.51.36260 x64；`/Od /GS- /Zl /Brepro`，链接 `/DLL /NOENTRY /NODEFAULTLIB /DYNAMICBASE /FIXED:NO`。无 CRT、无 DllMain。

| 文件 | SHA-256 | 说明 |
| --- | --- | --- |
| `aevguest.dll` | `1f82b67978dc13e1e31c1262497a9fe186d39f637949b94408c646d4b11152b0` | 4608 字节；PE32+ AMD64 DLL；9 个 named export；1 个 IAT `aevhost.dll!AevUpdate`；1 个 DIR64 relocation（RVA 0x3028，指向 0x300C）；`.pdata` 9 项；`.text`/`.rdata`/`.data`/`.pdata`/`.reloc` |
| `aevhost.dll` | `0eabd013127ef5cc4e73c149aa0988b3d88dbb009d491c0c895e7a0d479757d1` | 无 CRT helper；`AevUpdate(int,int)`、`AevHelperCalls` |
| `aevguest_ref.exe` | `0b30d8ea7ce3bfb8b528857d9b1398e0d4f786ca5cf9a8666262e09f9ca96b7f` | Windows 系统 loader reference；不参与 Mac 转换 |

PE 盘点原始证据：`testdata/pe/aevguest.headers.txt`、`.imports.txt`、`.exports.txt`、`.relocations.txt`、`build.log`。Windows reference 输出 `reference_stdout.txt`。业务包含全局 `seed/table/counter/calls`、可写全局状态、`AevUpdate(a + calls, b)` IAT 调用、`g_state.cursor = &g_state.table[2]` 的绝对指针、分支/循环/32 位回绕；导出 `AevDllReset/Compute/State/CursorValue/CursorAddress/Calls/ReadAt/WriteAt/CallPtr`。

## 3. 实现范围

- `src/pe.{hpp,cpp}`：有界 PE32+ 解析/映射/零填充/DIR64/IAT/导出/.pdata/直接调用发现；上限与拒绝清单见契约。
- `src/pe_runner.cpp`：`aev-pe-runner`；区域表、导入 thunk、HostImport、JIT dispatcher、AOT dylib 校验、trace、reload/stale handle、Guest VAs only 输出。
- `src/translator.*`：`FunctionSpec`/`build_program_from_specs`/external jmp；合成单 section `CoffObject` 复用共享 decoder/emitter/块身份。
- `src/aev_mem.c`：区域模型、跨边界/权限/execute 检查、fault 字段、COFF 兼容谓词。
- `src/ir_emitter.cpp`：`aev_flags_shift`、`CDQE`、`SHL/SHR/SAR`、`pending_transfer`、模块身份符号。
- `src/translate_main.cpp`：PE 模式、manifest、artifact 身份。
- `scripts/aot_artifact.py` / `aot_supplement.py` / `publish_aot.py`：ABI2、module 身份、PE supplement/publisher；不新增第三套补充器。
- `scripts/pe_dll_test.py` / `run_pe_dll_test.sh`：真实 DLL 端到端与负例驱动（新增 ctest `p0_pe_dll`）。
- GUI 消费者：`engine_bridge.py`/`engine_host.py` 升 ABI3/296/2，原 COFF GUI 链保持。

## 4. 地址/内存/重定位证据

首选 base `0x180000000`：

- `image_size=0x6000`；`thunk_base=0x180006000`；`stack_base=0x180007000`；`entry_rsp=0x180106ef8`。
- relocation `delta=0`；`AevDllCursorAddress()=0x18000300c`；与 `.data`/relocation RVA 0x300C 一致。
- 业务：`prep` result `-940722`（JSON `4294026574` 的 32 位有符号解释）、state `-1515459203`、cursor value `1515866136`、calls `1`、helper `1`。

非首选 base `0x190000000`：

- `image_sha256=57278e77…`（relocation 后字节随 base 改变）；`thunk_base=0x190006000`；relocation `delta=0x10000000`；`AevDllCursorAddress()=0x19000300c`。
- 同一 DLL/同一导出/同一输入的业务结果与首选 base 一致；cursor 指针随 base 平移。
- 每 base 生成独立 AOT；`0x190` artifact 在 `0x180` 运行、`0x180` artifact 在 `0x190` 运行均 `status=3 LOAD`，错误 `AOT module identity does not match loaded PE module`。

故障证据（`p0_pe_dll` 110/110）：

| 场景 | 结果 |
| --- | --- |
| 只读 `.rdata` 写 | `MEMORY`，`mem_fault_kind=3`，`error_pc` 精确；随后读值未变 |
| 非执行段取指 | `MEMORY`，`kind=4`，`mem_fault_addr` 为被取指 VA |
| 未映射读 | `MEMORY`，`kind=1` |
| 跨区域读 | `MEMORY`，`kind=5` |
| 未知 import | 装载失败 `unknown import: aevhost.dll!AevUpdate` |
| 截断/错架构/坏 relocation/非可执行 export | 装载前明确失败 |
| 模块 SHA/image/ABI/module kind/基址错配 | 进入 Guest 前明确失败 |
| `--expect-error` 失败后成功 | rc 0，顶层保留首个失败 status 6，`expected_error=true`、`failure_status=6` |
| 无 `--expect-error` 失败后成功 | rc 6，不再被后续成功覆盖 |
| destroy 后旧 handle | `STALE_HANDLE=9` |

## 5. AOT/JIT → Trace → 新进程 AOT

固定 DLL、base `0x180000000`，全部 25 个已发现块：

1. JIT-only/empty AOT 运行 prep，真实 `jit_compile`/`jit_exec`；trace 由实际 dispatcher/JIT 产生。
2. `aot_supplement.py --input-kind pe` 用实际 empty base + prep trace 生成 base artifact：selected 16/25，QA final2 构建的 `libpe_base.dylib` SHA `ec7fe535…`，实际 dylib 表与 manifest 一致。
3. hidden 输入 `AevDllCompute(-9,2,17)` 在 base artifact 下：1 个未覆盖合法块 `0x180001165` 真实 JIT（`jit_compiles=1`，`jit_execs=1`），业务 `861539472`。
4. prep(base 下) + hidden trace 离线补充：selected 17/25，promoted `[0x180001165]`，demoted `[]`；supp artifact（QA final2 构建）SHA `e2537c6f…`。
5. 新进程加载 supp artifact 跑 hidden：`jit_compiles=0`、`jit_execs=0`，trace 中 `0x180001165` 有 `aot` 事件、无 JIT 事件，业务仍 `861539472`。
6. `publish_aot.py` 用实际 base/new dylib + manifest 校验并发布；错误 base SHA/base selected/fake promoted/missing base 均拒绝；发布后的 artifact 新进程 hidden 零 JIT。

Full AOT（不排除块）在首选/非首选 base 下分别 22 个 AOT selected、3 个 IMUL 块 JIT；所有 4 组输入业务与 Windows 参考一致。100 次调用与独立 Python oracle 一致；3 次 reload 状态隔离，旧 handle `STALE_HANDLE`。

## 6. 回归与双机结果

| 项 | 结果 | 证据 |
| --- | --- | --- |
| 全新 configure/build | rc 0 | `qa-behavior/logs/build-exit.txt`、实施 `.local/build-stage1` |
| ctest | **6/6**（原 5 + `p0_pe_dll`） | `qa-behavior/logs/ctest.log`、`.local/results/P0-PE-DLL-01/ctest/ctest-all.log` |
| `p0_pe_dll` | **110/110** | `.local/build-stage1/results-pe-dll/checks.json`、QA final2 |
| Trace/AOT | **78 PASS / 0 FAIL** | `.local/results/P0-PE-DLL-01/trace-aot/trace-aot.log` |
| GUI base/首次补充/空新增再补充 × compat/native | 六组各 **44/44**、4 张截图 | `.local/results/P0-PE-DLL-01/gui-summary.json`；`gui/{base,supp,empty}/{compat,native}/summary.json` |
| Windows 同 DLL reference | 8 次 rc 0；业务字段与 committed reference 一致 | `qa-behavior/windows/`；ASLR 高位变化、低 16 位 0x300C 稳定 |
| 独立 QA artifact/负例 | PASS；`--expect-error`、relocation 回绕、COFF `jmp [IAT]`、64 位 shift 均复测；110/110 | `.local/results/P0-PE-DLL-01/qa-final2/REPORT.md` |
| 独立 QA behavior/Windows/GUI | PASS；274/274 独立行为检查；六组 GUI；AOT shift/回归 | `.local/results/P0-PE-DLL-01/qa-behavior/REPORT.md`、`qa-final2/REPORT.md` |
| 依赖/反替代 | runner/bridge 链接同一 LLVM 23.1.4/Capstone 5.0.9；AOT 仅 libSystem；无 Unicorn/pefile/guest_pe/Rosetta 进入 engine 路径 | QA `otool -L`/`nm`/源码扫描 |

GUI 的自动截图与事件/结果已核；物理键鼠、焦点、IME、人工观感仍待用户，不作为本轮已通过项。

## 7. 关键命令（节选）

```bash
# 独立构建
cmake -S spikes/p0-engine -B .local/build-stage1 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo   # rc 0
cmake --build .local/build-stage1 -j 8                                                         # rc 0
ctest --test-dir .local/build-stage1 --output-on-failure                                        # 6/6

# Windows fixture（ssh win）
powershell -NoProfile -ExecutionPolicy Bypass -File C:/Users/PeterWind/AevotraceP0/P0-PE-DLL-01/src/build_msvc.ps1
# BUILD_OK; aevguest.dll SHA 1f82b6…

# PE 翻译/AOT（首选 base）
.local/build-stage1/aev-translate --pe spikes/p0-engine/testdata/pe/aevguest.dll \
  --load-base 0x180000000 --registry spikes/p0-engine/testdata/pe/aevimports.txt \
  --entry AevDllCompute --out-ll … --out-header … --out-manifest …
clang --target=arm64-apple-macos14.0 -O2 -c … -o … && clang -arch arm64 -dynamiclib -o …

# 运行/负例（参见 scripts/pe_dll_test.py 的全部命令日志）
.local/build-stage1/aev-pe-runner --dll … --registry … --load-base 0x190000000 \
  --aot .local/results/P0-PE-DLL-01/artifacts/all190.dylib --entry-export AevDllCompute \
  --call AevDllReset:0x1234 --call AevDllCompute:-9,2,17 --call AevDllState
```

完整命令/输出/退出码在 `.local/results/P0-PE-DLL-01/` 与 QA 报告目录；`.local` 不作为提交内容。AOT dylib 的 SHA-256 会随输出路径/install name 改变；持久身份以 artifact 的 module/file/load/image/import/host/entry/thunk/ABI/all/selected 字段为准。上列 SHA 来自 QA final2 的 `p0_pe_dll-standalone/artifacts`：

```text
ec7fe5352115aba90659aa01966f718f5b43ec427f7c735258f79eb13014cac5  libpe_base.dylib
e2537c6f8701765cbcd3d7e900ceae94e753b2783ec5170c40f8299f60b605a3  libpe_supp.dylib / libpe_published.dylib
ea5bf98859899c769aac43ea8d7fa0dd8588c75af4ebbee6e5430406f189cc4c  libpe_all190.dylib
ecce6a41ab9618ca9920e78a2ce322f70bfb1d8f7af1cfe1b636eb3917c0a272  libpe_empty.dylib
```

## 8. 最终修复与复核（f864d97）

在首轮 QA 后修复了三项独立发现并复跑：

- **relocation 解析 32 位回绕/越界**：`parse_relocations` 全部改为 64 位位置/剩余量检查，entry 二次边界检查，`total_entries`/site 上限；新增 `neg-bad-reloc-size`（第二个 block `SizeOfBlock=0xFFFFFFF8`）。QA `qa-final2` 用 5 种巨值 × 普通/`--expect-error`/ASAN 共 15 组验证，均在约 0.02s 内 `invalid relocation block`，无越界/挂起/ASAN 报告。
- **COFF bridge `pending_transfer` 门禁/清零**：`aev_bridge.cpp` import 分支在读栈/调用 Host 前要求 `pending_transfer==CALL`，成功后清零；QA 自建 COFF `jmp [IAT]` 负例为 `NO_BLOCK`、Host calls=0、counter=0；正常 `call` 仍通过。
- **64 位 shift mask 回归**：`ir_emitter.cpp`/`aev_mem.c` 使用 64 位 mask 63、8/16/32 位 mask 31；新增 `shift64_cl/shift32_cl/shift8_cl` JIT 语义回归（`p0_core_fix_semantics` **501 PASS / 0 FAIL**）；QA 独立 AOT/JIT 复测 `shlq` 32=`0x100000000`、63=`0x8000000000000000`、32 位 32=`0x1`、8 位 8=`0x0`。

最终独立 QA：`.local/results/P0-PE-DLL-01/qa-final2/REPORT.md`（PASS，0 阻断；ctest 6/6、`p0_pe_dll` 110/110、六组 GUI 44/44）。最终 reviewer：ACCEPT WITH NON-BLOCKING NOTES（无阻断；见 §9）。

## 9. 未测/限制与后续非阻断项

- 未实现/未验证：通用 EXE/进程启动、完整 Win32/消息泵、真实第三方应用、TLS callbacks、DllMain/CRT、SEH/unwind、COM、线程、delay import/API set/forwarder/ordinal import、运行中重装/并发失效、间接目标按需建函数、持久化 JIT 缓存产品化、打包/签名/公证、性能结论。
- Windows 参考只提供业务行为，不提供指令级 Trace；helper 寄存器级抓取未能 attach，参数/返回值通过输出状态与 100 次调用间接验证。
- GUI 自动截图/事件通过；人工键鼠/焦点/IME/观感未验收。
- PE 驱动 GUI 留后续任务；现有双 GUI 链保持。
- 本轮未合并 main、未 push、未删除工作树；停在 `p0/pe-dll-01` 供外部复核。P0 整体仍不通过。

最终 reviewer 的非阻断项（不影响本轮有界验收，建议下一轮处理）：runner 把所有装载前失败统一为 `LOAD=3`，未细分契约中的 4/5；`--entry-export` 在无 AOT 的 `--call`/`--trace` 场景未强制；COFF bridge 未校验全部零 hash/thunk/relocs 字段；AOT required-symbol 列表未显式要求全部新符号；publisher 未重算 `entry_exports` 列表 SHA；ILT 恰好 4096 项时未检测零终止；`--call + --dump-image` 写出的是调用后 image；compat 半初始化 region 字段可能降级；`num_names > num_funcs` 对别名导出过严；Host `args` 声明未执行；B2/shift AOT 只在 QA 工具而非 ctest 中回归。
