# P0-ENGINE-01 completion report

Status: **completed spike with explicit limitations**.  This is not a frozen
product ABI and not a claim of support for real applications.

- Worktree: `/Users/peterwind/Script/Systems/Aevotrace/.worktrees/p0-engine`
- Branch: `p0/engine-spike`
- Baseline commit: `586662a4ff09465d7d3aa453fb44e5aa6e880c79`
- Code commit: see the `P0-ENGINE-01` commit on this branch (this report is
  committed in the same change set; `git log -1` is the authoritative hash)
- No main-worktree source was modified; nothing was pushed.

## 1. What was built

Route A: Capstone 5.0.9 x86-64 decode + a declared small semantics/lifting
layer + LLVM 23.1.4 AArch64 code generation + a separately loaded ARM64 Mach-O
dylib + LLVM ORC LLJIT fallback.

```
MSVC x64 COFF .text
  -> Capstone decode / basic-block discovery
  -> own GuestState + flag/memory helper ABI
  -> textual LLVM IR (AOT blocks + table; JIT block IR on demand)
  -> clang --target=arm64-apple-macos14.0
  -> guest_aot.o + aot_runtime.o + aev_mem_aot.o
  -> libguest_aot.dylib (Mach-O arm64)
  -> aev-runner (new process) dlopen + dlsym
  -> AOT dispatcher; missing block -> ORC compile from original x64 block
  -> JIT block sets RIP and returns to AOT dispatcher
```

The runner is a separate executable from both the translator and the AOT
dylib.  The translator never recompiles `guest_x64.c`; it reads the COFF
section bytes.  JIT IR is generated at runtime from the decoded x86-64 block
(the same input bytes), not from a precompiled host function.

## 2. Input fixture and Windows reference

Windows reference machine: `ssh win`, VS 18.11.3, MSVC tools 14.51.36231,
x64.  Windows task directory:
`C:\Users\PeterWind\AevotraceP0\P0-ENGINE-01\input` (no collision with the QA
task directory).

Guest source: `testdata/guest_x64.c`.  Build flags: `/c /O2 /GS- /Oy- /Brepro
/FA`.  `/Brepro` made two consecutive MSVC builds produce the identical object
hash.

| Item | SHA-256 |
| --- | --- |
| `testdata/guest_x64.obj` (whole COFF) | `a12fa7fff6087e2e0ae95a09e395cd41874f5871674a4feda0cbff8ca9da7b1e` |
| `.text$mn` for `guest_kernel` (128 bytes) | `f8b91b47a92695cd06d04b9d49ff296c967e20008f9633452079640e3e5415fe` |
| `.text$mn` for `guest_op` (9 bytes) | `ccd52689088cc01f51084b3b41ab38ae2d0ad7f4e10ed4c94959bdc8469840d4` |
| `testdata/guest_x64.c` | `c20a20804a12ca1a25da3c04272b03853db83b9503563589181d79c3f4c40789` |
| `testdata/ref_main.c` | `a9d38aabafbb7e48567aebe40f57c84e913cae7db157f2bbfcecb914e86a4a90` |
| `testdata/reference_stdout.txt` | `7d623c9acca20c8a546648a50067161e1e5ee7c0fd08c69ad994bfa3fff81a9c` |
| Windows `p0_engine_ref.exe` | `9159c74f99fdca6a6ab3d9653e70c1e1e8c474a099b906b0c4e060f92adda9a1` |

Windows reference output (exit code 0):

```
case=prep_a result=305419957 arr=305419957,-4,7,19,0,-13,21,5
case=prep_b result=16909036 arr=-9,2,-3,4,5,-6,16909036,8
case=hidden result=195948858 arr=100,-101,102,-103,104,-105,106,195948858
case=unaligned result=1437218558 arr=13,-8,21,0,-5,34,55,1437218558
case=crosspage result=253635724 arr=17,-22,31,-44,253635724,-66,77,-88
```

The x86-64 disassembly is kept at `testdata/guest_x64.disasm.txt`.  The two
translated functions are:

- `guest_kernel` at COFF `.text$mn` offset 0, 128 bytes, 45 instructions, 9
  basic blocks (8 AOT + one JIT-only `imul` block);
- `guest_op` at COFF `.text$mn` offset 0, 9 bytes, 4 instructions, one AOT
  block.

`guest_kernel` includes a loop, conditional branches, array read/write, an
indirect `call r15`, and a variable `imul eax, ebp`.  `guest_op` is reached by
the translated indirect call.

## 3. Guest PC -> generated symbol mapping

Synthetic guest image base is `0x140000000`; functions are assigned in CLI
order at `0x140001000`, `0x140002000`:

| Guest RIP | Guest function / block | AOT symbol | AOT? |
| --- | --- | --- | --- |
| `0x140001000` | guest_kernel entry/prologue/test/jle | `aev_blk_140001000` | yes |
| `0x140001024` | loop preheader (spill rbx, nop) | `aev_blk_140001024` | yes |
| `0x140001030` | loop head (`mov ebx`, `test bl`, `je`) | `aev_blk_140001030` | yes |
| `0x140001039` | indirect-call arm | `aev_blk_140001039` | yes |
| `0x140001040` | post-call `jmp` | `aev_blk_140001040` | yes |
| `0x140001042` | even arm, `mov eax,ebx; imul eax,ebp` | `aev_jit_140001042` (ORC) | **no** |
| `0x140001047` | join/cmovs/inc/cmp/jl | `aev_blk_140001047` | yes |
| `0x14000105c` | restore rbx / fallthrough | `aev_blk_14000105c` | yes |
| `0x140001061` | epilogue/ret | `aev_blk_140001061` | yes |
| `0x140002000` | guest_op entry | `aev_blk_140002000` | yes |

`guest_manifest.json` records the same mapping; `--print-symbols` resolves each
AOT symbol with `dlsym` and prints its host address.  `nm -gU` shows
`_aev_blocks`, `_aev_block_count`, `_aev_aot_run` and the nine `_aev_blk_*`
symbols.  `_aev_blk_140001042` is deliberately absent, so the JIT boundary is
visible in the artifact itself.

`file` output:

```
guest_aot.o:          Mach-O 64-bit object arm64
libguest_aot.dylib:   Mach-O 64-bit dynamically linked shared library arm64
aev-runner:           Mach-O 64-bit executable arm64
```

`otool -tv libguest_aot.dylib` begins with real AOT block code (excerpt):

```
_aev_blk_140001000:
0000000000000560  stp x22, x21, [sp, #-0x30]!
0000000000000564  stp x20, x19, [sp, #0x10]
...
0000000000000580  bl  _aev_note_aot
0000000000000584  ldp x2, x8, [x19, #0x30]
0000000000000588  add x1, x8, #0x10
...
```

Full evidence: `.local/results/P0-ENGINE-01/final/otool_tv.txt`,
`nm_global.txt`, `block_symbols.txt`, `guest_manifest.json`.

## 4. AOT/JIT execution evidence

Commands and exit codes (full transcript:
`.local/results/P0-ENGINE-01/final/commands.txt`):

| Command | Exit |
| --- | --- |
| `cmake -S spikes/p0-engine -B .local/build-p0-engine -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo` | 0 |
| `cmake --build .local/build-p0-engine` | 0 |
| `ctest --test-dir .local/build-p0-engine --output-on-failure` | 0 (1/1 test passed) |
| `aev-runner ... --case prep_a --trace --dump-jit` | 0 |
| `aev-runner ... --case hidden` | 0 |
| `aev-runner ... --case prep_a --bad-op` | 3 (intentional failure) |
| `aev-translate ... --function divide_guest` | 1 (intentional unsupported instruction) |
| `aev-translate ... --function atomic_guest` | 1 (intentional `lock xadd` rejection) |

Per-case stats (all `rc=0`):

| Case | AOT blocks | JIT block execs | JIT compiles | Steps |
| --- | ---: | ---: | ---: | ---: |
| prep_a | 38 | 2 | 1 | 40 |
| prep_b | 25 | 3 | 1 | 28 |
| hidden (held-out) | 27 | 4 | 1 | 31 |
| unaligned | 30 | 3 | 1 | 33 |
| crosspage | 18 | 2 | 1 | 20 |

The trace for `prep_a` contains both:

```
AOT  rip=0x140001000
...
AOT  rip=0x140002000       # translated indirect-call target
...
JIT compile rip=0x140001042 symbol=aev_jit_140001042 host_addr=0x... source=ORC(LLJIT) block_jit_only=1
JIT exec rip=0x140001042 host_addr=... rax_before=0x16 rsp_before=...
JIT return rip=0x140001042 next_rip=0x140001047 rax_after=0xffffffe0 error=0
AOT  rip=0x140001047       # AOT resumes after the JIT block
```

With `--dump-jit`, the runner disassembles the code returned by
`LLVMOrcLLJITLookup` as ARM64.  The dump includes the real `imul` lowering:

```
JITCODE 0x...: mul w20, w8, w9
JITCODE 0x...: bl #...
JITCODE 0x...: str x20, [x19]
JITCODE 0x...: movk x8, #0x4000, lsl #16
```

Thus the JIT path is not a host fake return: ORC compiled a module at runtime,
the runner looked up `aev_jit_140001042`, disassembled its bytes, called it, and
the guest state advanced from RIP `0x140001042` to `0x140001047`, where the AOT
dispatcher continued.  The JIT block's source is generated from the original
COFF block at runtime (`tests` also assert `jit_compiles=1`).

AOT is not merely "the runner ran": generated AOT blocks call
`aev_note_aot`, and the trace shows the AOT entry, the translated
indirect-call target `guest_op`, and the post-JIT successor.  `otool`/`nm`
show those blocks as arm64 text in the separately loaded dylib.

## 5. Held-out input / branch variation

The translator/AOT artifact is data-independent; the test cases are driver
inputs.  `hidden` is the held-out case: its array values and branch pattern
were not used to select the AOT block set.  It produces a different result
(`195948858` vs `305419957`) and a different number of JIT executions (4 vs 2),
so this is not a fixed replay.  All five cases match the Windows reference
(`reference.diff` is empty).

## 6. ASLR / independent-process load

Two fresh runner processes loaded the same dylib at different ASLR slides:

```
aot_load_base=0x1047ac000   (pid different)
aot_load_base=0x1001f4000   (pid different)
```

The runner is a separate process each time; `dlopen`/`dlsym` perform the load,
and dyld resolves the block table/function pointers.  `guest_layout.h`,
`guest_manifest.json` and the `--print-symbols` output bind guest RIPs to
generated symbols.  The translated code uses synthetic guest RIP constants and
PC-relative AOT code; no host pointer is persisted in the guest state.

This is dynamic-load/ASLR evidence, not a relocation-schema proof for a future
PE loader.  The spike does not implement PE base relocations or section
remapping.

## 7. Memory model checks and diagnostics

- **Unaligned**: case `unaligned` passes an array pointer at `base+1`; the
  result matches Windows x64.  `aev_load`/`aev_store` use `memcpy`.
- **Host page crossing**: case `crosspage` uses an array starting at
  `page_size-6`, so a 4-byte load/store crosses a host page boundary inside the
  single mapped region; the result matches Windows x64.
- **Atomics / memory ordering**: not modeled.  A `lock xadd` object is rejected:

  ```
  translation setup failed: unsupported instruction 0x140001002:
    lock xadd dword ptr [rcx], eax (4 bytes)
  ```

  No TSO/acquire-release emulation is claimed.
- **OOB / permissions**: the helper does a linear range check, records
  `AEV_ERR_MEMORY`, and the dispatcher stops.  There is no guest page table,
  page permission, fault delivery, or dirty/accessed bit.
- **Unsupported non-atomic instruction**: `cdq`/`idiv` object is rejected:

  ```
  translation setup failed: unsupported instruction 0x140001005: cdq (1 bytes)
  ```

- Unknown indirect target: `--bad-op` produces

  ```
  JIT ERROR: unresolved guest indirect target rip=0xdead0000;
  no translated function or JIT block exists
  ```

  with non-zero exit and no fabricated result.

## 8. Artifact hashes

| Artifact | SHA-256 |
| --- | --- |
| `generated/guest_aot.ll` | `876f0f872de8f5f2b63a39bc30fc0730d76cc61ea58e4a980a0e65f0ebad9057` |
| `generated/guest_aot.o` | `b1c75dbd976e11935e71a71f7fa926e57859786186cd06ec3877a747b9ca99ca` |
| `generated/libguest_aot.dylib` | `d227034ce225bda09515472c5913ef92ab8337256d77728cb9d1c3a5cb958fba` |
| `aev-runner` | `263ea43183277e4db4ed03c8e80970fd9e16e4134b52de72e2599d6d55302565` |
| `aev-translate` | `a59bd03a63e4938d0473a238957bb4feeb7e0ae8d62be0903638a8bec5f82fba` |

Dylib/object hashes are from one exact build; the LLVM/clang version and the
generated IR are recorded with them.  The Mach-O dylib UUID/signature is not
part of the functional contract.

## 9. Route comparison conclusion

See `ROUTE_B.md` for the matrix and pinned upstream commits/licenses.

- **A (selected)**: Capstone + own lifting + LLVM AArch64 + Mach-O dylib + ORC
  JIT.  Gives an actual relocatable object and AOT↔JIT block handoff.  Cost:
  own semantics/ABI/PE/runtime work.  Capstone BSD-3-Clause; LLVM Apache-2.0
  with LLVM exception.
- **QEMU TCG**: broad semantics and JIT, but GPL-2.0, dynamic-only TB cache,
  no public AOT export/relocation interface, and upstream user mode lists only
  Linux/BSD hosts (no macOS).
- **FEX-Emu**: MIT, broad JIT/IR and Wine/Proton, but Arm64 **Linux** process
  emulator; no documented AOT object export; its cache is a JIT code cache.
- **Box64**: MIT, Linux-only process emulator with DynaRec/DynaCache; DynaCache
  is a JIT cache, not an AOT artifact; no Windows/macOS host path.
- No Route B engine was built or run in this task; conclusions are from pinned
  upstream documentation/source, as recorded in `ROUTE_B.md`.

## 10. Untested / known limitations

- No PE loader: no imports, exports, base relocations, TLS, resources, delay
  load, section permissions, or image mapping.
- No real Windows ABI bridge beyond the register/shadow-space convention used
  by the fixture; no variadic calls, no stack arguments beyond this function,
  no unwind/exception recovery.
- No signals, longjmp, thread suspension, callbacks from Host to Guest, or
  guest stack unwinding.
- No atomics/TSO/acquire-release memory-order model; atomics are rejected.
- No guest page table, page-permission faults, cross-page guest faults, or
  self-modifying-code invalidation.  The `crosspage` case is a host-page
  crossing inside one linear region.
- No `MAP_JIT`/hardened-runtime/signing path for the JIT; ORC default memory
  manager only.  The dylib is unsigned.
- JIT compiles one known block per process and does not persist a JIT cache.
- No AOT artifact version/hash validation at load; no invalidation if input
  bytes change.  Regeneration is required.
- No performance measurement or claim; only functional path evidence.
- The AOT code uses helper calls for memory and flags; this is a minimal
  lifting strategy, not the final optimized lowering.
- `imul` is deliberately AOT-excluded even though the JIT implements it; this
  is a test of the coverage boundary, not an engine capability statement.
- C++/C ABI of `GuestState` is a spike ABI.  The `aev_flags_imul` helper sets
  architecturally undefined ZF/SF/PF to zero; this fixture does not consume
  them after `imul`.

## 11. ABI / artifact entries for coordinator to consider freezing

Do **not** freeze these as product contracts without review; these are the
interfaces the spike actually exercised.

1. **Guest state**
   - `GuestState` is an array of 32 `uint64_t` fields in the order in
     `include/aev_guest_fields.def`; `aev_guest.h` must be generated/validated
     from the same source.
   - Minimum required fields: GPRs, RIP, RFLAGS, memory base/size, block/JIT
     counters, error code/PC, trace flag.  A product version should add
     explicit ABI version, XLAT/segment state, MXCSR/x87/vector state and
     thread/TEB pointers without reordering existing fields blindly.
2. **AOT entry**
   - `int aev_aot_run(GuestState *s, const AevRuntime *rt)`.
   - `AevRuntime` carries `jit_dispatch(ctx, s, rip)` and opaque `ctx`.
   - Dispatcher returns 0 at `AEV_RETURN_SENTINEL`; non-zero for explicit
     errors.
3. **Block ABI**
   - `void aev_blk_<guest-rip-hex>(GuestState *s)`.
   - `AevBlock { uint64_t rip; void (*fn)(GuestState *); }`, table
     `aev_blocks`, count `aev_block_count`.
   - `call` pushes the guest return address; `ret` pops it; blocks set
     `GuestState.rip` and return.
4. **JIT hand-off**
   - `jit_dispatch` must only accept a RIP that is a known basic-block start;
     missing AOT coverage is not the same as an unknown/unsupported target.
   - The JIT must set `GuestState.rip` to the successor and return to the AOT
     dispatcher.  A JIT block that cannot be compiled must produce a typed
     error, never a zero.
5. **Helper ABI**
   - `aev_load(s, addr, size, sign_extend)`,
     `aev_store(s, addr, value, size)`, flags helpers, `aev_note_aot(s, rip)`.
   - Size/width is bytes; addresses are 64-bit guest/host canonical values in
     this spike; a product memory manager must replace the linear check.
6. **Artifact manifest**
   - Record: input module SHA-256, `.text`/section hashes, function/block RIPs,
     AOT/JIT coverage, translator version, LLVM/clang version, target triple,
     GuestState/helper ABI version, generated object/dylib hashes, relocation
     assumptions, target macOS min version.
   - A JIT cache must not be accepted as an AOT artifact; AOT must be a
     loadable Mach-O object/module with resolved or explicit relocations.
7. **Code identity/version**
   - A block/RIP is valid only for the exact input bytes and artifact version.
     The spike has no runtime validation; P1 must bind artifact -> module bytes
     and invalidate on code modification.  Indirect targets must have an
     explicit table or be rejected.

## 12. Suggested next steps

1. Add a minimal PE section/relocation loader and map guest RIP/RVA consistently
   with the artifact manifest.
2. Freeze a versioned GuestState/helper/block ABI after review; add ABI layout
   checks/tests.
3. Replace the linear memory helper with a real region map (permissions,
   cross-page faults, no-execute), then add atomics/TSO decisions and tests.
4. Add exception/unwind and Host↔Guest callback contracts (including reentry
   and thread teardown) before GUI work.
5. Add code invalidation/version checks and a test that changes input bytes and
   proves stale AOT/JIT blocks are rejected.
6. Keep JIT and AOT semantic tests differential: MSVC reference + independent
   oracle + DSO/ASLR reloading.
