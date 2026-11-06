"""ctypes wrapper for the P0 callback bridge.

This module intentionally imports no Unicorn/pefile/guest_pe.  The engine GUI
path loads libaev_bridge.dylib, the same bridge used by the no-GUI tests, and
executes the fixed COFF fixture through the shared AOT/JIT core.
"""
from __future__ import annotations

import ctypes
import os
from dataclasses import dataclass
from typing import Callable, Optional

AEV_CALLBACK_ABI_VERSION = 2
AEV_ARTIFACT_ABI_VERSION = 1
AEV_TRANSLATOR_ABI_VERSION = 1
AEV_ARTIFACT_INFO_ABI_VERSION = 1
AEV_GUEST_SENTINEL = 0xFFFFFFFFFFFFFF00
AEV_BRIDGE_IMPORT_BASE = 0x140010000
AEV_BRIDGE_IMPORT_STRIDE = 0x10
AEV_BRIDGE_IMPORT_LIMIT = 0x140020000
AEV_BRIDGE_STEP_LIMIT = 1000000

AEV_BRIDGE_OK = 0
AEV_BRIDGE_ERR_INVALID_ARGUMENT = 1
AEV_BRIDGE_ERR_ABI_MISMATCH = 2
AEV_BRIDGE_ERR_LOAD = 3
AEV_BRIDGE_ERR_NO_BLOCK = 4
AEV_BRIDGE_ERR_UNKNOWN_IMPORT = 5
AEV_BRIDGE_ERR_MEMORY = 6
AEV_BRIDGE_ERR_JIT = 7
AEV_BRIDGE_ERR_STEP_LIMIT = 8
AEV_BRIDGE_ERR_REENTRY = 9
AEV_BRIDGE_ERR_WRONG_THREAD = 10
AEV_BRIDGE_ERR_HOST = 11
AEV_BRIDGE_ERR_INTERNAL = 12

STATUS_NAMES = {
    0: "OK",
    1: "INVALID_ARGUMENT",
    2: "ABI_MISMATCH",
    3: "LOAD",
    4: "NO_BLOCK",
    5: "UNKNOWN_IMPORT",
    6: "MEMORY",
    7: "JIT",
    8: "STEP_LIMIT",
    9: "REENTRY",
    10: "WRONG_THREAD",
    11: "HOST",
    12: "INTERNAL",
}


class BridgeError(RuntimeError):
    def __init__(self, status: int, message: str, result: Optional["CallResult"] = None):
        self.status = int(status)
        self.status_name = STATUS_NAMES.get(self.status, f"UNKNOWN({self.status})")
        self.result = result
        super().__init__(f"{self.status_name}: {message}")


class BridgeConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("coff_path", ctypes.c_char_p),
        ("function_name", ctypes.c_char_p),
        ("entry_addr", ctypes.c_uint64),
        ("aot_dylib_path", ctypes.c_char_p),
        ("guest_mem_size", ctypes.c_uint64),
        ("guest_stack_size", ctypes.c_uint64),
        ("trace", ctypes.c_uint32),
        ("dump_jit", ctypes.c_uint32),
        ("trace_aot", ctypes.c_uint32),
        ("require_main_thread", ctypes.c_uint32),
        ("reserved0", ctypes.c_uint32),
    ]


class CallResult(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("status", ctypes.c_uint32),
        ("guest_error", ctypes.c_uint32),
        ("guest_rax", ctypes.c_uint64),
        ("guest_rip", ctypes.c_uint64),
        ("guest_rsp", ctypes.c_uint64),
        ("error_pc", ctypes.c_uint64),
        ("steps", ctypes.c_uint64),
        ("aot_blocks", ctypes.c_uint64),
        ("jit_blocks", ctypes.c_uint64),
        ("jit_compiles", ctypes.c_uint64),
        ("jit_execs", ctypes.c_uint64),
        ("host_import_id", ctypes.c_uint64),
        ("call_index", ctypes.c_uint64),
        ("guest_mem_base", ctypes.c_uint64),
        ("guest_mem_size", ctypes.c_uint64),
        ("total_calls", ctypes.c_uint64),
        ("total_aot_blocks", ctypes.c_uint64),
        ("total_jit_blocks", ctypes.c_uint64),
        ("total_jit_compiles", ctypes.c_uint64),
        ("total_jit_execs", ctypes.c_uint64),
    ]


class ArtifactInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("aot_table_count", ctypes.c_uint32),
        ("all_block_count", ctypes.c_uint32),
        ("artifact_abi_version", ctypes.c_uint32),
        ("translator_abi_version", ctypes.c_uint32),
        ("entry_addr", ctypes.c_uint64),
        ("input_sha256", ctypes.c_uint8 * 32),
        ("aot_sha256", ctypes.c_uint8 * 32),
        ("target_triple", ctypes.c_char * 48),
    ]


HostImportFn = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_uint64,
    ctypes.c_uint64,
    ctypes.c_uint64,
    ctypes.c_uint64,
    ctypes.POINTER(ctypes.c_uint64),
)


@dataclass
class GuestInfo:
    base: int
    size: int
    stack_base: int
    stack_top: int


class EngineBridge:
    """One bridge session; create/destroy on the AppKit main thread."""

    def __init__(self, library_path: str):
        self.library_path = os.path.abspath(library_path)
        self.lib = ctypes.CDLL(self.library_path)
        self._handle = ctypes.c_void_p()
        self._callbacks: list[HostImportFn] = []
        self.callback_error: Optional[BaseException] = None
        self._configure_prototypes()
        abi = self.abi_info()
        if abi["abi_version"] != AEV_CALLBACK_ABI_VERSION:
            raise BridgeError(
                AEV_BRIDGE_ERR_ABI_MISMATCH,
                f"bridge ABI {abi['abi_version']} != {AEV_CALLBACK_ABI_VERSION}",
            )
        if abi["config_size"] != ctypes.sizeof(BridgeConfig):
            raise BridgeError(
                AEV_BRIDGE_ERR_ABI_MISMATCH,
                f"config size {abi['config_size']} != {ctypes.sizeof(BridgeConfig)}",
            )
        if abi["call_result_size"] != ctypes.sizeof(CallResult):
            raise BridgeError(
                AEV_BRIDGE_ERR_ABI_MISMATCH,
                f"result size {abi['call_result_size']} != {ctypes.sizeof(CallResult)}",
            )

    def _configure_prototypes(self) -> None:
        self.lib.aev_bridge_abi_info.argtypes = [
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self.lib.aev_bridge_abi_info.restype = ctypes.c_int
        self.lib.aev_bridge_create.argtypes = [
            ctypes.POINTER(BridgeConfig),
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self.lib.aev_bridge_create.restype = ctypes.c_int
        self.lib.aev_bridge_destroy.argtypes = [ctypes.c_void_p]
        self.lib.aev_bridge_destroy.restype = ctypes.c_int
        self.lib.aev_bridge_guest_info.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.POINTER(ctypes.c_uint64),
        ]
        self.lib.aev_bridge_guest_info.restype = ctypes.c_int
        self.lib.aev_bridge_artifact_info.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ArtifactInfo),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self.lib.aev_bridge_artifact_info.restype = ctypes.c_int
        self.lib.aev_bridge_selected_block_contains.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint64,
        ]
        self.lib.aev_bridge_selected_block_contains.restype = ctypes.c_int
        self.lib.aev_bridge_selected_block_count.argtypes = [ctypes.c_void_p]
        self.lib.aev_bridge_selected_block_count.restype = ctypes.c_uint32
        self.lib.aev_bridge_selected_block_rip.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint64),
        ]
        self.lib.aev_bridge_selected_block_rip.restype = ctypes.c_int
        self.lib.aev_bridge_debug_reserved0.argtypes = [ctypes.c_void_p]
        self.lib.aev_bridge_debug_reserved0.restype = ctypes.c_uint64
        self.lib.aev_bridge_trace_open.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self.lib.aev_bridge_trace_open.restype = ctypes.c_int
        self.lib.aev_bridge_trace_close.argtypes = [ctypes.c_void_p]
        self.lib.aev_bridge_trace_close.restype = ctypes.c_int
        self.lib.aev_bridge_register_import.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_uint64,
            HostImportFn,
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self.lib.aev_bridge_register_import.restype = ctypes.c_int
        self.lib.aev_bridge_guest_write.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self.lib.aev_bridge_guest_write.restype = ctypes.c_int
        self.lib.aev_bridge_guest_read.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self.lib.aev_bridge_guest_read.restype = ctypes.c_int
        self.lib.aev_bridge_call.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.POINTER(CallResult),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self.lib.aev_bridge_call.restype = ctypes.c_int
        self.lib.aev_bridge_set_test_fault.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint64,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self.lib.aev_bridge_set_test_fault.restype = ctypes.c_int

    @staticmethod
    def _errbuf() -> ctypes.Array:
        return ctypes.create_string_buffer(512)

    @staticmethod
    def _errmsg(buf) -> str:
        raw = bytes(buf.value)
        return raw.decode("utf-8", "replace")

    def abi_info(self) -> dict:
        abi = ctypes.c_uint32()
        guest = ctypes.c_uint32()
        result = ctypes.c_uint32()
        config = ctypes.c_uint32()
        rc = self.lib.aev_bridge_abi_info(
            ctypes.byref(abi), ctypes.byref(guest), ctypes.byref(result),
            ctypes.byref(config),
        )
        if rc != AEV_BRIDGE_OK:
            raise BridgeError(rc, "aev_bridge_abi_info failed")
        return {
            "abi_version": abi.value,
            "guest_state_size": guest.value,
            "call_result_size": result.value,
            "config_size": config.value,
        }

    def create(
        self,
        coff_path: str,
        aot_dylib_path: str,
        entry_addr: int,
        guest_mem_size: int = 4 * 1024 * 1024,
        guest_stack_size: int = 64 * 1024,
        trace_aot: bool = False,
        dump_jit: bool = True,
        require_main_thread: bool = True,
        function_name: str = "aev_guest_callback",
    ) -> None:
        if self._handle:
            raise RuntimeError("bridge already created")
        cfg = BridgeConfig()
        cfg.struct_size = ctypes.sizeof(BridgeConfig)
        cfg.abi_version = AEV_CALLBACK_ABI_VERSION
        cfg.coff_path = str(coff_path).encode()
        cfg.function_name = function_name.encode()
        cfg.entry_addr = int(entry_addr)
        cfg.aot_dylib_path = str(aot_dylib_path).encode()
        cfg.guest_mem_size = int(guest_mem_size)
        cfg.guest_stack_size = int(guest_stack_size)
        cfg.trace = 0
        cfg.dump_jit = 1 if dump_jit else 0
        cfg.trace_aot = 1 if trace_aot else 0
        cfg.require_main_thread = 1 if require_main_thread else 0
        cfg.reserved0 = 0
        handle = ctypes.c_void_p()
        err = self._errbuf()
        rc = self.lib.aev_bridge_create(
            ctypes.byref(cfg), ctypes.byref(handle), err, ctypes.sizeof(err)
        )
        if rc != AEV_BRIDGE_OK:
            raise BridgeError(rc, self._errmsg(err) or "bridge create failed")
        self._handle = handle

    def guest_info(self) -> GuestInfo:
        base = ctypes.c_uint64()
        size = ctypes.c_uint64()
        stack_base = ctypes.c_uint64()
        stack_top = ctypes.c_uint64()
        rc = self.lib.aev_bridge_guest_info(
            self._handle, ctypes.byref(base), ctypes.byref(size),
            ctypes.byref(stack_base), ctypes.byref(stack_top),
        )
        if rc != AEV_BRIDGE_OK:
            raise BridgeError(rc, "guest_info failed")
        return GuestInfo(base.value, size.value, stack_base.value, stack_top.value)

    def artifact_info(self) -> dict:
        info = ArtifactInfo()
        info.struct_size = ctypes.sizeof(ArtifactInfo)
        info.abi_version = AEV_ARTIFACT_INFO_ABI_VERSION
        err = self._errbuf()
        rc = self.lib.aev_bridge_artifact_info(
            self._handle, ctypes.byref(info), err, ctypes.sizeof(err)
        )
        if rc != AEV_BRIDGE_OK:
            raise BridgeError(rc, self._errmsg(err) or "artifact_info failed")
        return {
            "aot_table_count": info.aot_table_count,
            "all_block_count": info.all_block_count,
            "artifact_abi_version": info.artifact_abi_version,
            "translator_abi_version": info.translator_abi_version,
            "entry_addr": info.entry_addr,
            "input_sha256": bytes(info.input_sha256).hex(),
            "aot_sha256": bytes(info.aot_sha256).hex(),
            "target_triple": bytes(info.target_triple).split(b"\0", 1)[0].decode(),
        }

    def selected_block_contains(self, rip: int) -> bool:
        rc = self.lib.aev_bridge_selected_block_contains(self._handle, rip)
        if rc < 0:
            raise BridgeError(rc, "selected_block_contains failed")
        return rc == 1

    def selected_block_count(self) -> int:
        return int(self.lib.aev_bridge_selected_block_count(self._handle))

    def selected_block_rip(self, index: int) -> int:
        rip = ctypes.c_uint64()
        rc = self.lib.aev_bridge_selected_block_rip(
            self._handle, index, ctypes.byref(rip)
        )
        if rc != 0:
            raise BridgeError(rc, "selected_block_rip failed")
        return int(rip.value)

    def selected_blocks(self) -> list[int]:
        return [self.selected_block_rip(i) for i in range(self.selected_block_count())]

    def debug_reserved0(self) -> int:
        return int(self.lib.aev_bridge_debug_reserved0(self._handle))

    def trace_open(self, path: str, scenario: str) -> None:
        err = self._errbuf()
        rc = self.lib.aev_bridge_trace_open(
            self._handle, str(path).encode(), str(scenario).encode(),
            err, ctypes.sizeof(err)
        )
        if rc != AEV_BRIDGE_OK:
            raise BridgeError(rc, self._errmsg(err) or "trace_open failed")

    def trace_close(self) -> None:
        rc = self.lib.aev_bridge_trace_close(self._handle)
        if rc != AEV_BRIDGE_OK:
            raise BridgeError(rc, "trace_close failed")

    def register_import(
        self, guest_addr: int, import_id: int, fn: Callable[[int, int, int, int], int]
    ) -> None:
        def _trampoline(_ctx, _bridge, a0, a1, a2, a3, out_rax):
            try:
                value = int(fn(int(a0), int(a1), int(a2), int(a3)))
            except BaseException as exc:  # noqa: BLE001 - must not cross C
                self.callback_error = exc
                return AEV_BRIDGE_ERR_HOST
            if not out_rax:
                return AEV_BRIDGE_ERR_INVALID_ARGUMENT
            out_rax[0] = value & 0xFFFFFFFFFFFFFFFF
            return AEV_BRIDGE_OK

        cb = HostImportFn(_trampoline)
        self._callbacks.append(cb)
        err = self._errbuf()
        rc = self.lib.aev_bridge_register_import(
            self._handle, int(guest_addr), int(import_id), cb, None,
            err, ctypes.sizeof(err),
        )
        if rc != AEV_BRIDGE_OK:
            raise BridgeError(rc, self._errmsg(err) or "register_import failed")

    def guest_write(self, guest_addr: int, data: bytes) -> None:
        if not data:
            return
        buf = ctypes.create_string_buffer(data, len(data))
        err = self._errbuf()
        rc = self.lib.aev_bridge_guest_write(
            self._handle, int(guest_addr), ctypes.cast(buf, ctypes.c_void_p),
            len(data), err, ctypes.sizeof(err),
        )
        if rc != AEV_BRIDGE_OK:
            raise BridgeError(rc, self._errmsg(err) or "guest_write failed")

    def guest_read(self, guest_addr: int, size: int) -> bytes:
        if size <= 0:
            return b""
        buf = ctypes.create_string_buffer(size)
        err = self._errbuf()
        rc = self.lib.aev_bridge_guest_read(
            self._handle, int(guest_addr), ctypes.cast(buf, ctypes.c_void_p),
            size, err, ctypes.sizeof(err),
        )
        if rc != AEV_BRIDGE_OK:
            raise BridgeError(rc, self._errmsg(err) or "guest_read failed")
        return bytes(buf.raw)

    def call(
        self, entry_addr: int, args: list[int], raise_on_error: bool = True
    ) -> CallResult:
        if len(args) > 4:
            raise ValueError("at most four guest arguments are supported")
        padded = [int(x) & 0xFFFFFFFFFFFFFFFF for x in args] + [0] * (4 - len(args))
        result = CallResult()
        result.struct_size = ctypes.sizeof(CallResult)
        result.abi_version = AEV_CALLBACK_ABI_VERSION
        err = self._errbuf()
        rc = self.lib.aev_bridge_call(
            self._handle, int(entry_addr), *padded, ctypes.byref(result),
            err, ctypes.sizeof(err),
        )
        if self.callback_error is not None:
            exc = self.callback_error
            self.callback_error = None
            result.status = AEV_BRIDGE_ERR_HOST
            raise BridgeError(AEV_BRIDGE_ERR_HOST, f"host callback raised: {exc}", result)
        if rc != AEV_BRIDGE_OK:
            # The C API may return before filling AevCallResult (reentry,
            # wrong thread, invalid struct), so keep the status observable.
            result.status = rc
            if raise_on_error:
                raise BridgeError(rc, self._errmsg(err) or "guest call failed", result)
        return result

    def set_test_fault(self, mode: int, rip: int = 0) -> None:
        err = self._errbuf()
        rc = self.lib.aev_bridge_set_test_fault(
            self._handle, int(mode), int(rip), err, ctypes.sizeof(err)
        )
        if rc != AEV_BRIDGE_OK:
            raise BridgeError(rc, self._errmsg(err) or "set_test_fault failed")

    def destroy(self) -> None:
        if not self._handle:
            return
        rc = self.lib.aev_bridge_destroy(self._handle)
        if rc == AEV_BRIDGE_OK:
            self._handle = ctypes.c_void_p()
        else:
            raise BridgeError(rc, "bridge destroy failed")
