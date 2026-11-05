# P0-ENGINE-01 spike: x86-64 → ARM64 AOT/JIT closed loop

Status: **spike, not a frozen product ABI and not a support claim**.  This is the
P0-ENGINE-01 evidence package: a real MSVC-built x86-64 COFF fixture is decoded
from its `.text` bytes, translated to LLVM IR, compiled to an ARM64 Mach-O
object, linked into a dylib, loaded by a fresh runner process, and executed.
A block containing `imul` is deliberately omitted from AOT coverage and is
compiled/executed by LLVM ORC at runtime; it returns to the AOT dispatcher.

## Layout

```
include/aev_guest.h            GuestState + helper ABI (spike-only)
include/aev_guest_fields.def   field order shared by C/C++ and IR emitter
src/coff.{hpp,cpp}             minimal COFF object parser
src/x64.{hpp,cpp}              Capstone x86-64 decode
src/translator.{hpp,cpp}       function/block discovery, AOT coverage boundary
src/ir_emitter.{hpp,cpp}       x86-64 subset -> textual LLVM IR
src/translate_main.cpp         CLI: COFF -> guest_aot.ll + layout header + manifest
src/aot_runtime.c              AOT dispatcher, block table lookup, JIT hand-off
src/aev_mem.c                  memory bounds checks, flags helpers, AOT trace
src/runner.cpp                 dlopen AOT dylib, ORC JIT, test driver, ASLR/logs
testdata/                      MSVC input fixture, source, disassembly, reference
scripts/run_tests.sh           closed-loop and negative-control checks
ROUTE_B.md                     QEMU/FEX/Box64 comparison and licenses
REPORT.md                      actual commands, hashes, evidence, gaps
```

## Input fixture

`testdata/guest_x64.obj` is compiled on Windows with MSVC 14.51 (`/O2 /GS-
/Oy- /Brepro`; `/Brepro` makes the COFF timestamp deterministic).  The guest
source is `testdata/guest_x64.c`; it is **not** recompiled on macOS.  The
translator reads the object's `.text` sections.  `testdata/ref_main.c` is the
Windows x64 reference driver linked against the same object;
`testdata/reference_stdout.txt` is its stable output.

The fixture exercises:

- a loop with conditional branches (`test`/`je`, `cmp`/`jl`, `jle`);
- array read/write through a guest pointer;
- an indirect call through the fourth argument (`call r15`) into a second
  translated guest function;
- a variable `imul eax, ebp`, intentionally outside the AOT subset.

Fixed input hashes and the reference output are recorded in `REPORT.md`.

## Build and run

Prerequisites on macOS arm64:

- CMake >= 3.20, Ninja;
- Apple clang (host arm64) and a macOS SDK;
- Capstone 5.0.9 at `/opt/homebrew/opt/capstone`;
- LLVM 23.1.4 at `/opt/homebrew/opt/llvm` (the runner links `libLLVM-23.dylib`
  and uses the LLVM C API/ORC LLJIT);
- Windows MSVC only for regenerating the input/reference fixture.

```sh
# from the worktree root
cmake -S spikes/p0-engine -B .local/build-p0-engine -G Ninja
cmake --build .local/build-p0-engine
ctest --test-dir .local/build-p0-engine --output-on-failure
```

Manual run:

```sh
.local/build-p0-engine/aev-runner \
  --aot .local/build-p0-engine/generated/libguest_aot.dylib \
  --input spikes/p0-engine/testdata/guest_x64.obj \
  --function guest_kernel --function guest_op \
  --case prep_a --trace --dump-jit
```

`--print-symbols` prints the AOT load base and every guest block RIP → dylib
symbol → host address mapping.  `--bad-op` forces an unknown indirect target and
must fail explicitly.  `--dump-jit` disassembles the ARM64 bytes returned by
`LLVMOrcLLJITLookup`.

The test script writes logs, `file`/`otool`/`nm` output, hashes and the Windows
reference diff under `$BUILD_DIR/results` (override with `AEV_RESULTS_DIR`).

## Translation model

- Guest code addresses are synthetic: image base `0x140000000`, first function
  at `0x140001000`, 4 KiB stride.  Data pointers are host pointers in one
  registered memory region (identity data mapping).  This is a spike
  simplification, not the project memory model.
- The translator discovers basic blocks from direct branch targets and
  fallthroughs.  Each AOT block becomes an exported ARM64 function
  `aev_blk_<guest-rip>`; the table is `aev_blocks`/`aev_block_count`.
- The AOT dispatcher in `aot_runtime.c` finds the block for `GuestState.rip`,
  calls it, and repeats.  `call` pushes the guest return address and sets RIP;
  `ret` pops it.  Guest-to-guest calls therefore use the guest stack exactly as
  x86-64 code expects, while host recursion is replaced by the dispatcher.
- A block containing an AOT coverage boundary is absent from the AOT table.
  On a miss, the dispatcher calls the runner's `jit_dispatch`, which:
  1. finds the block in the original COFF-derived instruction stream;
  2. emits IR with the `imul` semantics enabled;
  3. compiles it with LLVM ORC LLJIT;
  4. dumps the returned ARM64 bytes (optional) and calls it;
  5. the JIT block sets `GuestState.rip` to the successor and returns to AOT.
- Unknown RIP / unknown indirect target / a RIP in the middle of a block is an
  explicit error, never a zero return.

### Implemented x86-64 subset

`mov`, `lea`, `push`, `pop`, `add`, `sub`, `and`, `or`, `xor`, `cmp`, `test`,
`inc`, `dec`, `neg`, `imul` (JIT path only in this spike), `movzx`, `movsx`,
`movsxd`, `jmp` (direct/indirect), `jcc` (all common conditions), `call`
(direct/indirect), `ret`, `cmovcc`, `nop`, `endbr64`.

Not implemented and rejected with an address + mnemonic + bytes: `cdq`/`idiv`,
`lock`/`xadd` and other atomics, SSE/AVX, x87, string ops, `syscall`, etc.
There is no silent fallback.

### Memory model in this spike

`aev_load`/`aev_store` do a linear range check against one `mmap`ed region and
use `memcpy`, so unaligned and host-page-crossing accesses are supported and
tested.  There is **no** guest virtual-address translation, page permission,
dirty/accessed bit, atomicity, or memory-order (TSO) model.  Atomic instructions
are rejected.  The `crosspage` test crosses a host page inside the single
region; it does not test guest page faults.

## Deliberate limitations / not frozen

- No PE loader, relocations, imports, TLS, resources, unwind/exception
  recovery, signals, threads, self-modifying code, or code invalidation.
- No `MAP_JIT`/hardened-runtime packaging path; ORC's default memory manager is
  used.  The dylib is unsigned and not distributed.
- The JIT compiles one known block from the same translator library; there is no
  persistent JIT cache and no claim that a JIT cache equals an AOT artifact.
- `imul` is intentionally excluded from AOT to force the hand-off; the JIT
  implements it.  This is a coverage-boundary test, not a statement that the
  engine cannot implement `imul`.
- The AOT artifact format is spike-local (`guest_aot.ll`, `guest_aot.o`,
  `libguest_aot.dylib`, `guest_manifest.json`, `guest_layout.h`).  It is not a
  v0 frozen contract.
- Evidence is functional only; no performance conclusion is made.

See `REPORT.md` for actual commands, exit codes, hashes, log paths, and the
list of untested items.
