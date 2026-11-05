"""Minimal PE32+ loader and x86-64 execution bridge for P0-UI-01.

IMPORTANT SCOPE NOTE
--------------------
This is a P0 spike execution bridge built on Unicorn. It is NOT the product
AOT/JIT core, and it does not attempt to be a general Windows loader. It maps
the fixture DLL at its preferred base, binds its imports to host stubs, and
executes the guest machine code. Every business decision remains in the guest
DLL; the host only implements the declared Win32 API subset plus GUI adapters.

Known P0 limitations are recorded in the report:
  * no TLS, SEH, APC, real Windows heap/loader, or delay imports
  * imports are stubs dispatched to Python; no general DLL loading
  * executable memory comes from host buffers (no MAP_JIT/signing work)
"""
from __future__ import annotations

import ctypes
import struct
import time

import pefile
import unicorn
from unicorn import (
    Uc,
    UcError,
    UC_ARCH_X86,
    UC_MODE_64,
    UC_HOOK_CODE,
    UC_HOOK_MEM_UNMAPPED,
    UC_PROT_ALL,
)
from unicorn.x86_const import (
    UC_X86_REG_RAX,
    UC_X86_REG_RBX,
    UC_X86_REG_RBP,
    UC_X86_REG_RCX,
    UC_X86_REG_RDI,
    UC_X86_REG_RDX,
    UC_X86_REG_RIP,
    UC_X86_REG_RSI,
    UC_X86_REG_RSP,
    UC_X86_REG_R8,
    UC_X86_REG_R9,
    UC_X86_REG_R10,
    UC_X86_REG_R11,
    UC_X86_REG_R12,
    UC_X86_REG_R13,
    UC_X86_REG_R14,
    UC_X86_REG_R15,
)

from .errors import GuestExitThread, GuestFault, UnsupportedApi

__version__ = unicorn.__version__

STUB_BASE = 0x60000000
STUB_SIZE = 0x10000
STUB_STRIDE = 0x10

TEMP_BASE = 0x61000000
TEMP_SIZE = 0x10000

MAIN_STACK_BASE = 0x62000000
MAIN_STACK_SIZE = 0x20000
MAIN_STACK_TOP = MAIN_STACK_BASE + 0x1F000

WORKER_STACK_BASE = 0x62100000
WORKER_STACK_SIZE = 0x20000
WORKER_STACK_TOP = WORKER_STACK_BASE + 0x1F000

SENTINEL_BASE = 0x63000000
SENTINEL_SIZE = 0x1000
SENTINEL = SENTINEL_BASE
SENTINEL_POINTER = SENTINEL_BASE + 0x800


class ImportSymbol:
    __slots__ = ("dll", "name", "iat_va", "stub_va")

    def __init__(self, dll: str, name: str, iat_va: int, stub_va: int):
        self.dll = dll
        self.name = name
        self.iat_va = iat_va
        self.stub_va = stub_va

    def __repr__(self) -> str:
        return f"{self.dll}!{self.name}@{self.stub_va:#x}"


def _align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


class PEImage:
    """A PE fixture mapped into host-owned buffers shared by guest threads."""

    def __init__(self, path: str, logger=None):
        self.path = path
        self.logger = logger
        self.pe = pefile.PE(path, fast_load=False)
        opt = self.pe.OPTIONAL_HEADER
        self.base = int(opt.ImageBase)
        self.size = _align(int(opt.SizeOfImage), 0x1000)
        self.entry_va = self.base + int(opt.AddressOfEntryPoint)
        self.image_buf = ctypes.create_string_buffer(self.size)
        ctypes.memset(self.image_buf, 0, self.size)

        raw = open(path, "rb").read()
        headers_size = int(opt.SizeOfHeaders)
        ctypes.memmove(self.image_buf, raw[:headers_size], min(headers_size, len(raw)))
        for section in self.pe.sections:
            va = int(section.VirtualAddress)
            raw_size = int(section.SizeOfRawData)
            virtual_size = int(section.Misc_VirtualSize)
            data = section.get_data()
            if va + max(raw_size, virtual_size) > self.size:
                raise GuestFault(f"section {section.Name!r} exceeds SizeOfImage")
            if data:
                ctypes.memmove(
                    ctypes.addressof(self.image_buf) + va,
                    data[:raw_size],
                    len(data[:raw_size]),
                )

        self.stub_buf = ctypes.create_string_buffer(STUB_SIZE)
        ctypes.memset(self.stub_buf, 0xCC, STUB_SIZE)
        self.temp_buf = ctypes.create_string_buffer(TEMP_SIZE)
        self.main_stack_buf = ctypes.create_string_buffer(MAIN_STACK_SIZE)
        self.worker_stack_buf = ctypes.create_string_buffer(WORKER_STACK_SIZE)
        self.sentinel_buf = ctypes.create_string_buffer(SENTINEL_SIZE)
        ctypes.memset(self.sentinel_buf, 0xCC, SENTINEL_SIZE)

        self.imports: list[ImportSymbol] = []
        self.stub_map: dict[int, ImportSymbol] = {}
        self._bind_imports()

    def _write_u64(self, va: int, value: int) -> None:
        off = va - self.base
        if off < 0 or off + 8 > self.size:
            raise GuestFault(f"IAT write outside image: {va:#x}")
        struct.pack_into("<Q", self.image_buf, off, value)

    def _bind_imports(self) -> None:
        index = 0
        for entry in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []):
            dll = entry.dll.decode("ascii", errors="replace")
            for imp in entry.imports:
                if imp.name:
                    name = imp.name.decode("ascii", errors="replace")
                else:
                    name = f"ordinal_{imp.ordinal}"
                stub_va = STUB_BASE + index * STUB_STRIDE
                if stub_va + STUB_STRIDE > STUB_BASE + STUB_SIZE:
                    raise GuestFault("import stub page exhausted")
                symbol = ImportSymbol(dll, name, int(imp.address), stub_va)
                self.imports.append(symbol)
                self.stub_map[stub_va] = symbol
                self._write_u64(symbol.iat_va, stub_va)
                index += 1

    def export(self, name: str) -> int:
        for exp in getattr(self.pe, "DIRECTORY_ENTRY_EXPORT", []).symbols:
            if exp.name and exp.name.decode("ascii") == name:
                return self.base + int(exp.address)
        raise GuestFault(f"guest export not found: {name}")

    def image_bytes(self) -> bytes:
        return self.image_buf.raw


class GuestMachine:
    """One guest thread: a Unicorn instance over the shared PE image."""

    def __init__(self, image: PEImage, host, logger, thread_name: str, thread_id: int):
        self.image = image
        self.host = host
        self.logger = logger
        self.thread_name = thread_name
        self.thread_id = thread_id
        self.instruction_count = 0
        self.api_counts: dict[str, int] = {}
        self.in_emulation = False
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        self._map(image.base, image.size, image.image_buf)
        self._map(STUB_BASE, STUB_SIZE, image.stub_buf)
        self._map(TEMP_BASE, TEMP_SIZE, image.temp_buf)
        self._map(SENTINEL_BASE, SENTINEL_SIZE, image.sentinel_buf)
        if thread_name == "main":
            self.stack_base = MAIN_STACK_BASE
            self.stack_top = MAIN_STACK_TOP
            self._map(MAIN_STACK_BASE, MAIN_STACK_SIZE, image.main_stack_buf)
        else:
            self.stack_base = WORKER_STACK_BASE
            self.stack_top = WORKER_STACK_TOP
            self._map(WORKER_STACK_BASE, WORKER_STACK_SIZE, image.worker_stack_buf)
        self.uc.hook_add(UC_HOOK_CODE, self._count_instruction,
                         begin=image.base, end=image.base + image.size)
        self.uc.hook_add(UC_HOOK_CODE, self._stub_hook,
                         begin=STUB_BASE, end=STUB_BASE + STUB_SIZE)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._unmapped_hook)

    def _map(self, address: int, size: int, buf) -> None:
        self.uc.mem_map_ptr(address, size, UC_PROT_ALL, ctypes.addressof(buf))

    def _count_instruction(self, uc, address, size, user_data):
        self.instruction_count += 1
        return True

    def _unmapped_hook(self, uc, access, address, size, value, user_data):
        self.logger.event(
            "guest.mem_unmapped",
            thread=self.thread_name,
            access=int(access),
            address=f"0x{address:x}",
            size=size,
            value=f"0x{value:x}",
            rip=f"0x{uc.reg_read(UC_X86_REG_RIP):x}",
        )
        return False

    # ------------------------------------------------------------------
    # Memory helpers
    # ------------------------------------------------------------------
    def read_bytes(self, va: int, size: int) -> bytes:
        if va == 0 or size <= 0:
            return b""
        return bytes(self.uc.mem_read(va, size))

    def write_bytes(self, va: int, data: bytes) -> None:
        if va == 0:
            raise UnsupportedApi("guest passed a NULL output buffer")
        self.uc.mem_write(va, data)

    def read_u64(self, va: int) -> int:
        return struct.unpack("<Q", self.read_bytes(va, 8))[0]

    def write_u64(self, va: int, value: int) -> None:
        self.write_bytes(va, struct.pack("<Q", value & 0xFFFFFFFFFFFFFFFF))

    def read_wstring(self, va: int, max_units: int = 4096) -> str:
        if va == 0:
            return ""
        out = bytearray()
        for i in range(max_units):
            pair = self.uc.mem_read(va + i * 2, 2)
            if pair[0] == 0 and pair[1] == 0:
                break
            out.extend(pair)
        return out.decode("utf-16-le", errors="replace")

    def write_wstring(self, va: int, text: str, max_units: int | None = None) -> int:
        if va == 0:
            raise UnsupportedApi("guest passed a NULL string pointer")
        data = text.encode("utf-16-le") + b"\x00\x00"
        if max_units is not None:
            limit = max_units * 2
            if len(data) > limit:
                data = data[: max(0, limit - 2)] + b"\x00\x00"
        self.uc.mem_write(va, data)
        return (len(data) // 2) - 1

    def read_guest_state(self) -> dict:
        """Call the guest P0GetState export and parse its struct."""
        from .guest_abi import P0_STATE_SIZE, parse_state
        out_va = TEMP_BASE
        self.write_bytes(out_va, b"\x00" * P0_STATE_SIZE)
        self.call(self.image.export("P0GetState"), [out_va])
        return parse_state(self.read_bytes(out_va, P0_STATE_SIZE))

    # ------------------------------------------------------------------
    # Calls into guest code
    # ------------------------------------------------------------------
    def call(self, address: int, args: list[int], timeout_us: int = 15_000_000,
             purpose: str = "") -> int:
        if self.in_emulation:
            # Calling guest code from inside a guest import hook would need
            # nested uc_emu_start, which is not safe. The spike detects and
            # reports this instead of pretending it works.
            raise UnsupportedApi(
                f"nested guest call requested while executing guest code: {purpose or hex(address)}"
            )
        if address == 0:
            raise GuestFault(f"call to NULL guest address ({purpose})")
        sp = self.stack_top - 8
        self.write_u64(sp, SENTINEL)
        self.uc.reg_write(UC_X86_REG_RSP, sp)
        self.uc.reg_write(UC_X86_REG_RIP, address)
        for reg in (UC_X86_REG_RBX, UC_X86_REG_RBP, UC_X86_REG_RSI, UC_X86_REG_RDI,
                    UC_X86_REG_R10, UC_X86_REG_R11, UC_X86_REG_R12, UC_X86_REG_R13,
                    UC_X86_REG_R14, UC_X86_REG_R15):
            self.uc.reg_write(reg, 0)
        regs = [UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9]
        for i, reg in enumerate(regs):
            self.uc.reg_write(reg, args[i] if i < len(args) else 0)
        before = self.instruction_count
        self.in_emulation = True
        started = time.monotonic()
        try:
            self.uc.emu_start(address, SENTINEL, timeout=timeout_us)
        except GuestExitThread:
            raise
        except UcError as exc:
            rip = self.uc.reg_read(UC_X86_REG_RIP)
            self.logger.event(
                "guest.fault",
                thread=self.thread_name,
                purpose=purpose,
                error=str(exc),
                rip=f"0x{rip:x}",
                rsp=f"0x{self.uc.reg_read(UC_X86_REG_RSP):x}",
            )
            raise GuestFault(f"guest fault in {purpose}: {exc}") from exc
        finally:
            self.in_emulation = False
        elapsed = time.monotonic() - started
        rip = self.uc.reg_read(UC_X86_REG_RIP)
        if rip != SENTINEL:
            raise GuestFault(f"guest stopped at 0x{rip:x}, expected sentinel after {purpose}")
        ret = self.uc.reg_read(UC_X86_REG_RAX)
        self.logger.event(
            "guest.return",
            thread=self.thread_name,
            purpose=purpose or f"0x{address:x}",
            address=f"0x{address:x}",
            ret=f"0x{ret & 0xFFFFFFFFFFFFFFFF:x}",
            instructions=self.instruction_count - before,
            elapsed_ms=round(elapsed * 1000.0, 3),
        )
        return ret

    def run_thread(self, start: int, param: int, timeout_us: int = 15_000_000) -> None:
        """Run a guest thread entry until it calls ExitThread or returns."""
        try:
            self.call(start, [param], timeout_us=timeout_us, purpose="thread_entry")
        except GuestExitThread:
            self.logger.event("guest.thread_exit", thread=self.thread_name,
                              instructions=self.instruction_count)

    def _read_args(self, count: int = 12) -> list[int]:
        regs = [UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9]
        args = [int(self.uc.reg_read(r)) for r in regs]
        sp = self.uc.reg_read(UC_X86_REG_RSP)
        for i in range(4, count):
            args.append(self.read_u64(sp + 0x28 + (i - 4) * 8))
        return args

    def _stub_hook(self, uc, address, size, user_data):
        symbol = self.image.stub_map.get(address)
        if symbol is None:
            raise GuestFault(f"unknown import stub at 0x{address:x}")
        self.api_counts[symbol.name] = self.api_counts.get(symbol.name, 0) + 1
        args = self._read_args()
        ret = self.host.dispatch_api(self, symbol, args)
        return_address = self.read_u64(self.uc.reg_read(UC_X86_REG_RSP))
        self.uc.reg_write(UC_X86_REG_RSP, self.uc.reg_read(UC_X86_REG_RSP) + 8)
        self.uc.reg_write(UC_X86_REG_RAX, ret & 0xFFFFFFFFFFFFFFFF)
        self.uc.reg_write(UC_X86_REG_RIP, return_address)
        return True
