# P0-PE-CORE-FIX-01 报告：PE 段权限与 SAR 标志位聚焦修复

日期：2026-11-06。状态：**本轮有界修复 PASS，独立 QA 两项 PASS，reviewer ACCEPT WITH NON-BLOCKING NOTES；P0 整体未完成**。基线 `00ee8d3a20fc68507adcfe5d7fc1e1007a1b2fe2`（上一轮源码 `f864d97`），本轮代码提交 `987692be19fdc1057f8bdd4be3ada87d2047e280`。分支 `p0/pe-core-fix-01`，工作树 `.worktrees/p0-pe-core-fix`。main 仍 `c91a3f7`；未合入、未 push、未删除工作树。

## 1. 外部审查问题与范围

外部审查 `.local/results/CODEX-PE-DLL-REVIEW-20261106/REPORT.md` 接受上一轮正常 PE/DLL/AOT/JIT 主链和既有回归，但指出两项 P2 正确性问题：

- **F1**：`src/pe.cpp` 用 `!MEM_WRITE` 推导 READ，导致 R/W/X 全空的 `.data` 仍被映射为可读；Windows 系统 loader 对同一修改 DLL 的 `.data` 为 `PAGE_NOACCESS`。修复前 JIT/AOT `AevDllReadAt(0x180003000)` 返回 4660。
- **F2**：`src/aev_mem.c` 对 SAR masked count 超过操作数位宽时把 CF 置 0；8 位 `0x80, count=9`、16 位 `0x8000, count=17` 的 Windows x64 原生 CF=1，AOT/JIT CF=0。

本轮只修这两项、补红/绿回归、双机对照和独立 QA/reviewer，不扩 PE 驱动 GUI、Win32 API、真实应用、打包或性能。

## 2. 契约与 ABI

- 契约：`docs/p0/PE-DLL-CONTRACT-20261106.md` v3 + §12 `P0-PE-CORE-FIX-01` 修订（提交 `48268b7`）。
- Guest ABI 2 / callback ABI 3 / artifact ABI 2 / translator ABI 2 **不变**；未改 `aev_guest.h`、bridge/artifact 公共字段或公共构建入口。
- 新增权限规则：`readable = MEM_READ || MEM_WRITE`、`writable = MEM_WRITE`、`executable = MEM_EXECUTE`；R/W/X 全空 -> prot 0（PAGE_NOACCESS 语义），W-only -> R+W，X-only -> X，R+X -> R+X，W+X/RWX 继续装载前拒绝。
- SAR：8/16/32 位 count mask 31、64 位 mask 63；masked count=0 flags 保持；masked count >= 操作数位宽时 SAR CF = 符号位，< 位宽时 CF = 最后移出的位；只断言有定义的结果/CF/ZF/SF/PF 和 count=1 的 OF。

## 3. 红/绿证据

### F1

同一外部修改 DLL：`data-no-access.dll` SHA-256 `b0bd2c7e58c2af2708354fe8340529932a90a841514e26a19554a66e4051252e`（仅 `.data` characteristics `0xC0000040 -> 0x40`）。

| 路径 | 修复前 | 修复后 |
| --- | --- | --- |
| `.data` section/region | prot 1（R） | **prot 0（no access）** |
| JIT `AevDllReadAt(0x180003000)` | status OK，rax 4660 | **status 6 MEMORY，kind 2** |
| JIT `AevDllWriteAt(0x180003000,1)` | 未测试 | **status 6，kind 3** |
| JIT `AevDllCallPtr(0x180003000)` | 未测试 | **status 6，kind 4** |
| AOT `AevDllReadAt` | status OK，rax 4660，`aot_blocks=1` | **status 6，kind 2，aot_blocks=1，jit=0** |
| 故障后 `.data` image 字节 | — | **不变** |
| 原始/修改 DLL SHA | — | **不变** |

首故障位置：读/写 `error_pc=0x180001316` / `0x18000135e`（均在 `.text`，JIT/AOT 一致）；取指 `error_pc=0x180003000`。Windows 系统 loader 对同一 DLL 的 `.data` `VirtualQuery Protect=0x1 PAGE_NOACCESS`（ssh exit 0）。

权限矩阵（独立 QA + 提交测试）：

| characteristics | Windows 实测 | 本院 region/prot | 行为 |
| --- | --- | --- | --- |
| R/W/X 全空 | 0x1 PAGE_NOACCESS | 0 | 读 kind2 / 写 kind3 / 取指 kind4 |
| R only | 0x2 PAGE_READONLY | 1 | 读成功，写 kind3 |
| W only | 0x8 PAGE_WRITECOPY（有效 R/W） | 3 | 读/写成功 |
| R+W | 0x8 PAGE_WRITECOPY（有效 R/W） | 3 | 读/写成功 |
| X only | 0x10 PAGE_EXECUTE | 4 | 读/写失败 |
| R+X | 0x20 PAGE_EXECUTE_READ | 5 | 读成功，写 kind3 |
| W+X / RWX | 0x80 PAGE_EXECUTE_WRITECOPY | 装载前拒绝 | `W+X section is not supported` |

实现只按 Characteristics 位推导，没有按 section 名、输入哈希或反例路径硬编码。

### F2

外部真实 COFF 输入：`sar8.obj` SHA-256 `241750cb00cf9121c17c5fb444226a4dff0a5b407482373b240f97845c578bd2`；`sar16.obj` `f5859755904512d89aec83af2cffe28ee4b47fec61fae1c04574a637d96bda6a`。

| 输入 | 修复前 AOT/JIT | 修复后 AOT/JIT | Windows x64 |
| --- | --- | --- | --- |
| SAR8 `0x80` by 9 | AL=0xFF，CF=0 | **AL=0xFF，CF=1** | AL=0xFF，CF=1（RFLAGS=0x287） |
| SAR16 `0x8000` by 17 | AX=0xFFFF，CF=0 | **AX=0xFFFF，CF=1** | AX=0xFFFF，CF=1（RFLAGS=0x287） |

提交回归覆盖 8/16 位正负、count 0/1/7/8/9/31/32/33（32/33 验证原始 count mask 0/1），AOT/JIT 各 32 组；另用 `sar8_cf.s`/`sar16_cf.s` 的真实 `SAR` + `JC` 分支消费 CF，JIT 路径使用空 AOT 表 + 真实 `aev_aot_run`/ORC fallback 执行分支目标，负数为 1、正数为 0。count=0 用预置 `RFLAGS=0x1` 验证 flags 保持。独立 QA 扩展 8/16/32/64 位、count 0/1/位宽±1/31/32/33/63/64/65 共 312 次执行、52 个 Windows 对照全部一致。

## 4. 测试与回归

| 项 | 结果 | 证据 |
| --- | --- | --- |
| 全新 configure/build | rc 0 | QA `build-qa-corefix-perm`/`-sar` |
| ctest | **6/6** | QA `logs/ctest.log` |
| `p0_pe_dll` | **147/147** | QA perm |
| `p0_core_fix_semantics` | 代码提交 987692b 上 **715/0**；测试加强后本地 **791/0** | QA `qa-sar/semantics/summary.txt`；本轮本地 rerun |
| Trace/AOT | **78/0** | ctest verbose / `logs/trace-aot.log` |
| GUI base / 首次补充 / 空新增再补充 × compat/native | 六组各 **44/44**、4 截图 | QA perm 重跑；`gui/*/summary.json` |
| 旧 PE AOT artifact | 旧 `libpe_base.dylib` SHA `19d8645d…` 在当前 runner 下成功加载并运行 prep | `old-pe-artifact.json`（`.local/results/P0-PE-CORE-FIX-01/`） |
| 原始 DLL / 修改 DLL | SHA 未变 | QA perm |

独立 QA 结论：

- `.local/results/P0-PE-CORE-FIX-01/qa-perm/REPORT.md`：**PASS**，343/343 权限矩阵；Windows PAGE_NOACCESS；JIT/AOT fault、无副作用、原 DLL 不变；六组 GUI。
- `.local/results/P0-PE-CORE-FIX-01/qa-sar/REPORT.md`：**PASS**，312 次矩阵 / 76 项脚本 / 52 个 Windows 对照；AOT/JIT 分支消费 CF；红证据用父提交 helper 对照。
- Reviewer：**ACCEPT WITH NON-BLOCKING NOTES**，无阻断；指出文档 Windows 常量命名、测试内 AOT 分支/`error=0`/初始 flags 断言偏弱、旧 PE AOT 复跑证据缺口等，均已在本报告/测试加强中处理或列为非阻断。

## 5. 关键命令（节选）

```bash
# 独立构建与回归
cmake -S spikes/p0-engine -B .local/build-baseline -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build .local/build-baseline -j 8
ctest --test-dir .local/build-baseline --output-on-failure -j 1   # 6/6

# F1 复现/修复后（外部修改 DLL）
.local/build-baseline/aev-pe-runner --dll .local/corefix/data-no-access.dll \
  --registry spikes/p0-engine/testdata/pe/aevimports.txt --load-base 0x180000000 \
  --entry-export AevDllCompute --call AevDllReadAt:0x180003000 --expect-error
# 修复前 status OK/4660；修复后 status 6、kind 2、prot 0；AOT 同样

# F2 复现/修复后
.local/build-baseline/aev-jit-probe .local/corefix/sar8.obj sar_probe 0x140001000 \
  rax=0x80 rcx=9
.local/build-baseline/aev-aot-probe .local/corefix/libsar8.fixed.dylib 0x140001000 \
  rax=0x80 rcx=9

# Windows 对照
ssh win 'python -' < .local/results/CODEX-PE-DLL-REVIEW-20261106/windows-sar-reference.py
# {"bits":8,"count":9,"rflags":"0x287","CF":1}
```

完整命令/退出码/日志在 `.local/results/P0-PE-CORE-FIX-01/`（before/after/windows/semantics/pe/gui/logs、qa-perm、qa-sar）。

## 6. 未测/限制

- 未扩 PE 驱动 GUI、Win32 API、真实应用、打包/签名/公证、性能、冷/持久化 JIT 缓存、运行中失效。
- X-only 数据段的实际取指受“JIT 块必须属于已发现 Guest 函数”的既有边界限制；权限闸门本身通过，未把该路径扩展为任意数据执行。
- 未在 Windows 原生进程故意执行非法访存制造崩溃；使用 VirtualQuery 和独立 RW→RX SAR 进程内参考。
- Windows 参考不提供指令级 Trace；人工 GUI 外观/物理键鼠/IME 待用户验收。
- 旧 PE AOT 兼容性只抽查上一轮 base artifact（ABI 未变）；未穷举所有历史产物。
- 本轮只读审查会话无法独立证明 Max 档位；`session_v2` 显示主控/QA 会话为 `opencode-go / deepseek-v4.1-flash / max`。

## 7. 停止状态

代码提交 `987692b`；测试/文档加强后集成分支 HEAD 见 `git rev-parse p0/pe-core-fix-integration`。未合入 main（仍 `c91a3f7`）、未 push、未清理工作树、未开启下一轮。
