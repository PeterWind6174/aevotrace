# P0-TRACE-AOT-01 trace fixture

Self-authored Windows x64 COFF fixture for the bounded execution-record → offline
AOT supplement experiment. It is test input, not a product PE loader or app.

## Fixed input

- `aev_trace_fixture.obj` SHA-256
  `145800f4a4729c201aab2c94a74937d349e01f6ec7871658b6becd3773bd1205`.
- MSVC x64 `/c /O2 /GS- /Brepro /W4`; function `aev_trace_guest` in its own
  `.aevtext` section (0x4c bytes).
- Synthetic entry `0x140001000`.
- Blocks (before coverage selection):
  - `0x140001000` entry/branch, size 21 (`0x15`)
  - `0x140001015` B-only IMUL body, size 17 (`0x11`)
  - `0x140001026` A-only add/xor body, size 9 (`0x9`)
  - `0x14000102f` helper call frame, size 9 (`0x9`)
  - `0x140001038` counter/return, size 20 (`0x14`)
- Default translator coverage excludes the IMUL block, so B first executes it via
  real JIT; A does not execute it at all.

## ABI and business semantics

```c
typedef struct GuestCtx {
    u64 (*update)(u64 result, u64 x, u64 op);
    u64 *counter;
} GuestCtx;
u32 aev_trace_guest(unsigned int op, unsigned int x, GuestCtx *ctx);
```

- A: `op != 0xA5A5`, `result = (x + 0x55) ^ 0x0F0F`.
- B: `op == 0xA5A5`, `result = ((x * 17) ^ 0x1234) + 0x200` (IMUL block).
- Helper returns `op`; Guest returns `result ^ op`; counter increments after helper.
- Windows reference links this exact `.obj`; expected output in
  `reference_stdout.txt`:
  ```text
  CASE A op=1 x=7 result=3922 counter=1
  CASE B op=42405 x=9 result=45320 counter=2
  ```

## Windows reference

From this directory on the Windows reference machine, with the committed
`aev_trace_fixture.obj` copied next to the script/source:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_msvc.ps1
```

The script verifies the committed object SHA-256, compiles `aev_trace_fixture.c`
to `aev_trace_fixture.rebuilt.obj` only as a diagnostic, links the driver with
the committed object, runs A/B, dumps disassembly/relocations, and prints
`FIXTURE_SHA256 ...`, `REBUILT_SHA256 ...`, `REBUILT_MATCH ...`,
`COMMITTED_OBJECT_BUILD_RUN_OK`. Under different MSVC versions the rebuilt
object bytes may differ while the `.aevtext` instruction sequence is the same;
the behavior reference always uses the committed object.

## Evidence

`aev_trace_fixture.disasm.txt` shows the B-only `imull` at `0x15`; the A path
at `0x26` has no IMUL. The block bytes and offsets are consumed by the bridge
trace and supplement validator.
