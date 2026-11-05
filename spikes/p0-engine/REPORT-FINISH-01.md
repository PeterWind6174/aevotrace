# P0-FINISH-01 completion report

Status: **completed on the task branch; not pushed and not merged**.
This report covers P0-ENGINE-AOT-EMPTY-01 and P0-CORE-FIX-02 on the isolated
branch.  It is functional evidence for the engine spike only; it is not a
frozen ABI, a real-application support claim, or a performance claim.

- Worktree: `/Users/peterwind/Script/Systems/Aevotrace/.worktrees/p0-core-fix-02`
- Branch: `p0/core-fix-02`
- Baseline: `8c0fc15087221e408e84f4b7adf50fc67f2a9abf`
  (`p0/engine-spike`, the P0-CORE-FIX-01 review HEAD)
- Stage A commit: `9ad06322563b2c5eb953953c65d8b9601901993f`
- Stage B code commit / final code HEAD:
  `7487e04ed2ff8cc393dba14d3a17ec931c9175af`
- The report commit is a later documentation-only commit; `git log -1` is
  authoritative for the branch head.
- No main-worktree source, old worktree, old QA/review directory, or old
  evidence directory was modified or deleted.  No push, merge, or history
  rewrite.

Model/tier: the coordinator verified the OpenCode session database entry for
this session as `opencode-go/deepseek-v4.1-flash`, variant `max`, directory
`.worktrees/p0-core-fix-02` (see
`.local/results/P0-FINISH-01/MODEL-VERIFICATION.md`).  No `opencode run` or CLI
worker was started, and no subagent was spawned by this session.

## 1. Environment

| Item | Value |
| --- | --- |
| Host | macOS 27.0.1, arm64 |
| CMake | 4.4.4 |
| Apple clang | 21.0.0 |
| LLVM | 23.1.4 (`/opt/homebrew/opt/llvm`) |
| Capstone | 5.0.9 (`/opt/homebrew/opt/capstone`) |
| Build dir | `.local/build-core-fix-02` (wiped and rebuilt at final HEAD) |
| Results | `/Users/peterwind/Script/Systems/Aevotrace/.local/results/P0-FINISH-01/` |

## 2. Stage A — P0-ENGINE-AOT-EMPTY-01

### 2.1 Reproduction on the unmodified baseline

Input: only `imull $7, %ecx, %eax; ret`, assembled to COFF with
`clang --target=x86_64-pc-windows-msvc`.  Full transcript:
`repro-empty-aot/REPRO.md`.

| Observation | Result |
| --- | --- |
| `aev-translate` | rc=0, reports `aot_blocks=0 jit_blocks=1` |
| generated IR line | `@aev_blocks = constant [0 x %BlockEntry] [zeroinitializer]` |
| `clang --target=arm64-apple-macos14.0 -O2 -c` | rc=1, `error: expected type` at that line |
| produced `.ll/.h/manifest` | all written despite the invalid artifact |

Hashes: input object `5088c4ea…`; invalid `.ll` `750c4b22…`.
This is exactly the QA/review observation on the baseline.

### 2.2 Fix

`src/ir_emitter.cpp`: a zero-length array is now emitted as the legal aggregate
spelling

```llvm
@aev_blocks = constant [0 x %BlockEntry] zeroinitializer
```

The non-empty branch is byte-for-byte unchanged.  The non-empty fixture
`generated/guest_aot.ll` keeps its previous hash `df9aa1d6…` and
`guest_aot.o` keeps `5b158e59…`, so this did not alter existing AOT tables.

no IMUL AOT support was added; `block_is_aot_covered` and the JIT hand-off are
unchanged.

### 2.3 Execution verification (not a text-only test)

`src/aot_jit_probe.cpp` (test-only) loads the translated dylib, resolves
`aev_aot_run` and `aev_block_count`, and supplies a real LLVM ORC JIT callback
using the engine's own `emit_jit_block`.  The automated test:

1. translates `empty_aot_imul.s` with rc=0 and checks the legal empty table;
2. compiles the `.ll` to an ARM64 Mach-O object with clang;
3. links `empty_aot_imul.o` + `generated/aot_runtime.o` +
   `generated/aev_mem_aot.o` into `empty_aot_imul.dylib`;
4. loads the dylib in a fresh process and calls `aev_aot_run`;
5. the empty table misses, so the dispatcher calls `jit_dispatch`, which
   compiles and executes the IMUL block with ORC.

Observed state:

```text
rax=0x15 rcx=0x3 rsp=<rsp_in+8> rip_out=0xffffffffffffff00
rc=0 error=0 steps=1 aot_table_count=0 aot_blocks=0
jit_compiles=1 jit_execs=1
JIT compile ... triple=arm64-apple-darwin...
```

`RAX=0x15` (21) is the correct `10 * 7` result, proving the JIT block actually
executed rather than merely compiling.  `aot_table_count=0` and
`aot_blocks=0` prove the AOT table was empty and no AOT block ran.  Exit code
and state checks are in `final/empty-aot/summary.txt`.

Result: **PASS**.  The alternative (non-zero rejection with a diagnostic) was
not needed because the legal empty table loads and dispatches correctly.

### 2.4 Automated coverage

New ctest: `p0_engine_empty_aot`, driven by
`scripts/run_empty_aot_test.sh`.  It performs 22 assertions for one independent
scenario (translate → compile → link → load → dispatch → execute).  The original
two ctest tests retain their behavior.

## 3. Stage B — P0-CORE-FIX-02

### F1 — LOCK rejection artifacts and terminator guard

Reproduction/observation: the previous `translate_fail` wrote to fixed paths in
the shared results directory and did not pass `--out-manifest`, so it could not
prove that a failed translation left no artifact.  The emitter checked
`lock_prefix` only in `emit_instruction`; a caller-constructed Program with a
LOCK terminator never went through that function.

Changes:

- `scripts/run_semantics_tests.sh`: each rejected translation now uses a fresh
  `mktemp -d` directory under this run's results, passes all three output
  arguments, and asserts that none of `.ll`, `.h`, or `.json` exists.
  Old result directories are not deleted.  All 13 existing rejection cases now
  run with this stronger check, and 4 new rejection cases use it.
- `src/ir_emitter.cpp`: `emit_terminator` rejects `lock_prefix` at entry.
  Normal `build_program` still rejects every LOCK form before IR generation.
- `src/emitter_lock_probe.cpp` (test-only) constructs a `Program` by hand with
  a LOCK RET and a LOCK JMP, plus a plain RET control.  Both LOCK terminators
  are rejected with the LOCK diagnostic and address; the control emits.

Result: **PASS**.  17 rejection scenarios left zero `.ll/.h/manifest` files;
the terminator probe logged 3/3 PASS.

### F2 — one-operand IMUL diagnostic

Observed on the baseline: one-operand `imul` at 16/32/64-bit was reported as
`8-bit IMUL form`; only the 8-bit text was correct.

Minimal fix in `src/translator.cpp`: the diagnostic uses
`operands[0].size * 8`, keeps the searchable `one-operand imul` prefix, and
prints the actual `8/16/32/64-bit IMUL form`.  New inputs `imul1_16.s`,
`imul1_32.s`, `imul1_64.s` (alongside the existing `imul1_8.s`) verify all four
widths.  The JIT setup path shares `build_program`; `imul1_8` and `imul1_32` are
also checked through the JIT probe.

Result: **PASS**.  No execution semantics changed; one-operand IMUL remains
explicitly rejected at every width.

### F3 — faulting arithmetic flags and the complete AOT→JIT fault path

New inputs `fault_{add,inc,neg,and,or,xor}_flags.s` each execute
`mov eax,0x11` then a faulting memory-destination operation as the second
instruction, followed by `mov ebx,456; ret`.  RDX points to `mem_end-2`, so the
four-byte access is out of range and cannot partially write.

RFLAGS is preset to `0x8d5` (CF, PF, AF, ZF, SF, OF).  The tests assert:

- `error=1` and exact `error_pc=0x140001005` and `rip_out` at the faulting
  instruction (not the block entry);
- `rflags=0x8d5` unchanged (a committed helper would produce `0x4` for
  add/inc/logic or `0x44` for neg);
- `RAX=0x11` (the prior instruction committed), `RBX` unchanged (the
  post-fault instruction did not), `RDX` unchanged, RSP unchanged;
- the two in-range bytes at `mem_end-2` remain `00:00`.

All six run through the ORC JIT path; `fault_add_flags` also runs through a
separately loaded AOT dylib.

Complete hand-off case `fault_imul_add.s`:

```text
imull $7, %ecx, %eax      ; JIT-only, block start
addl  $1, (%rdx)          ; faults at 0x140001003
movl  $456, %ebx          ; must not run
ret
```

The only block contains IMUL, so the AOT table is empty.  `aev-aot-jit-probe`
drives `aev_aot_run` → `jit_dispatch` → ORC.  Observed:
`rc=1`, `error=1`, `error_pc=0x140001003`, `RAX=0x15`, `rflags=0x10`
(IMUL committed and cleared CF/OF/ZF/SF/PF; AF remains from the preset; the
faulting add did not commit), `RBX=0xbbbb…` (post-fault mov absent),
RSP unchanged, memory `00:00`, `aot_table_count=0`, `aot_blocks=0`,
`jit_compiles=1`, `jit_execs=1`.  This proves the faulting block returns
non-zero through the real dispatcher and that the earlier IMUL is committed
while the faulting instruction and its successors are not.

Note: an initial draft expected the fault at `0x140001008`; the assembled
object places the faulting add at `0x140001003` (the file has no leading
`mov`).  The committed expectation is the actual instruction address, verified
with `llvm-objdump`; this was a test-arithmetic correction, not an engine
expectation change.

Result: **PASS**.  8 new independent scenarios, 94 assertions.

### F4 — push immediate success and `pushw` rejection

Reproduction: `push imm8` and `push imm32` already behaved correctly before
the Stage B changes; no product-code change was required.

New inputs and checks:

- `push_imm8.s` (`pushq $-1; popq %rax; movq %rsp,%rbx; ret`):
  `RAX=0xffffffffffffffff`, `RBX=rsp_in` (POP restored RSP),
  final `RSP=rsp_in+8` after RET.
- `push_imm32.s` (`pushq $-2147483648`):
  `RAX=0xffffffff80000000`, same RSP checks.
- Both run through the ORC JIT and through a separately loaded AOT dylib.
- `pushw_imm.s` (`pushw $1; ret`) is rejected in translation with
  `unsupported 16-bit push/pop` and in the JIT setup path.

This also covers the previously untested `pushw $imm` immediate form (the old
16-bit tests used `pushw %ax`).  Result: **PASS**; no defect found.

### F5 — pre-set CF/OF and IMUL overflow clearing

`jit_flags` now initialises `rflags=0x801` (CF|OF) before each IMUL.  All 19
JIT cases still expect the same CF/OF values, so a no-overflow case proves the
helper clears both bits (`0/0`) and an overflow case keeps them (`1/1`).  Only
CF/OF are judged; ZF/SF/PF remain architecturally undefined for IMUL and are
never used as a correctness oracle.  The host and AOT-dylib helper matrices are
unchanged and still pass (27 cases each).

Result: **PASS**.

## 4. Test results

All commands were run on the final code HEAD `7487e04…` after wiping and
rebuilding `.local/build-core-fix-02`.

| Command | Exit | Result |
| --- | ---: | --- |
| CMake configure | 0 | PASS |
| CMake/Ninja build | 0 | PASS |
| `ctest --output-on-failure` | 0 | 3/3 tests PASS |
| `scripts/run_tests.sh` (original 5-case closed loop) | 0 | 22 PASS, 0 FAIL |
| `scripts/run_semantics_tests.sh` | 0 | 489 PASS, 0 FAIL |
| `scripts/run_empty_aot_test.sh` | 0 | 22 PASS, 0 FAIL |

### Assertions versus independent scenarios

Assertion counts and scenario counts are deliberately kept separate:

- 533 assertions passed in total (22 closed-loop + 489 semantics + 22
  empty-AOT); 0 failed.
- The P0-CORE-FIX-02 additions contribute 125 new semantics assertions and
  19 new independent semantic scenarios:
  - F1: 1 emitter-terminator scenario (3 internal cases);
  - F2: 4 new one-operand diagnostic scenarios (16/32/64-bit translation and
    32-bit JIT);
  - F3: 8 new fault scenarios (6 arithmetic JIT, 1 arithmetic AOT,
    1 AOT→JIT);
  - F4: 6 new scenarios (2 JIT push-immediate, 2 AOT push-immediate,
    2 `pushw $imm` rejection paths).
- Stage A adds 1 independent end-to-end scenario and 22 assertions.
- The 489 semantics assertions are **not** 489 independent workloads.  For
  example, 19 F5 JIT cases produce 76 assertions, and 8 new fault scenarios
  produce 94 assertions.
- Existing coverage was strengthened, not counted as new scenarios:
  13 pre-existing translation-rejection cases now also assert artifact
  absence, and 19 pre-existing F5 JIT cases now pre-set CF/OF.

Original behavior is preserved: the 5 closed-loop cases still match the
Windows MSVC reference, the held-out case still differs from `prep_a`, and the
original 364 semantics assertions are all still present (489 now, none removed
or relaxed).  `guest_x64.obj` and `reference_stdout.txt` hashes are unchanged
from the earlier task.

## 5. PASS / FAIL / N/A

| Item | Status |
| --- | --- |
| Stage A baseline reproduction | PASS (defect reproduced) |
| Stage A legal empty table + compile/link/load/run | PASS |
| Stage A ctest coverage | PASS |
| F1 clean rejection output + terminator guard | PASS |
| F2 width-accurate one-operand IMUL diagnostic | PASS |
| F3 faulting arithmetic flags/no side effects | PASS |
| F3 complete AOT→JIT fault return | PASS |
| F4 push imm8/imm32 + `pushw $imm` rejection | PASS |
| F5 pre-set CF/OF clearing | PASS |
| Original closed loop and 364 semantics assertions | PASS |
| AOT IMUL support | N/A (intentionally not added) |
| Atomics/TSO | N/A (LOCK still rejected) |
| PE loader / GUI / real application / Trace | N/A (out of scope) |
| Version invalidation / callbacks / guest exceptions | N/A (out of scope) |

No FAIL remains in the final evidence.

## 6. Hashes

Full list: `final/MANIFEST.sha256` and `final/input-sources.sha256`.

| Item | SHA-256 |
| --- | --- |
| Baseline commit | `8c0fc15087221e408e84f4b7adf50fc67f2a9abf` |
| Stage A commit | `9ad06322563b2c5eb953953c65d8b9601901993f` |
| Stage B / final code HEAD | `7487e04ed2ff8cc393dba14d3a17ec931c9175af` |
| `testdata/guest_x64.obj` | `a12fa7fff6087e2e0ae95a09e395cd41874f5871674a4feda0cbff8ca9da7b1e` |
| `testdata/reference_stdout.txt` | `7d623c9acca20c8a546648a50067161e1e5ee7c0fd08c69ad994bfa3fff81a9c` |
| non-empty `generated/guest_aot.ll` | `df9aa1d6902aaed26995e7e5cfc08022f817ee95c8301065c1438b3804b21d1f` |
| non-empty `generated/guest_aot.o` | `5b158e59aae4ef0cd869c8a161fd48345a2723db022c21bdf60c36e7fb2beb8d` |
| empty-AOT input `empty_aot_imul.obj` | `5088c4ea2c644ba033f1a31f8be0c79952f2254f07069dce58b8343537a19a76` |
| fixed empty-AOT `.ll` | `a2a4e8d763a74986654c79de1a2e0feb4aca0078deec2826676c68320b875d7c` |
| fixed empty-AOT `.o` | `304261c34fef3256ebd12395858a9da88b7b3b7324c64b7263ccb747afee6273` |
| fixed empty-AOT `.dylib` | `5dce616cfb4d13af135a2effb5e7b0c98875d476b3ddd24f8d5fa1a69768a9c2` |
| `aev-translate` | `d55373752b98d2b876c11393577af97a10731c76d1fdd81f2b7da779556168df` |
| `aev-aot-jit-probe` | `d0f1dcc2fdc45b37276c0f3520580596560bcc4f76e8a2bd185132ae76db57af` |
| `aev-emitter-probe` | `0cf8d3dba6852651153af0e33f1a217ab9849c8ef9def6493c34cfa37cbf1b3e` |
| `aev-jit-probe` | `6773e6c6ff4e47f813366a068f01af735def94bfbcdaba00b929ee90f24357a3` |
| `aev-aot-probe` | `95cf5932a46d234e9b6a0d36eaba4a13a1f261f9d5a88aba7543d54e9c470758` |
| `aev-flags-test` | `4d60d2f8077b465710d3df0dfcaa3341ec8717c7eed002eb3fa31752354dc2b9` |
| `aev-runner` | `dcc8ca6490e715bca9510d512b1eb04e7066e3483089ba3e5e7dd0ba74257af1` |
| `src/ir_emitter.cpp` | `ac2ac696fe0b1248f26484bdf987ae2abed9d200955b3c5bb0ba8564b3631130` |
| `src/translator.cpp` | `abd8939f15b656f50af5a56f5ccc166bf465b137cbd2d8a04e84fc98c48897a4` |
| `scripts/run_semantics_tests.sh` | `73b74b8d7a8819ac7487c8c91f74741d63e0de8e26049c2ab4f58a2df8d7248a` |
| `scripts/run_empty_aot_test.sh` | `8bc81260795baeed89dab3bedd9aef601a42ecdcf07e98a828d2c7c22ddde54a` |

The non-empty fixture `libguest_aot.dylib` hash is build-local
(`cc519ab8…` here) and can vary with the linker UUID; its inputs `.ll` and
`.o` match the earlier task hashes, which is the meaningful comparison.
The empty-AOT dylib hash above is likewise a build artifact hash, not a stable
format contract.

## 7. Evidence paths

All under `/Users/peterwind/Script/Systems/Aevotrace/.local/results/P0-FINISH-01/`:

- `repro-empty-aot/REPRO.md` — baseline reproduction, commands, hashes,
  invalid IR and clang error, pre-fix feasibility check.
- `final/commands.txt` — final configure/build/ctest/script commands and exit
  codes.
- `final/logs/configure.log`, `final/logs/build.log`, `final/logs/ctest.log`.
- `final/logs/closed-loop.out`, `final/logs/semantics.out`,
  `final/logs/empty-aot.out` — raw PASS/FAIL transcripts.
- `final/closed-loop/` — `summary.txt`, per-case logs, reference diff (empty).
- `final/semantics/` — `summary.txt`, 17 empty `translate-fail-*` directories,
  per-case JIT/AOT logs, `input-sources.sha256`, `input-objects.sha256`,
  `aot-artifacts.sha256`, `environment.txt`.
- `final/empty-aot/` — generated `.ll/.o/.dylib`, probe stdout/stderr,
  `summary.txt`, `aot-artifacts.sha256`, `input.sha256`.
- `final/MANIFEST.sha256`, `final/input-sources.sha256`.

## 8. Untested / limits

- No PE loader, imports, relocations, TLS, resources, unwind, signals, guest
  exception delivery, threads, self-modifying code, or code invalidation.
  F3 faults are reported through `error_code`/`error_pc`; they are not a
  Windows exception or unwind path.
- No host↔guest callback ABI was added or tested beyond the AOT↔JIT dispatcher
  under test.  No DLL callback boundary was exercised.
- No `MAP_JIT`, hardened-runtime signing, packaging, or persistent JIT cache.
- No atomics/TSO; every LOCK form remains rejected.
- No AOT IMUL support.  The empty-table test deliberately exercises the JIT
  hand-off; the F5 AOT check still calls the exported helper directly rather
  than executing a generated AOT IMUL block.
- No Windows reference run for the new counterexamples.  Their expected values
  are ISA-defined and were verified through the engine's executed state; they
  are not MSVC-compiled fixture code.
- No performance measurement or claim.  The added fault checks are correctness
  mechanisms, not an optimized lowering.
- No GUI, real application, Trace, or version-invalidation work.

## 9. Integration status

The two code commits are on `p0/core-fix-02`, not pushed and not merged.  QA
and reviewer should independently rebuild the exact commit, run the three
ctest tests, and inspect the raw evidence above.  After the report commit,
`git log -1` will differ from the final code HEAD `7487e04…`; use
`git show 7487e04:spikes/p0-engine/src/ir_emitter.cpp` (or check out that
commit) when binding code evidence.
