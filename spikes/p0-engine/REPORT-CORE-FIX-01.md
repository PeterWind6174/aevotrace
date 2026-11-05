# P0-CORE-FIX-01 completion report

Status: **completed on the task branch; not merged or pushed**.

- Worktree: `/Users/peterwind/Script/Systems/Aevotrace/.worktrees/p0-core-fix`
- Branch: `p0/core-fix-01`
- Baseline: `a4ef11c723c929a61030ef3e0ae8fc8fba537ab2`
- Code commit: `9f21649b2a0470d2d6dc4bea576785af22ac6e8c`
  (the report itself is a later commit on the same branch; `git log -1` is
  authoritative for the branch head)
- No main-worktree source was modified.  No push, merge, or main commit.
- Scope: only `spikes/p0-engine/{src,include,testdata,scripts,CMakeLists.txt,
  README.md}`.  GUI, PE loader, Unicorn bridge, real applications and product
  performance were not touched.

This report is functional evidence only.  It does not freeze the spike ABI and
does not claim real-application or Windows-GUI support.

## 1. Environment

| Item | Value |
| --- | --- |
| Host | macOS 27.0.1, arm64 |
| CMake | 4.4.4 |
| Apple clang | 21.0.0 (clang-2100.3.34.2) |
| LLVM | 23.1.4 (`/opt/homebrew/opt/llvm`) |
| Capstone | 5.0.9 (`/opt/homebrew/opt/capstone`) |
| Build dir | `.local/build-core-fix` |
| Input fixture | `testdata/guest_x64.obj`, SHA-256 `a12fa7fff6087e2e0ae95a09e395cd41874f5871674a4feda0cbff8ca9da7b1e` |

Detailed environment output: `.local/results/P0-CORE-FIX-01/fix/semantics-committed/environment.txt`.
Raw command/exit-code record: `.local/results/P0-CORE-FIX-01/fix/commands.txt`.

## 2. Independent reproduction on unmodified `a4ef11c`

The worktree was clean at `a4ef11c` when these observations were taken.  A fresh
configure/build/ctest passed (1/1).  The reproduction harness is in
`.local/results/P0-CORE-FIX-01/repro-compiler/`: `repro_orc.cpp` uses the
engine's `build_program` + `emit_jit_block`, compiles with LLVM ORC LLJIT, and
executes the returned ARM64 code; `repro_aot.c` loads a dylib produced by
`aev-translate` and calls `aev_aot_run`.

Observed on the unmodified tree (full transcript:
`repro-compiler/repro-summary.txt`, per-case logs in
`repro-compiler/logs/repro/`):

| ID | Input / state | Observed | Required |
| --- | --- | --- | --- |
| F1 | `lock addl $1,(%rcx)` and the other committed LOCK forms | translation rc=0; JIT executed an ordinary load/store and changed memory (e.g. `0x2a`→`0x2b`) | explicit rejection |
| F2 | `imul eax,ecx,7`, EAX=10, ECX=3 | RAX=`0x46` (70) | RAX=`0x15` (21) |
| F2 | `imul eax,ecx,-3`, EAX=10, ECX=3 | RAX=`0xffffffe2` (-30) | RAX=`0xfffffff7` (-9) |
| F2 | two-operand `imul ecx,eax` | `0x1e` (30), correct | `0x1e` |
| F2 | 16-bit three-operand | `0x46` (old AX used) | `0x15` |
| F2 | 8-bit `imul cl` | rejected as `imul form` at IR emission | explicit rejection |
| F3 | `mov ebx,0x11223344; mov eax,[rcx]; mov [rdx],123` (RCX=0, RDX valid, mem=0) | RAX=0, RBX=0x11223344, mem=123, RSP advanced, `error_pc=0x140001000` | RAX unchanged, mem=0, RSP unchanged, `error_pc=0x140001005` |
| F3 | faulting store `mov [rdx],123` (bad RDX) | subsequent `mov ebx,456` and `ret` executed; `error_pc` = block start | stop at store; RBX/RSP unchanged; exact PC |
| F3 | faulting push/pop/ret/call | stack/RIP updated after the fault; `error_pc` = block start | no RSP/RIP commit; exact PC |
| F4 | `pop rsp; mov rax,rsp; ret` | RAX=old RSP+8; RSP=old RSP+0x10 after `ret` | RAX=popped value; RSP=popped value+8 |
| F4 | `pushw %ax; popw %ax; ret` | accepted and executed with 8-byte stack operations | explicit rejection |
| F5 | `aev_flags_imul(8,-1,1,-1)` | CF=1, OF=1 | CF=0, OF=0 |
| F5 | 64-bit `min*1` | CF=1 | CF=0 |
| F5 | 64-bit `min*-1`, `max*2`, `2^62*2` | CF=0 (overflow missed) | CF=1 |
| F5 | 32/16/8-bit helper cases | correct | correct |

Baseline build/ctest and artifact hashes:
`repro-compiler/hashes-baseline.txt`, `repro-compiler/logs/configure.log`,
`repro-compiler/logs/build.log`, `repro-compiler/logs/ctest.log`.

## 3. Fixes

### F1 — LOCK prefix

- `src/x64.hpp`/`src/x64.cpp`: decode keeps
  `lock_prefix = (cs_x86.prefix[0] == X86_PREFIX_LOCK)`.
- `src/translator.cpp`: every LOCK-prefixed instruction is rejected before IR
  emission.  The diagnostic contains the address and mnemonic, e.g.
  `unsupported instruction (LOCK prefix; atomic semantics are not implemented)
  0x140001000: lock add dword ptr [rcx], 1 (4 bytes)`.
- `src/ir_emitter.cpp`: a second guard rejects a LOCK-prefixed instruction if a
  caller constructs a program without going through `build_program`.
- Covered forms: `LOCK ADD` (8/16/32/64-bit), `INC`, `AND`, `OR`, `XOR`, `SUB`,
  `DEC`; the existing `lock xadd` negative control now also fails with the LOCK
  diagnostic.  No atomic lowering is claimed.

### F2 — IMUL operand selection

- `src/ir_emitter.cpp`: two-operand uses `dst` (old value) × `src`;
  three-operand uses `operand[1]` × `operand[2]` and never reads the old
  destination as the multiplicand.
- Tested at 16/32/64-bit with dst≠src, positive and negative immediates.
- The one-operand 8-bit form is outside the subset.  It is rejected explicitly
  (`unsupported one-operand imul (8-bit IMUL form) ...`), not silently
  mis-lowered.  The CF/OF helper is still covered at width 1 by the direct
  flag matrix.

### F3 — memory-fault side effects and exact PC

- `src/ir_emitter.cpp`: every `aev_load`/`aev_store` call site is emitted
  through `load_mem`/`store_mem`, which:
  1. store the faulting instruction's address into `GuestState.rip`;
  2. call the helper;
  3. load `GuestState.error_code` and branch to a per-block `fault` exit.
- The `fault` block returns from the generated block immediately.  Checks cover
  mov/arithmetic/inc/dec/neg/cmp/test/movzx/movsx/imul memory operands, push,
  pop, call (target and pushed return address), ret and indirect jmp/call.
- Arithmetic writes now precede the flag-helper call, so a faulting memory
  destination cannot commit RFLAGS.  `POP RSP` and stack-pointer updates happen
  only after a successful load/store.
- `GuestState.error_pc` is set by `aev_mem.c` from the instruction-address RIP;
  the AOT dispatcher returns the error, and the JIT dispatcher returns failure.
- `src/aev_mem.c` was not changed for F3 (the existing range check already
  refused the access; the bug was the generated block continuing).

### F4 — POP RSP and 16-bit stack forms

- `src/ir_emitter.cpp`: `pop rsp` writes the popped value to RSP and skips the
  ordinary `old RSP + 8` update.  Other destinations still get that update.
- 16-bit `pushw`/`popw` are rejected in `build_program` and in the emitter.  The
  diagnostic contains `unsupported 16-bit push/pop` plus the instruction.
- `push imm8`/`push imm32` are still accepted as sign-extended 64-bit pushes;
  only operand size 2 is rejected.

### F5 — 64-bit IMUL CF/OF

- `src/aev_mem.c`: for width 8 the truncated `r` is now sign-extended with
  `(__int128)(int64_t)r` before comparison with the full signed product, instead
  of being zero-extended.  Widths 1/2/4 keep the existing shift-based
  sign-extension.
- The helper still sets architecturally undefined ZF/SF/PF to zero; no test
  judges those bits.

## 4. Regression integration

New ctest test: `p0_core_fix_semantics`, run by
`scripts/run_semantics_tests.sh`.  It assembles the committed
`testdata/semantics/*.s` counterexamples with
`clang --target=x86_64-pc-windows-msvc` and checks:

- F1: translator and JIT reject every committed LOCK form, with address and
  mnemonic in the diagnostic.
- F2: each 16/32/64-bit two/three-operand case executes through
  `aev-jit-probe`, which uses the real emitter + ORC LLJIT and checks
  `JIT compile ... triple=arm64`; results are read from the executed state, not
  from IR text.
- F3: each fault case runs through both `aev-jit-probe` and a separately loaded
  ARM64 AOT dylib (`aev-aot-probe`).  Checks include the exact `error_pc`,
  unchanged RSP, unchanged faulting destination/previous values, and a
  zero-initialized destination memory that must remain zero.  Stack faults are
  tested both as first and second instructions so the block-entry address cannot
  satisfy the PC check.
- F4: `pop rsp` runs through JIT and AOT; 16-bit forms are rejected in
  translation and in the JIT path.
- F5: `aev-flags-test` runs the host helper and the helper exported by the AOT
  dylib; the 64/32/16-bit boundary matrices also run through
  `aev-jit-probe`.

The original `p0_engine_closed_loop` test is unchanged in behavior and still
checks the original five MSVC fixture cases, the JIT hand-off, ASLR load,
unknown-target failure, unsupported-instruction rejection and manifest boundary.

### Test expectation note

The first draft of the `fault_store` regression expected `error_pc=0x140001000`.
`fault_store.s` begins with the 5-byte `mov eax,0x55`, so the faulting store is
at `0x140001005`; the fixed value `0x140001005` is the correct precise PC.  This
was a test-arithmetic correction, not a change of engine expectation.  The
pre-fix run reports the block start `0x140001000`, while the post-fix run
reports `0x140001005`, which is exactly the F3 distinction being tested.

## 5. Results

Commands and exit codes (also in `fix/commands.txt`):

| Command | Exit |
| --- | ---: |
| `cmake -S spikes/p0-engine -B .local/build-core-fix -G Ninja` | 0 |
| `cmake --build .local/build-core-fix` | 0 |
| `ctest --test-dir .local/build-core-fix --output-on-failure` | 0 (2/2 tests) |
| `scripts/run_tests.sh ...` (closed loop) | 0 (22 PASS, 0 FAIL) |
| `scripts/run_semantics_tests.sh ...` | 0 (364 PASS, 0 FAIL) |

Original closed-loop cases (all `rc=0`, Windows reference diff empty):

| Case | AOT blocks | JIT execs | JIT compiles | Steps |
| --- | ---: | ---: | ---: | ---: |
| prep_a | 38 | 2 | 1 | 40 |
| prep_b | 25 | 3 | 1 | 28 |
| hidden (held-out) | 27 | 4 | 1 | 31 |
| unaligned | 30 | 3 | 1 | 33 |
| crosspage | 18 | 2 | 1 | 20 |

Post-fix F1–F5 results:

- **F1**: all committed LOCK forms fail translation with the LOCK diagnostic;
  `aev-jit-probe` also fails at program setup with the same diagnostic.  No IR
  or AOT artifact is produced for those inputs.
- **F2**: `0x15` (21) for `imul eax,ecx,7`; `0x1e` (30) for two-operand;
  `0xfffffff7` (-9) for `imul eax,ecx,-3`; 64-bit `0x15`/`0x1e`/
  `0xfffffffffffffff7`; 16-bit `0x15`/`0x1e`/`0xfff7`.  All through ORC.
- **F3**:
  - `fault_load_second`: `error=1`, `error_pc=0x140001005`, RAX unchanged
    (`0xaaaaaaaaaaaaaaaa`), RBX=`0x11223344` (previous legal instruction),
    destination memory `0x00…00`, RSP unchanged.
  - `fault_load_first`: `error_pc=0x140001000`, RAX unchanged, destination
    memory zero, RSP unchanged.
  - `fault_store_second`: `error_pc=0x14000100a`; RAX=`0x11`, RCX=`0x22`,
    RBX unchanged, RSP unchanged.
  - `fault_store`: `error_pc=0x140001005`; RAX=`0x55`, RBX unchanged, RSP
    unchanged.
  - `fault_store_cross`: `error_pc=0x140001000`; the two in-range bytes remain
    zero; no write.
  - `fault_push`/`fault_pop`/`fault_ret`/`fault_call`: exact PC `0x140001000`;
    RSP/RIP unchanged; subsequent EBX write absent.
  - second-instruction stack variants: exact PC `0x140001005`, proving the PC is
    not the block entry.
  - All of the above also pass through the loaded AOT dispatcher (`rc=1`).
- **F4**: `pop rsp` gives RAX=`mem+0x240`, RSP=`mem+0x248` through JIT and AOT;
  `pushw`/`popw` are rejected with `unsupported 16-bit push/pop`.
- **F5**: 64-bit `-1*1`, `min*1`, `max*1`, `-1*-1` → CF=OF=0;
  `min*-1`, `max*2`, `2^62*2`, `-2*max`, `min*min` → CF=OF=1.  32-bit and
  16-bit matrices match.  The same matrix passes against
  `libguest_aot.dylib`'s `aev_flags_imul`.

## 6. Hashes

Full list: `.local/results/P0-CORE-FIX-01/fix/hashes.txt`; input object hashes:
`fix/semantics-committed/input-objects.sha256`; AOT dylib/object/IR hashes:
`fix/semantics-committed/aot-artifacts.sha256`.

| Item | SHA-256 |
| --- | --- |
| Baseline commit | `a4ef11c723c929a61030ef3e0ae8fc8fba537ab2` |
| Code commit | `9f21649b2a0470d2d6dc4bea576785af22ac6e8c` |
| `testdata/guest_x64.obj` | `a12fa7fff6087e2e0ae95a09e395cd41874f5871674a4feda0cbff8ca9da7b1e` |
| fixed `generated/guest_aot.ll` | `df9aa1d6902aaed26995e7e5cfc08022f817ee95c8301065c1438b3804b21d1f` |
| fixed `generated/guest_aot.o` | `5b158e59aae4ef0cd869c8a161fd48345a2723db022c21bdf60c36e7fb2beb8d` |
| fixed `generated/libguest_aot.dylib` | `a520ea44fe552c4602b21a11db8078f75bd04d2826b760a15d5b6e39bff65f72` |
| fixed `aev-runner` | `86142db8015ea6da4a8fec0ea4176d10cf768da1bd3fd0398a8e974de9444c77` |
| `aev-jit-probe` | `f23e073add4949d3cb70d0beaf50f091d96a71a3d999c050021689378d39c22d` |
| `aev-aot-probe` | `829f8968d393a7782b6eb756344ae60ccb97d57571fae4d09686d74782de4fa7` |
| `aev-flags-test` | `c745597f1bfc734ec97e3f1de3bbf986fc1f42a2d92480885fb993b848649f4c` |
| `scripts/run_semantics_tests.sh` | `88d648a309b91b26e3c4feee97538532b10c96b7cb60b5e344c0329a437c4bbf` |

## 7. Evidence paths

- Final ctest: `.local/results/P0-CORE-FIX-01/fix/logs/ctest-committed.log`
- Final configure/build: `fix/logs/configure.log`, `fix/logs/build.log`
- Closed-loop stdout: `fix/logs/closed-loop-committed.stdout`
- Closed-loop details: `fix/closed-loop-committed/` (including empty
  `reference.diff`, `summary.txt`, `logs/`)
- Semantics stdout: `fix/logs/semantics-committed.stdout`
- Semantics details: `fix/semantics-committed/` (`summary.txt`, `logs/`,
  `input-*.sha256`, `aot-artifacts.sha256`, `environment.txt`)
- Separate reproduction: `repro-compiler/` (`REPRO.md`, `repro-summary.txt`,
  `after-fix-summary.txt`, `logs/`)

## 8. Untested / limits

- **8-bit IMUL is not executed**: the only 8-bit form is one-operand `imul`,
  which remains outside the subset and is rejected.  Width 1 is covered only by
  the direct `aev_flags_imul` helper matrix.  A follow-up would need a
  one-operand IMUL implementation (AX/DX:AX/EDX:EAX/RDX:RAX and high-half
  register writes), which was outside F1–F5.
- **No atomics/TSO**: all LOCK forms are rejected; no atomic IR, fences or
  memory-order model was added.
- **F5 AOT path is the helper in the AOT dylib, not an AOT-generated IMUL
  block**: the spike intentionally keeps `imul` out of AOT to exercise the
  JIT hand-off, so there is no generated AOT `imul` to execute.  The AOT dylib
  exports the fixed helper and the test calls it directly; the JIT path executes
  generated ARM64 code that calls the helper.
- **No PE loader, relocations, imports, TLS, section permissions, signals,
  guest exception delivery, unwind, threads, callbacks or code invalidation**
  were added.  F3 reports faults through `error_code`/`error_pc`; it is not a
  Windows exception/unwind path.
- **No guest page tables/page-permission/cross-page guest faults.**  The
  `crosspage` case remains a host-page crossing inside one linear region.
- **No `MAP_JIT`/hardened-runtime/signing path; no persistent JIT cache.**
- **No Windows reference run for the new counterexamples.**  The expected
  values are ISA-defined and, for F1–F4, independently observable in the
  engine's own state; the new inputs are not MSVC-compiled fixture code.
- **No performance measurement or claim.**  The added per-memory-op error checks
  are a correctness mechanism, not an optimized lowering.
- **No GUI/real application integration.**  This task is limited to the engine
  semantics.

## 9. Integration status

The code and tests are committed on `p0/core-fix-01` (code commit
`9f21649…`), not pushed and not merged into main.  The coordinator should review
the diff and run the same ctest on the exact commit; QA should independently
rerun `ctest` and inspect the raw logs above.  The main worktree and
`.local/results/CODEX-REVIEW-20261105/` were only read, never written.
