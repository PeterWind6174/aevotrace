# Route B: existing x86-64 execution engines (evaluation only)

Task: P0-ENGINE-01.  This is a desk evaluation, not an integration.  It records
what can be verified from the upstream documentation/source at the commits
below and why those engines were not selected as the P0 primary path.

Date of inspection: 2026-11-05.

| Candidate | Pinned upstream commit | License | Primary host / binary model |
| --- | --- | --- | --- |
| QEMU (TCG user mode) | `f29ccc287a6cfdd51d3a4fd17fbe9af3310a0ac5` | GPL-2.0 (top-level `COPYING`) | Linux and BSD user mode; guest ELF; dynamic translation |
| FEX-Emu | `eff9f459d8a13dc80aa9edb331c3da0e98334a38` | MIT (`LICENSE`) | Arm64 **Linux**; guest ELF; Wine/Proton for Windows programs; JIT |
| Box64 | `59b3eb1555406db697941ddd5e414c7085b90e4f` | MIT (`LICENSE`) | non-x86_64 **Linux**; guest ELF; DynaRec JIT |

## QEMU TCG

Verified statements from upstream:

- QEMU describes itself as a **dynamic translator**: it converts a piece of
  guest code to host instructions when it first encounters it, and keeps
  Translation Blocks (TBs) in a runtime cache.  The TCG internals document also
  describes direct block chaining, host-page write protection for
  self-modifying-code invalidation, `longjmp` exception handling and host
  `SIGSEGV`/`SIGBUS` mapping.  See:
  <https://github.com/qemu/qemu/blob/master/docs/devel/tcg.rst>.
- The user-mode documentation lists only **Linux** and **BSD** as supported
  user-mode host operating systems; macOS is not listed:
  <https://github.com/qemu/qemu/blob/master/docs/user/main.rst>.
- The top-level license is GPL-2.0:
  <https://github.com/qemu/qemu/blob/master/COPYING>.

Consequences for Aevotrace:

- No supported public "compile a guest function to a relocatable ARM64 object"
  API.  TBs are runtime allocations tied to a QEMU CPU state, chaining slots,
  invalidation lists and the main loop.  Extracting them would require a
  GPL-covered fork and an internal-ABI integration, not a stable artifact
  exporter.
- There is no AOT-first / JIT-fallback handoff contract between an offline
  object and a separately loaded process.  TCG itself is the JIT; the cache is
  not an AOT artifact.
- QEMU user mode is not a macOS host in the upstream supported list, so using
  it as the P0 macOS ARM64 runtime would add an unsupported host port plus
  Windows PE/ABI work.
- GPL-2.0 makes static/derived combination with the current non-commercial
  Aevotrace licensing a release-engineering problem that must be resolved
  before any product path.  This evaluation does not claim that GPL
  combination is impossible; it records the constraint.

## FEX-Emu

Verified statements from upstream:

- README: "A fast usermode x86 and x86-64 emulator for **Arm64 Linux**";
  requires an x86-64 rootfs; Wine/Proton can run Windows programs on top of the
  Linux layer.  It mentions an "experimental code cache" for reducing
  stuttering, i.e. a **JIT code cache**, not an offline AOT object format:
  <https://github.com/FEX-Emu/FEX/blob/main/Readme.md>.
- FEXCore is a custom IR plus a JIT; the project is a process-level emulator
  with a Linux syscall/personality layer.  It does not document a stable public
  AOT export/relocation API.
- License: MIT (<https://github.com/FEX-Emu/FEX/blob/main/LICENSE>).

Consequences: MIT is compatible with a product path, but the Linux ELF/syscall
personality and process-level JIT are not the Windows x64 → macOS ARM64 model.
Adopting FEXCore would still leave PE loading, Windows ABI/personality, macOS
loading/signing, AOT export/relocation, and AOT↔JIT state/exception contracts
to build.  Its JIT cache cannot be reused as the required AOT artifact.

## Box64

Verified statements from upstream:

- README: "Box64 enables running x86_64 **Linux** programs ... on non-x86_64
  **Linux** systems"; DynaRec is a JIT, and DynaCache stores generated code in
  `~/.cache/box64` for faster subsequent launches.  DynaCache is a JIT cache,
  not an offline linkable AOT object:
  <https://github.com/ptitSeb/box64/blob/main/README.md>.
- License: MIT (<https://github.com/ptitSeb/box64/blob/main/LICENSE>).

Consequences: same structural mismatch as FEX, plus no documented Windows PE
or macOS runtime.  MIT is not the blocker; the missing AOT export, Windows
personality and macOS host integration are.

## Comparison matrix

Ratings are for the P0 question "can this be the primary engine for Windows
x64 → macOS ARM64 with AOT artifacts and JIT fallback?".

| Dimension | A: Capstone + own semantics + LLVM | QEMU TCG | FEX | Box64 |
| --- | --- | --- | --- | --- |
| x64 semantic coverage | small declared subset in this spike; extendable | very broad | very broad (AVX/AVX2 per project) | broad |
| ARM64 generation | LLVM AArch64 object + ORC JIT | TCG ARM64 backend | custom ARM64 JIT | ARM64 DynaRec |
| AOT export / relocation | **yes**: LLVM object, Mach-O dylib, dyld relocations | none exposed | none exposed | none exposed |
| JIT fallback | **yes**: ORC block compile, returns to AOT dispatcher | JIT is the only mode; no AOT handoff | JIT-only | JIT-only |
| Exception recovery | not implemented in spike; explicit gap | internal longjmp/signal mechanisms, not a public artifact ABI | internal signal handling | internal signal handling |
| Code invalidation | not implemented; artifact must be regenerated | page-protection/TB invalidation | JIT cache invalidation | JIT cache invalidation |
| Embedding in a macOS app | small library + dylib; feasible | process-level, GPL, no public embed API | Linux process-level | Linux process-level |
| Build cost | small spike; LLVM 162 MB dylib at runtime | large; GPL fork | large | large |
| License | Capstone BSD-3-Clause; LLVM Apache-2.0 with LLVM exception | GPL-2.0 | MIT | MIT |
| macOS ARM64 host | native arm64 build, tested | not in upstream user-mode OS list | Linux only | Linux only |

## Decision

Route A is the P0 primary path.  The spike demonstrates the specific missing
properties that the existing engines do not provide as a documented product
interface: a relocatable ARM64 Mach-O artifact loaded by a separate process,
and a real JIT fallback that returns to AOT code at a block boundary.  Route B
remains useful as a semantic/coverage reference and as a possible future
compatibility layer, not as the P0 AOT exporter.

## What was *not* verified for Route B

- No QEMU/FEX/Box64 source was built or run in this task.
- No candidate's JIT cache format was parsed or imported.
- No claim is made about their current Windows/macOS ports beyond the upstream
  documentation cited above.
- The QEMU GPL analysis is a licensing-risk note, not legal advice.
