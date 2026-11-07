"""ctypes wrapper for the P0-PE-GUI-01 persistent PE session C ABI.

The wrapper mirrors ``spikes/p0-engine/include/aev_pe_session.h`` (ABI 1)
exactly.  The compat/native GUI paths call this module in-process; they never
spawn ``aev-pe-runner`` and never parse runner JSON.  This module intentionally
imports no Unicorn/pefile/guest_pe (or the COFF bridge): the PE execution
engine is the shared ``libaev_pe_session.dylib``.

Public surface used by the GUI harness:
    PeSession.create / info / counts / exports / selected / all / block /
    call / trace_open / trace_close / destroy

All output structures have ``struct_size``/``abi_version`` filled before the
call.  A non-OK status raises :class:`PeSessionError` with ``status`` and
``status_name``; ``call(..., raise_on_error=False)`` is available for callers
that need to inspect a failed result without an exception (the GUI negative
button uses the exception form and keeps its previous business display).
"""
from __future__ import annotations

import ctypes
import os
from typing import Iterable, Optional

AEV_PE_SESSION_ABI_VERSION = 1
AEV_PE_SESSION_MAX_ARGS = 4
AEV_PE_SESSION_MAX_NAME = 512

AEV_GUEST_ABI_VERSION = 2
AEV_GUEST_STATE_SIZE = 37 * 8
AEV_ARTIFACT_ABI_VERSION = 2
AEV_TRANSLATOR_ABI_VERSION = 2
AEV_CALLBACK_ABI_VERSION = 3

AEV_PE_SESSION_OK = 0
AEV_PE_SESSION_ERR_INVALID_ARGUMENT = 2
AEV_PE_SESSION_ERR_LOAD = 3
AEV_PE_SESSION_ERR_ABI_MISMATCH = 4
AEV_PE_SESSION_ERR_UNKNOWN_IMPORT = 5
AEV_PE_SESSION_ERR_MEMORY = 6
AEV_PE_SESSION_ERR_JIT = 7
AEV_PE_SESSION_ERR_STEP_LIMIT = 8
AEV_PE_SESSION_ERR_STALE_HANDLE = 9
AEV_PE_SESSION_ERR_REENTRY = 10
AEV_PE_SESSION_ERR_HOST = 11
AEV_PE_SESSION_ERR_INTERNAL = 12
AEV_PE_SESSION_ERR_WRONG_THREAD = 13
AEV_PE_SESSION_ERR_TRACE = 14

STATUS_NAMES = {
    AEV_PE_SESSION_OK: "OK",
    AEV_PE_SESSION_ERR_INVALID_ARGUMENT: "INVALID_ARGUMENT",
    AEV_PE_SESSION_ERR_LOAD: "LOAD",
    AEV_PE_SESSION_ERR_ABI_MISMATCH: "ABI_MISMATCH",
    AEV_PE_SESSION_ERR_UNKNOWN_IMPORT: "UNKNOWN_IMPORT",
    AEV_PE_SESSION_ERR_MEMORY: "MEMORY",
    AEV_PE_SESSION_ERR_JIT: "JIT",
    AEV_PE_SESSION_ERR_STEP_LIMIT: "STEP_LIMIT",
    AEV_PE_SESSION_ERR_STALE_HANDLE: "STALE_HANDLE",
    AEV_PE_SESSION_ERR_REENTRY: "REENTRY",
    AEV_PE_SESSION_ERR_HOST: "HOST",
    AEV_PE_SESSION_ERR_INTERNAL: "INTERNAL",
    AEV_PE_SESSION_ERR_WRONG_THREAD: "WRONG_THREAD",
    AEV_PE_SESSION_ERR_TRACE: "TRACE",
}


def status_name(status: int) -> str:
    return STATUS_NAMES.get(int(status), f"UNKNOWN({int(status)})")


class PeSessionError(RuntimeError):
    def __init__(self, status: int, message: str, result: Optional[dict] = None):
        self.status = int(status)
        self.status_name = status_name(self.status)
        self.result = result
        super().__init__(f"{self.status_name} ({self.status}): {message}")


class AevPeSessionConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("dll_path", ctypes.c_char_p),
        ("registry_path", ctypes.c_char_p),
        ("aot_path", ctypes.c_char_p),
        ("entry_export", ctypes.c_char_p),
        ("load_base", ctypes.c_uint64),
        ("trace_path", ctypes.c_char_p),
        ("scenario", ctypes.c_char_p),
        ("flags", ctypes.c_uint32),
        ("reserved0", ctypes.c_uint32),
        ("reserved1", ctypes.c_uint64),
    ]


class AevPeSessionAbiInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("config_size", ctypes.c_uint32),
        ("result_size", ctypes.c_uint32),
        ("info_size", ctypes.c_uint32),
        ("counts_size", ctypes.c_uint32),
        ("export_info_size", ctypes.c_uint32),
        ("block_info_size", ctypes.c_uint32),
        ("guest_abi_version", ctypes.c_uint64),
        ("guest_state_size", ctypes.c_uint64),
        ("artifact_abi_version", ctypes.c_uint64),
        ("translator_abi_version", ctypes.c_uint64),
        ("callback_abi_version", ctypes.c_uint64),
        ("max_args", ctypes.c_uint64),
    ]


class AevPeCallResult(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("status", ctypes.c_uint32),
        ("guest_error", ctypes.c_uint32),
        ("args", ctypes.c_uint64 * AEV_PE_SESSION_MAX_ARGS),
        ("rax", ctypes.c_uint64),
        ("rax_i32", ctypes.c_int32),
        ("reserved0", ctypes.c_uint32),
        ("error_pc", ctypes.c_uint64),
        ("mem_fault_kind", ctypes.c_uint64),
        ("mem_fault_addr", ctypes.c_uint64),
        ("mem_fault_size", ctypes.c_uint64),
        ("mem_access", ctypes.c_uint64),
        ("steps", ctypes.c_uint64),
        ("aot_blocks", ctypes.c_uint64),
        ("jit_blocks", ctypes.c_uint64),
        ("jit_compiles", ctypes.c_uint64),
        ("jit_execs", ctypes.c_uint64),
        ("call_index", ctypes.c_uint64),
        ("total_calls", ctypes.c_uint64),
        ("total_aot_blocks", ctypes.c_uint64),
        ("total_jit_blocks", ctypes.c_uint64),
        ("total_jit_compiles", ctypes.c_uint64),
        ("total_jit_execs", ctypes.c_uint64),
        ("guest_rip", ctypes.c_uint64),
        ("guest_rsp", ctypes.c_uint64),
    ]


class AevPeModuleInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("guest_abi_version", ctypes.c_uint32),
        ("artifact_abi_version", ctypes.c_uint32),
        ("translator_abi_version", ctypes.c_uint32),
        ("callback_abi_version", ctypes.c_uint32),
        ("guest_state_size", ctypes.c_uint64),
        ("file_size", ctypes.c_uint64),
        ("preferred_base", ctypes.c_uint64),
        ("load_base", ctypes.c_uint64),
        ("image_size", ctypes.c_uint64),
        ("thunk_base", ctypes.c_uint64),
        ("import_count", ctypes.c_uint64),
        ("relocs_stripped", ctypes.c_uint64),
        ("entry_va", ctypes.c_uint64),
        ("stack_base", ctypes.c_uint64),
        ("stack_top", ctypes.c_uint64),
        ("entry_rsp", ctypes.c_uint64),
        ("region_count", ctypes.c_uint64),
        ("all_block_count", ctypes.c_uint64),
        ("selected_count", ctypes.c_uint64),
        ("aot_present", ctypes.c_uint64),
        ("load_count", ctypes.c_uint64),
        ("file_sha256", ctypes.c_uint8 * 32),
        ("image_sha256", ctypes.c_uint8 * 32),
        ("import_binding_sha256", ctypes.c_uint8 * 32),
        ("host_binding_sha256", ctypes.c_uint8 * 32),
        ("entry_exports_sha256", ctypes.c_uint8 * 32),
        ("aot_sha256", ctypes.c_uint8 * 32),
        ("module_kind", ctypes.c_char * 24),
        ("entry_export", ctypes.c_char * AEV_PE_SESSION_MAX_NAME),
        ("target_triple", ctypes.c_char * 48),
    ]


class AevPeSessionCounts(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("load_count", ctypes.c_uint64),
        ("calls_total", ctypes.c_uint64),
        ("host_import_calls", ctypes.c_uint64),
        ("aot_blocks_total", ctypes.c_uint64),
        ("jit_blocks_total", ctypes.c_uint64),
        ("jit_compiles_total", ctypes.c_uint64),
        ("jit_execs_total", ctypes.c_uint64),
        ("trace_records", ctypes.c_uint64),
        ("trace_open", ctypes.c_uint64),
    ]


class AevPeExportInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("rva", ctypes.c_uint32),
        ("ordinal", ctypes.c_uint32),
        ("va", ctypes.c_uint64),
        ("name", ctypes.c_char * AEV_PE_SESSION_MAX_NAME),
    ]


class AevPeBlockInfo(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("rip", ctypes.c_uint64),
        ("rva", ctypes.c_uint32),
        ("size", ctypes.c_uint32),
        ("sha256", ctypes.c_uint8 * 32),
        ("aot_selected", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


OnCallEnd = ctypes.CFUNCTYPE(
    None, ctypes.c_void_p, ctypes.POINTER(AevPeCallResult)
)
OnHostImport = ctypes.CFUNCTYPE(
    None, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint32
)


class AevPeSessionObserver(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("on_call_end", OnCallEnd),
        ("on_host_import", OnHostImport),
    ]


EXPECTED_STRUCT_SIZES = {
    "config": 80,
    "abi_info": 80,
    "call_result": 208,
    "module_info": 936,
    "counts": 80,
    "export_info": 536,
    "block_info": 64,
    "observer": 24,
}


def layout_self_check() -> dict:
    """Return the ctypes layout for offline verification against the header."""
    sizes = {
        "config": ctypes.sizeof(AevPeSessionConfig),
        "abi_info": ctypes.sizeof(AevPeSessionAbiInfo),
        "call_result": ctypes.sizeof(AevPeCallResult),
        "module_info": ctypes.sizeof(AevPeModuleInfo),
        "counts": ctypes.sizeof(AevPeSessionCounts),
        "export_info": ctypes.sizeof(AevPeExportInfo),
        "block_info": ctypes.sizeof(AevPeBlockInfo),
        "observer": ctypes.sizeof(AevPeSessionObserver),
    }
    differences = {name: (sizes[name], expected)
                   for name, expected in EXPECTED_STRUCT_SIZES.items()
                   if sizes[name] != expected}
    return {"sizes": sizes, "expected": dict(EXPECTED_STRUCT_SIZES),
            "differences": differences}


def _check_layout() -> None:
    report = layout_self_check()
    if report["differences"]:
        raise RuntimeError(f"PE session ctypes layout mismatch: {report['differences']}")


_check_layout()


def _as_hex(value: bytes) -> str:
    return bytes(value).hex()


def _cstr(value) -> str:
    return bytes(value).split(b"\0", 1)[0].decode("utf-8", "replace")


def _call_result_dict(result: AevPeCallResult) -> dict:
    return {
        "struct_size": int(result.struct_size),
        "abi_version": int(result.abi_version),
        "status": int(result.status),
        "status_name": status_name(int(result.status)),
        "guest_error": int(result.guest_error),
        "args": [int(x) for x in result.args],
        "rax": int(result.rax),
        "rax_i32": int(result.rax_i32),
        "error_pc": int(result.error_pc),
        "mem_fault_kind": int(result.mem_fault_kind),
        "mem_fault_addr": int(result.mem_fault_addr),
        "mem_fault_size": int(result.mem_fault_size),
        "mem_access": int(result.mem_access),
        "steps": int(result.steps),
        "aot_blocks": int(result.aot_blocks),
        "jit_blocks": int(result.jit_blocks),
        "jit_compiles": int(result.jit_compiles),
        "jit_execs": int(result.jit_execs),
        "call_index": int(result.call_index),
        "total_calls": int(result.total_calls),
        "total_aot_blocks": int(result.total_aot_blocks),
        "total_jit_blocks": int(result.total_jit_blocks),
        "total_jit_compiles": int(result.total_jit_compiles),
        "total_jit_execs": int(result.total_jit_execs),
        "guest_rip": int(result.guest_rip),
        "guest_rsp": int(result.guest_rsp),
    }


def _module_info_dict(info: AevPeModuleInfo) -> dict:
    return {
        "struct_size": int(info.struct_size),
        "abi_version": int(info.abi_version),
        "guest_abi_version": int(info.guest_abi_version),
        "artifact_abi_version": int(info.artifact_abi_version),
        "translator_abi_version": int(info.translator_abi_version),
        "callback_abi_version": int(info.callback_abi_version),
        "guest_state_size": int(info.guest_state_size),
        "file_size": int(info.file_size),
        "preferred_base": int(info.preferred_base),
        "load_base": int(info.load_base),
        "image_size": int(info.image_size),
        "thunk_base": int(info.thunk_base),
        "import_count": int(info.import_count),
        "relocs_stripped": int(info.relocs_stripped),
        "entry_va": int(info.entry_va),
        "stack_base": int(info.stack_base),
        "stack_top": int(info.stack_top),
        "entry_rsp": int(info.entry_rsp),
        "region_count": int(info.region_count),
        "all_block_count": int(info.all_block_count),
        "selected_count": int(info.selected_count),
        "aot_present": int(info.aot_present),
        "load_count": int(info.load_count),
        "file_sha256": _as_hex(info.file_sha256),
        "image_sha256": _as_hex(info.image_sha256),
        "import_binding_sha256": _as_hex(info.import_binding_sha256),
        "host_binding_sha256": _as_hex(info.host_binding_sha256),
        "entry_exports_sha256": _as_hex(info.entry_exports_sha256),
        "aot_sha256": _as_hex(info.aot_sha256),
        "module_kind": _cstr(info.module_kind),
        "entry_export": _cstr(info.entry_export),
        "target_triple": _cstr(info.target_triple),
    }


def _counts_dict(counts: AevPeSessionCounts) -> dict:
    return {
        "struct_size": int(counts.struct_size),
        "abi_version": int(counts.abi_version),
        "load_count": int(counts.load_count),
        "calls_total": int(counts.calls_total),
        "host_import_calls": int(counts.host_import_calls),
        "aot_blocks_total": int(counts.aot_blocks_total),
        "jit_blocks_total": int(counts.jit_blocks_total),
        "jit_compiles_total": int(counts.jit_compiles_total),
        "jit_execs_total": int(counts.jit_execs_total),
        "trace_records": int(counts.trace_records),
        "trace_open": int(counts.trace_open),
    }


def _export_dict(info: AevPeExportInfo) -> dict:
    return {
        "struct_size": int(info.struct_size),
        "abi_version": int(info.abi_version),
        "name": _cstr(info.name),
        "rva": int(info.rva),
        "va": int(info.va),
        "ordinal": int(info.ordinal),
    }


def _block_dict(info: AevPeBlockInfo) -> dict:
    return {
        "struct_size": int(info.struct_size),
        "abi_version": int(info.abi_version),
        "rip": int(info.rip),
        "rva": int(info.rva),
        "size": int(info.size),
        "sha256": _as_hex(info.sha256),
        "aot_selected": int(info.aot_selected),
    }


class PeSession:
    """One live PE session.  Create and destroy on the AppKit main thread."""

    def __init__(self, library_path: str):
        self.library_path = os.path.abspath(library_path)
        self.lib = ctypes.CDLL(self.library_path)
        self._handle: int = 0
        self._observer = None
        self._observer_callbacks: list = []
        self._configure_prototypes()
        self.abi = self.abi_info()
        self._validate_abi(self.abi)

    # -- ABI -----------------------------------------------------------
    def _configure_prototypes(self) -> None:
        lib = self.lib
        lib.aev_pe_session_abi_info.argtypes = [ctypes.POINTER(AevPeSessionAbiInfo)]
        lib.aev_pe_session_abi_info.restype = ctypes.c_int
        lib.aev_pe_session_status_name.argtypes = [ctypes.c_uint32]
        lib.aev_pe_session_status_name.restype = ctypes.c_char_p
        lib.aev_pe_session_create.argtypes = [
            ctypes.POINTER(AevPeSessionConfig),
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_create.restype = ctypes.c_int
        lib.aev_pe_session_destroy.argtypes = [ctypes.c_uint64]
        lib.aev_pe_session_destroy.restype = ctypes.c_int
        lib.aev_pe_session_info.argtypes = [
            ctypes.c_uint64,
            ctypes.POINTER(AevPeModuleInfo),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_info.restype = ctypes.c_int
        lib.aev_pe_session_counts.argtypes = [
            ctypes.c_uint64,
            ctypes.POINTER(AevPeSessionCounts),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_counts.restype = ctypes.c_int
        lib.aev_pe_session_export_count.argtypes = [
            ctypes.c_uint64,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_export_count.restype = ctypes.c_int
        lib.aev_pe_session_export_info.argtypes = [
            ctypes.c_uint64,
            ctypes.c_uint32,
            ctypes.POINTER(AevPeExportInfo),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_export_info.restype = ctypes.c_int
        lib.aev_pe_session_find_export.argtypes = [
            ctypes.c_uint64,
            ctypes.c_char_p,
            ctypes.POINTER(AevPeExportInfo),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_find_export.restype = ctypes.c_int
        lib.aev_pe_session_selected_count.argtypes = [
            ctypes.c_uint64,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_selected_count.restype = ctypes.c_int
        lib.aev_pe_session_selected_rip.argtypes = [
            ctypes.c_uint64,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_selected_rip.restype = ctypes.c_int
        lib.aev_pe_session_all_block_count.argtypes = [
            ctypes.c_uint64,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_all_block_count.restype = ctypes.c_int
        lib.aev_pe_session_all_block_rip.argtypes = [
            ctypes.c_uint64,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_all_block_rip.restype = ctypes.c_int
        lib.aev_pe_session_block_info.argtypes = [
            ctypes.c_uint64,
            ctypes.c_uint64,
            ctypes.POINTER(AevPeBlockInfo),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_block_info.restype = ctypes.c_int
        lib.aev_pe_session_call.argtypes = [
            ctypes.c_uint64,
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.POINTER(AevPeCallResult),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_call.restype = ctypes.c_int
        lib.aev_pe_session_trace_open.argtypes = [
            ctypes.c_uint64,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_trace_open.restype = ctypes.c_int
        lib.aev_pe_session_trace_close.argtypes = [
            ctypes.c_uint64,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_trace_close.restype = ctypes.c_int
        lib.aev_pe_session_set_observer.argtypes = [
            ctypes.c_uint64,
            ctypes.POINTER(AevPeSessionObserver),
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        lib.aev_pe_session_set_observer.restype = ctypes.c_int

    @staticmethod
    def _validate_abi(abi: dict) -> None:
        if abi["abi_version"] != AEV_PE_SESSION_ABI_VERSION:
            raise PeSessionError(
                AEV_PE_SESSION_ERR_ABI_MISMATCH,
                f"session ABI {abi['abi_version']} != "
                f"{AEV_PE_SESSION_ABI_VERSION}",
            )
        expected = {
            "config_size": ctypes.sizeof(AevPeSessionConfig),
            "result_size": ctypes.sizeof(AevPeCallResult),
            "info_size": ctypes.sizeof(AevPeModuleInfo),
            "counts_size": ctypes.sizeof(AevPeSessionCounts),
            "export_info_size": ctypes.sizeof(AevPeExportInfo),
            "block_info_size": ctypes.sizeof(AevPeBlockInfo),
        }
        for key, value in expected.items():
            if int(abi[key]) != value:
                raise PeSessionError(
                    AEV_PE_SESSION_ERR_ABI_MISMATCH,
                    f"{key} {abi[key]} != ctypes {value}",
                )
        if int(abi["guest_abi_version"]) != AEV_GUEST_ABI_VERSION or \
                int(abi["guest_state_size"]) != AEV_GUEST_STATE_SIZE:
            raise PeSessionError(
                AEV_PE_SESSION_ERR_ABI_MISMATCH,
                f"guest ABI {abi['guest_abi_version']}/"
                f"{abi['guest_state_size']} != "
                f"{AEV_GUEST_ABI_VERSION}/{AEV_GUEST_STATE_SIZE}",
            )
        if int(abi["artifact_abi_version"]) != AEV_ARTIFACT_ABI_VERSION or \
                int(abi["translator_abi_version"]) != AEV_TRANSLATOR_ABI_VERSION or \
                int(abi["callback_abi_version"]) != AEV_CALLBACK_ABI_VERSION:
            raise PeSessionError(
                AEV_PE_SESSION_ERR_ABI_MISMATCH,
                "artifact/translator/callback ABI mismatch",
            )
        if int(abi["max_args"]) != AEV_PE_SESSION_MAX_ARGS:
            raise PeSessionError(
                AEV_PE_SESSION_ERR_ABI_MISMATCH,
                f"max_args {abi['max_args']} != {AEV_PE_SESSION_MAX_ARGS}",
            )

    def abi_info(self) -> dict:
        out = AevPeSessionAbiInfo()
        out.struct_size = ctypes.sizeof(AevPeSessionAbiInfo)
        out.abi_version = AEV_PE_SESSION_ABI_VERSION
        rc = self.lib.aev_pe_session_abi_info(ctypes.byref(out))
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, "aev_pe_session_abi_info failed")
        return {
            "struct_size": int(out.struct_size),
            "abi_version": int(out.abi_version),
            "config_size": int(out.config_size),
            "result_size": int(out.result_size),
            "info_size": int(out.info_size),
            "counts_size": int(out.counts_size),
            "export_info_size": int(out.export_info_size),
            "block_info_size": int(out.block_info_size),
            "guest_abi_version": int(out.guest_abi_version),
            "guest_state_size": int(out.guest_state_size),
            "artifact_abi_version": int(out.artifact_abi_version),
            "translator_abi_version": int(out.translator_abi_version),
            "callback_abi_version": int(out.callback_abi_version),
            "max_args": int(out.max_args),
        }

    # -- lifecycle -----------------------------------------------------
    @property
    def handle(self) -> int:
        return int(self._handle)

    @property
    def live(self) -> bool:
        return bool(self._handle)

    def _require_handle(self) -> int:
        if not self._handle:
            raise PeSessionError(
                AEV_PE_SESSION_ERR_STALE_HANDLE,
                "session has been destroyed or was never created",
            )
        return int(self._handle)

    @staticmethod
    def _errbuf() -> ctypes.Array:
        return ctypes.create_string_buffer(2048)

    @staticmethod
    def _errmsg(buf) -> str:
        return bytes(buf.value).decode("utf-8", "replace")

    def create(self, dll_path: str, registry_path: str,
               aot_path: Optional[str] = None,
               entry_export: Optional[str] = None,
               load_base: int = 0,
               trace_path: Optional[str] = None,
               scenario: Optional[str] = None,
               flags: int = 0) -> int:
        if self._handle:
            raise PeSessionError(
                AEV_PE_SESSION_ERR_INVALID_ARGUMENT,
                "session already created",
            )
        if not dll_path or not registry_path:
            raise PeSessionError(
                AEV_PE_SESSION_ERR_INVALID_ARGUMENT,
                "dll_path and registry_path are required",
            )
        cfg = AevPeSessionConfig()
        cfg.struct_size = ctypes.sizeof(AevPeSessionConfig)
        cfg.abi_version = AEV_PE_SESSION_ABI_VERSION
        cfg.dll_path = str(dll_path).encode()
        cfg.registry_path = str(registry_path).encode()
        cfg.aot_path = str(aot_path).encode() if aot_path else None
        cfg.entry_export = str(entry_export).encode() if entry_export else None
        cfg.load_base = int(load_base)
        cfg.trace_path = str(trace_path).encode() if trace_path else None
        cfg.scenario = str(scenario).encode() if scenario else None
        cfg.flags = int(flags)
        cfg.reserved0 = 0
        cfg.reserved1 = 0
        handle = ctypes.c_uint64(0)
        err = self._errbuf()
        rc = self.lib.aev_pe_session_create(
            ctypes.byref(cfg), ctypes.byref(handle), err, ctypes.sizeof(err)
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "session create failed")
        if not handle.value:
            raise PeSessionError(
                AEV_PE_SESSION_ERR_INTERNAL,
                "session create returned OK with a null handle",
            )
        self._handle = int(handle.value)
        return self._handle

    def destroy(self) -> None:
        handle = self._require_handle()
        rc = self.lib.aev_pe_session_destroy(handle)
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, "session destroy failed")
        self._handle = 0
        self._observer = None
        self._observer_callbacks = []

    # -- queries -------------------------------------------------------
    def info(self) -> dict:
        out = AevPeModuleInfo()
        out.struct_size = ctypes.sizeof(AevPeModuleInfo)
        out.abi_version = AEV_PE_SESSION_ABI_VERSION
        err = self._errbuf()
        rc = self.lib.aev_pe_session_info(
            self._require_handle(), ctypes.byref(out), err, ctypes.sizeof(err)
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "session info failed")
        return _module_info_dict(out)

    def counts(self) -> dict:
        out = AevPeSessionCounts()
        out.struct_size = ctypes.sizeof(AevPeSessionCounts)
        out.abi_version = AEV_PE_SESSION_ABI_VERSION
        err = self._errbuf()
        rc = self.lib.aev_pe_session_counts(
            self._require_handle(), ctypes.byref(out), err, ctypes.sizeof(err)
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "session counts failed")
        return _counts_dict(out)

    def export_count(self) -> int:
        out = ctypes.c_uint32(0)
        err = self._errbuf()
        rc = self.lib.aev_pe_session_export_count(
            self._require_handle(), ctypes.byref(out), err, ctypes.sizeof(err)
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "export_count failed")
        return int(out.value)

    def export_info(self, index: int) -> dict:
        out = AevPeExportInfo()
        out.struct_size = ctypes.sizeof(AevPeExportInfo)
        out.abi_version = AEV_PE_SESSION_ABI_VERSION
        err = self._errbuf()
        rc = self.lib.aev_pe_session_export_info(
            self._require_handle(), int(index), ctypes.byref(out), err,
            ctypes.sizeof(err),
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "export_info failed")
        return _export_dict(out)

    def find_export(self, name: str) -> dict:
        out = AevPeExportInfo()
        out.struct_size = ctypes.sizeof(AevPeExportInfo)
        out.abi_version = AEV_PE_SESSION_ABI_VERSION
        err = self._errbuf()
        rc = self.lib.aev_pe_session_find_export(
            self._require_handle(), str(name).encode(), ctypes.byref(out), err,
            ctypes.sizeof(err),
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "find_export failed")
        return _export_dict(out)

    def exports(self) -> list[dict]:
        return [self.export_info(i) for i in range(self.export_count())]

    def selected_count(self) -> int:
        out = ctypes.c_uint32(0)
        err = self._errbuf()
        rc = self.lib.aev_pe_session_selected_count(
            self._require_handle(), ctypes.byref(out), err, ctypes.sizeof(err)
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "selected_count failed")
        return int(out.value)

    def selected_rip(self, index: int) -> int:
        out = ctypes.c_uint64(0)
        err = self._errbuf()
        rc = self.lib.aev_pe_session_selected_rip(
            self._require_handle(), int(index), ctypes.byref(out), err,
            ctypes.sizeof(err),
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "selected_rip failed")
        return int(out.value)

    def selected_rips(self) -> list[int]:
        return [self.selected_rip(i) for i in range(self.selected_count())]

    def all_block_count(self) -> int:
        out = ctypes.c_uint32(0)
        err = self._errbuf()
        rc = self.lib.aev_pe_session_all_block_count(
            self._require_handle(), ctypes.byref(out), err, ctypes.sizeof(err)
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "all_block_count failed")
        return int(out.value)

    def all_block_rip(self, index: int) -> int:
        out = ctypes.c_uint64(0)
        err = self._errbuf()
        rc = self.lib.aev_pe_session_all_block_rip(
            self._require_handle(), int(index), ctypes.byref(out), err,
            ctypes.sizeof(err),
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "all_block_rip failed")
        return int(out.value)

    def all_block_rips(self) -> list[int]:
        return [self.all_block_rip(i) for i in range(self.all_block_count())]

    def block_info(self, rip: int) -> dict:
        out = AevPeBlockInfo()
        out.struct_size = ctypes.sizeof(AevPeBlockInfo)
        out.abi_version = AEV_PE_SESSION_ABI_VERSION
        err = self._errbuf()
        rc = self.lib.aev_pe_session_block_info(
            self._require_handle(), int(rip), ctypes.byref(out), err,
            ctypes.sizeof(err),
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "block_info failed")
        return _block_dict(out)

    # -- calls ---------------------------------------------------------
    def call(self, export_name: str, args: Iterable[int] = (),
             raise_on_error: bool = True) -> dict:
        values = [int(x) & 0xFFFFFFFFFFFFFFFF for x in args]
        if len(values) > AEV_PE_SESSION_MAX_ARGS:
            raise PeSessionError(
                AEV_PE_SESSION_ERR_INVALID_ARGUMENT,
                f"at most {AEV_PE_SESSION_MAX_ARGS} arguments are supported",
            )
        padded = values + [0] * (AEV_PE_SESSION_MAX_ARGS - len(values))
        argv = (ctypes.c_uint64 * AEV_PE_SESSION_MAX_ARGS)(*padded)
        out = AevPeCallResult()
        out.struct_size = ctypes.sizeof(AevPeCallResult)
        out.abi_version = AEV_PE_SESSION_ABI_VERSION
        err = self._errbuf()
        rc = self.lib.aev_pe_session_call(
            self._require_handle(), str(export_name).encode(), argv,
            ctypes.byref(out), err, ctypes.sizeof(err),
        )
        if rc != AEV_PE_SESSION_OK:
            out.status = rc
        result = _call_result_dict(out)
        if rc != AEV_PE_SESSION_OK and raise_on_error:
            raise PeSessionError(
                rc, self._errmsg(err) or f"{export_name} call failed", result=result
            )
        return result

    # -- trace ---------------------------------------------------------
    def trace_open(self, path: str, scenario: Optional[str] = None) -> None:
        err = self._errbuf()
        rc = self.lib.aev_pe_session_trace_open(
            self._require_handle(), str(path).encode(),
            str(scenario).encode() if scenario else None,
            err, ctypes.sizeof(err),
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "trace_open failed")

    def trace_close(self) -> None:
        err = self._errbuf()
        rc = self.lib.aev_pe_session_trace_close(
            self._require_handle(), err, ctypes.sizeof(err)
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "trace_close failed")

    # -- observer (diagnostic; not used by the GUI business path) ------
    def set_observer(self, on_call_end=None, on_host_import=None,
                     ctx: Optional[int] = None) -> None:
        self._observer_callbacks = []
        if on_call_end is None and on_host_import is None:
            observer = None
        else:
            if on_call_end is not None:
                def _call_end(_ctx, result_ptr, _cb=on_call_end):
                    _cb(_call_result_dict(result_ptr.contents))
                cb_end = OnCallEnd(_call_end)
            else:
                cb_end = OnCallEnd()
            if on_host_import is not None:
                cb_import = OnHostImport(on_host_import)
            else:
                cb_import = OnHostImport()
            self._observer_callbacks = [cb_end, cb_import]
            observer = AevPeSessionObserver()
            observer.struct_size = ctypes.sizeof(AevPeSessionObserver)
            observer.abi_version = AEV_PE_SESSION_ABI_VERSION
            observer.on_call_end = cb_end
            observer.on_host_import = cb_import
        self._observer = observer
        err = self._errbuf()
        rc = self.lib.aev_pe_session_set_observer(
            self._require_handle(),
            ctypes.byref(observer) if observer is not None else None,
            ctypes.c_void_p(ctx) if ctx is not None else None,
            err, ctypes.sizeof(err),
        )
        if rc != AEV_PE_SESSION_OK:
            raise PeSessionError(rc, self._errmsg(err) or "set_observer failed")

    def clear_observer(self) -> None:
        self.set_observer(None, None, None)
