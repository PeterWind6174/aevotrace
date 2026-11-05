# Third-party dependencies (P0-ENGINE-01)

Versions were checked on the macOS ARM64 build host on 2026-11-05.

| Dependency | Fixed version | License | Use in this spike | Evidence |
| --- | --- | --- | --- | --- |
| Capstone | 5.0.9 (`/opt/homebrew/opt/capstone`) | BSD-3-Clause | x86-64 decode (`cs_disasm_iter`) and ARM64 JIT-code dump | `cstool -v`, upstream `LICENSE.TXT` <https://github.com/capstone-engine/capstone/blob/master/LICENSE.TXT> |
| LLVM | 23.1.4 (`/opt/homebrew/opt/llvm`) | Apache-2.0 with LLVM exceptions | AArch64 object generation via `clang --target=arm64-apple-macos14.0`; runtime JIT via LLVM C API ORC LLJIT | `llvm-config --version`, upstream `LICENSE.TXT` <https://github.com/llvm/llvm-project/blob/main/LICENSE.TXT> |
| Apple clang | 21.0.0 (Xcode toolchain) | Apple SDK/toolchain terms | host C/C++ build, dylib link, Mach-O object compile | `clang --version` |
| CMake | 4.4.4 | BSD-3-Clause | build system | `cmake --version` |
| Ninja | 1.13.2 | Apache-2.0 | build generator | `ninja --version` |
| MSVC | Visual Studio 18.11.3, MSVC tools 14.51.36231 | Microsoft toolchain terms | Windows reference fixture and reference executable only | Windows build log |
| QEMU / FEX / Box64 | see `ROUTE_B.md` pinned commits | GPL-2.0 / MIT / MIT | **evaluation only, not linked** | `ROUTE_B.md` |

No third-party binary is vendored in this directory.  The test fixture
`testdata/guest_x64.obj` is Aevotrace-authored source compiled with MSVC and is
kept to pin the exact machine-code bytes consumed by the translator.

The current project's non-commercial license does not relicense any third-party
component.  Any product integration must preserve the notices above and resolve
GPL/LLVM/Apple toolchain obligations separately; the Route B GPL note is a
risk record, not legal advice.
