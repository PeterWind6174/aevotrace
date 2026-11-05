# P0 callback fixture (x64 COFF)

Self-authored Windows x64 test input for the P0 Host↔Guest callback bridge. It is a fixed COFF object, not a PE application and not a product artifact. The macOS translator consumes these bytes; it does not recompile the C source.

## Files

- `aev_callback_fixture.c` — source used to generate the object (MSVC x64, `/c /O2 /GS- /Brepro /W4`; function placed in `.aevtext`).
- `aev_callback_fixture.obj` — fixed input, SHA-256 `4f6929f7c71c82169295b6675517a839ae9533a85941e2632115bf0ef47328dc`.
- `aev_callback_ref.c` — Windows x64 reference driver. It links the same `.obj` and supplies a native `GuestCtx.update` helper.
- `build_msvc.ps1` — reproducible Windows build/run script (VS 18 x64 tools). It prints the object SHA-256 and `BUILD_RUN_OK`.
- `reference_stdout.txt` — expected Windows `CASE` lines (helper returns `op`).
- `aev_callback_fixture.disasm.txt`, `.sections.txt`, `.relocations.txt`, `.manifest.json`, `.translate.txt` — evidence generated from the committed object.

## Object evidence

- Format: COFF x86-64, function `aev_guest_callback`.
- `.aevtext`: 86 bytes, 27 instructions, 5 basic blocks.
- Translation (manifest): synthetic entry `0x140001000`; 4 AOT blocks + 1 JIT-only block at `0x140001020` (`imul`).
- `.aevtext` has no relocations; `.pdata`/`.chks64`/`.debug$S` metadata is not translated.
- The function calls `qword ptr [r9]`; on Windows `[r9]` is the native helper address, on macOS the bridge writes a GuestImport stub address into the same Guest memory. The machine code is identical.

## Guest ABI (summary)

```c
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned short u16;
typedef struct GuestCtx {
    u64 (*update)(u64 hash, u64 len, u64 op);
    u64 *counter;
} GuestCtx;

u32 aev_guest_callback(unsigned int op, const u16 *text, u64 len, GuestCtx *ctx);
```

Guest computes 32-bit FNV-1a over UTF-16 code units, calls `update(hash, len, op)`, increments `*counter` after the helper returns, and returns `hash ^ (u32)helper_return`. The helper must return `op` (the second/`a2` argument on the host side), so both the Guest→Host arguments and the Host→Guest return value affect the result. The Host must not compute the hash.

Expected Windows output (`reference_stdout.txt`):

```text
CASE alpha op=7 len=6 result=3828036654 counter=1
CASE beta op=9 len=8 result=614625053 counter=2
CASE hidden op=23130 len=7 result=651115151 counter=3
```

## Windows reference

From this directory on the Windows reference machine:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_msvc.ps1
```

The script finds VS via `vswhere`, initializes `vcvars64.bat`, compiles `aev_callback_fixture.c` to a separate `aev_callback_fixture.rebuilt.obj`, compares the `.aevtext` instruction stream against the committed object, links the committed `aev_callback_fixture.obj`, runs the driver, and prints `FIXTURE_SHA256 ...` / `AEVTEXT_INSNS ... MATCH` / `BUILD_RUN_OK`. The committed object hash must remain `4f6929f7...`. Whole-object hashes include path/debug/checksum metadata, so cross-directory rebuilds are checked by their `.aevtext` disassembly; the committed object is the fixed input. The driver also prints `HELPER` lines; `reference_stdout.txt` intentionally contains only the stable `CASE` lines for diffing against the macOS bridge.

## macOS translation (later phase)

```sh
aev-translate --input spikes/p0-engine/testdata/callback/aev_callback_fixture.obj \
  --function aev_guest_callback --entry aev_guest_callback \
  --out-ll <build>/callback_aot.ll --out-header <build>/callback_layout.h \
  --out-manifest <build>/callback_manifest.json
```

The bridge must use synthetic entry `0x140001000` and a GuestImport stub in `0x140010000 + id*0x10` (id >= 1). See `docs/p0/CALLBACK-ABI-20261105.md`.
