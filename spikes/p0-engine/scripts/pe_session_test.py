#!/usr/bin/env python3
"""P0-PE-GUI-01: headless C ABI verification for libaev_pe_session.dylib.

This test drives the versioned session ABI directly with ctypes.  It uses the
committed aevguest.dll fixture, the same DLL bytes as the Windows reference,
and an independent Python model of the fixture's business state.  AOT artifacts
are generated from the actual input PE with aev-translate + clang; the fixture
DLL is never modified.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
from pathlib import Path

PREFERRED = 0x180000000
NON_PREFERRED = 0x190000000
SENTINEL = 0xFFFFFFFFFFFFFF00
ERR_CAP = 2048
EXPORTS = sorted([
    "AevDllCallPtr", "AevDllCalls", "AevDllCompute",
    "AevDllCursorAddress", "AevDllCursorValue", "AevDllReadAt",
    "AevDllReset", "AevDllState", "AevDllWriteAt",
])

c_uint64_p = ctypes.POINTER(ctypes.c_uint64)
c_char_p = ctypes.c_char_p
c_size_t = ctypes.c_size_t


class AevPeSessionConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("dll_path", c_char_p),
        ("registry_path", c_char_p),
        ("aot_path", c_char_p),
        ("entry_export", c_char_p),
        ("load_base", ctypes.c_uint64),
        ("trace_path", c_char_p),
        ("scenario", c_char_p),
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
        ("args", ctypes.c_uint64 * 4),
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
        ("entry_export", ctypes.c_char * 512),
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
        ("name", ctypes.c_char * 512),
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


ObsCallEnd = ctypes.CFUNCTYPE(None, ctypes.c_void_p,
                              ctypes.POINTER(AevPeCallResult))
ObsHostImport = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_uint64,
                                 ctypes.c_uint32)


class AevPeSessionObserver(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("on_call_end", ObsCallEnd),
        ("on_host_import", ObsHostImport),
    ]


class Failure(RuntimeError):
    pass


def s32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value >= 0x80000000 else value


def u32(value: int) -> int:
    return value & 0xFFFFFFFF


class Oracle:
    """Independent model of testdata/pe/aevguest.c global state."""

    def __init__(self) -> None:
        self.table = [0] * 8
        self.seed = 0
        self.counter = 0
        self.calls = 0

    def reset(self, seed: int) -> None:
        self.seed = seed
        for i in range(8):
            self.table[i] = s32(u32(seed + (i << 3) - i) ^ 0x5A5A5A5A)
        self.counter = 0
        self.calls = 0

    def compute(self, a: int, b: int, n: int) -> int:
        acc = s32(u32(self.seed) ^ u32(a))
        for i in range(n):
            v = self.table[i & 7]
            if v & 1:
                acc = s32(u32(acc) + u32(v) + u32(v) + u32(v))
            else:
                acc = s32(u32(acc) - (u32(v) + u32(i)))
            acc = s32((u32(acc) << 1) | (u32(acc) >> 31))
        if u32(a) & 4:
            acc = s32(u32(acc) + u32(self.table[5]))
        else:
            acc = s32(u32(acc) - u32(self.table[6]))
        h = s32(u32(u32(a + self.calls) * 3 + u32(b) + 1))
        self.counter = (self.counter + u32(acc) + u32(h)) & 0xFFFFFFFFFFFFFFFF
        self.calls += 1
        return s32(u32(acc) ^ u32(self.table[2]))

    def state(self) -> int:
        return s32(u32(self.counter) ^ u32(self.calls))


class Driver:
    def __init__(self, build: Path, source: Path, results: Path) -> None:
        self.build = build.resolve()
        self.source = source.resolve()
        self.results = results.resolve()
        self.pe_dir = self.source / "testdata" / "pe"
        self.art = self.results / "artifacts"
        self.logs = self.results / "logs"
        self.art.mkdir(parents=True, exist_ok=True)
        self.logs.mkdir(parents=True, exist_ok=True)
        self.lib_path = self.build / "libaev_pe_session.dylib"
        self.runner = self.build / "aev-pe-runner"
        self.translator = self.build / "aev-translate"
        self.rt_obj = self.build / "generated" / "aot_runtime.o"
        self.mem_obj = self.build / "generated" / "aev_mem_aot.o"
        self.clang = shutil.which("clang") or "/usr/bin/clang"
        for path in (self.lib_path, self.runner, self.translator,
                     self.rt_obj, self.mem_obj):
            if not path.is_file():
                raise Failure(f"missing build artifact: {path}")
        self.lib = ctypes.CDLL(str(self.lib_path), mode=ctypes.RTLD_LOCAL)
        self._bind()
        self.checks: list[tuple[bool, str]] = []
        self.failures = 0

    def _bind(self) -> None:
        lib = self.lib
        lib.aev_pe_session_abi_info.argtypes = [ctypes.POINTER(AevPeSessionAbiInfo)]
        lib.aev_pe_session_abi_info.restype = ctypes.c_uint32
        lib.aev_pe_session_status_name.argtypes = [ctypes.c_uint32]
        lib.aev_pe_session_status_name.restype = c_char_p
        lib.aev_pe_session_create.argtypes = [
            ctypes.POINTER(AevPeSessionConfig), c_uint64_p,
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_create.restype = ctypes.c_uint32
        lib.aev_pe_session_destroy.argtypes = [ctypes.c_uint64]
        lib.aev_pe_session_destroy.restype = ctypes.c_uint32
        lib.aev_pe_session_info.argtypes = [
            ctypes.c_uint64, ctypes.POINTER(AevPeModuleInfo),
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_info.restype = ctypes.c_uint32
        lib.aev_pe_session_counts.argtypes = [
            ctypes.c_uint64, ctypes.POINTER(AevPeSessionCounts),
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_counts.restype = ctypes.c_uint32
        lib.aev_pe_session_export_count.argtypes = [
            ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_export_count.restype = ctypes.c_uint32
        lib.aev_pe_session_export_info.argtypes = [
            ctypes.c_uint64, ctypes.c_uint32, ctypes.POINTER(AevPeExportInfo),
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_export_info.restype = ctypes.c_uint32
        lib.aev_pe_session_find_export.argtypes = [
            ctypes.c_uint64, c_char_p, ctypes.POINTER(AevPeExportInfo),
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_find_export.restype = ctypes.c_uint32
        lib.aev_pe_session_selected_count.argtypes = [
            ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_selected_count.restype = ctypes.c_uint32
        lib.aev_pe_session_selected_rip.argtypes = [
            ctypes.c_uint64, ctypes.c_uint32, c_uint64_p,
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_selected_rip.restype = ctypes.c_uint32
        lib.aev_pe_session_all_block_count.argtypes = [
            ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_all_block_count.restype = ctypes.c_uint32
        lib.aev_pe_session_all_block_rip.argtypes = [
            ctypes.c_uint64, ctypes.c_uint32, c_uint64_p,
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_all_block_rip.restype = ctypes.c_uint32
        lib.aev_pe_session_block_info.argtypes = [
            ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(AevPeBlockInfo),
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_block_info.restype = ctypes.c_uint32
        lib.aev_pe_session_call.argtypes = [
            ctypes.c_uint64, c_char_p, c_uint64_p,
            ctypes.POINTER(AevPeCallResult), ctypes.POINTER(ctypes.c_char),
            c_size_t]
        lib.aev_pe_session_call.restype = ctypes.c_uint32
        lib.aev_pe_session_trace_open.argtypes = [
            ctypes.c_uint64, c_char_p, c_char_p,
            ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_trace_open.restype = ctypes.c_uint32
        lib.aev_pe_session_trace_close.argtypes = [
            ctypes.c_uint64, ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_trace_close.restype = ctypes.c_uint32
        lib.aev_pe_session_set_observer.argtypes = [
            ctypes.c_uint64, ctypes.POINTER(AevPeSessionObserver),
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_char), c_size_t]
        lib.aev_pe_session_set_observer.restype = ctypes.c_uint32

    def check(self, condition: bool, message: str) -> None:
        self.checks.append((bool(condition), message))
        if not condition:
            self.failures += 1
            print(f"FAIL: {message}")

    @staticmethod
    def errbuf() -> ctypes.Array:
        return ctypes.create_string_buffer(ERR_CAP)

    @staticmethod
    def errtxt(buf) -> str:
        return buf.value.decode("utf-8", "replace")

    def config(self, *, dll=None, registry=None, aot=None, entry="AevDllCompute",
               load_base=PREFERRED, trace=None, scenario=None, flags=0,
               reserved0=0, reserved1=0, struct_size=None,
               abi_version=1) -> AevPeSessionConfig:
        cfg = AevPeSessionConfig()
        cfg.struct_size = (ctypes.sizeof(AevPeSessionConfig)
                           if struct_size is None else struct_size)
        cfg.abi_version = abi_version
        cfg.dll_path = str((self.pe_dir / "aevguest.dll")
                           if dll is None else dll).encode()
        cfg.registry_path = str((self.pe_dir / "aevimports.txt")
                                if registry is None else registry).encode()
        cfg.aot_path = str(aot).encode() if aot else None
        cfg.entry_export = entry.encode() if entry is not None else None
        cfg.load_base = load_base
        cfg.trace_path = str(trace).encode() if trace else None
        cfg.scenario = scenario.encode() if scenario else None
        cfg.flags = flags
        cfg.reserved0 = reserved0
        cfg.reserved1 = reserved1
        return cfg

    def create(self, cfg: AevPeSessionConfig, cap: int = ERR_CAP):
        handle = ctypes.c_uint64(0)
        err = ctypes.create_string_buffer(cap)
        status = self.lib.aev_pe_session_create(
            ctypes.byref(cfg), ctypes.byref(handle), err, cap)
        return status, handle.value, self.errtxt(err)

    def require_session(self, **kwargs) -> int:
        cfg = self.config(**kwargs)
        status, handle, error = self.create(cfg)
        if status != 0:
            raise Failure(f"create failed status={status}: {error}")
        return handle

    def info(self, handle: int, size=None):
        out = AevPeModuleInfo()
        out.struct_size = ctypes.sizeof(AevPeModuleInfo) if size is None else size
        err = self.errbuf()
        status = self.lib.aev_pe_session_info(
            handle, ctypes.byref(out), err, ERR_CAP)
        return status, out, self.errtxt(err)

    def counts(self, handle: int, size=None):
        out = AevPeSessionCounts()
        out.struct_size = ctypes.sizeof(AevPeSessionCounts) if size is None else size
        err = self.errbuf()
        status = self.lib.aev_pe_session_counts(
            handle, ctypes.byref(out), err, ERR_CAP)
        return status, out, self.errtxt(err)

    def call(self, handle: int, name: str, args=(0, 0, 0, 0), size=None):
        out = AevPeCallResult()
        out.struct_size = ctypes.sizeof(AevPeCallResult) if size is None else size
        args_arr = (ctypes.c_uint64 * 4)(*[u32(a) for a in args])
        err = self.errbuf()
        status = self.lib.aev_pe_session_call(
            handle, name.encode(), args_arr, ctypes.byref(out), err, ERR_CAP)
        return status, out, self.errtxt(err)

    def export_info(self, handle: int, index: int):
        out = AevPeExportInfo()
        out.struct_size = ctypes.sizeof(AevPeExportInfo)
        err = self.errbuf()
        status = self.lib.aev_pe_session_export_info(
            handle, index, ctypes.byref(out), err, ERR_CAP)
        return status, out, self.errtxt(err)

    def find_export(self, handle: int, name: str):
        out = AevPeExportInfo()
        out.struct_size = ctypes.sizeof(AevPeExportInfo)
        err = self.errbuf()
        status = self.lib.aev_pe_session_find_export(
            handle, name.encode(), ctypes.byref(out), err, ERR_CAP)
        return status, out, self.errtxt(err)

    def block_info(self, handle: int, rip: int):
        out = AevPeBlockInfo()
        out.struct_size = ctypes.sizeof(AevPeBlockInfo)
        err = self.errbuf()
        status = self.lib.aev_pe_session_block_info(
            handle, rip, ctypes.byref(out), err, ERR_CAP)
        return status, out, self.errtxt(err)

    def trace_open(self, handle: int, path: Path, scenario: str = "session"):
        err = self.errbuf()
        status = self.lib.aev_pe_session_trace_open(
            handle, str(path).encode(), scenario.encode(), err, ERR_CAP)
        return status, self.errtxt(err)

    def trace_close(self, handle: int):
        err = self.errbuf()
        status = self.lib.aev_pe_session_trace_close(handle, err, ERR_CAP)
        return status, self.errtxt(err)

    def translate_full(self, tag: str, load_base: int = PREFERRED):
        ll = self.art / f"{tag}.ll"
        header = self.art / f"{tag}.h"
        manifest = self.art / f"{tag}.json"
        cmd = [str(self.translator), "--pe", str(self.pe_dir / "aevguest.dll"),
               "--load-base", hex(load_base),
               "--registry", str(self.pe_dir / "aevimports.txt"),
               "--entry", "AevDllCompute",
               "--out-ll", str(ll), "--out-header", str(header),
               "--out-manifest", str(manifest)]
        for name in EXPORTS:
            cmd += ["--export", name]
        self.log("translate-" + tag, " ".join(cmd))
        proc = subprocess.run(cmd, text=True, capture_output=True)
        self.log("translate-" + tag + ".stdout", proc.stdout)
        self.log("translate-" + tag + ".stderr", proc.stderr)
        if proc.returncode != 0:
            raise Failure(f"translator {tag} failed: {proc.stderr}")
        obj = self.art / f"{tag}.o"
        dylib = self.art / f"lib{tag}.dylib"
        proc = subprocess.run(
            [self.clang, "--target=arm64-apple-macos14.0", "-O2", "-c",
             str(ll), "-o", str(obj)], text=True, capture_output=True)
        if proc.returncode != 0:
            raise Failure(f"clang IR {tag}: {proc.stderr}")
        proc = subprocess.run(
            [self.clang, "-arch", "arm64", "-dynamiclib", "-o", str(dylib),
             str(obj), str(self.rt_obj), str(self.mem_obj)],
            text=True, capture_output=True)
        if proc.returncode != 0:
            raise Failure(f"clang link {tag}: {proc.stderr}")
        return obj, dylib, json.loads(manifest.read_text())

    def log(self, name: str, text: str) -> None:
        (self.logs / name).write_text(text)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--results", required=True)
    args = ap.parse_args()
    drv = Driver(Path(args.build), Path(args.source), Path(args.results))
    lib = drv.lib

    try:
        # The committed fixture bytes are the conversion input; variants are
        # written only under the ignored results directory.
        for line in (drv.pe_dir / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split()
            actual = hashlib.sha256((drv.pe_dir / name).read_bytes()).hexdigest()
            drv.check(actual == digest, f"fixture hash unchanged: {name}")

        # ---------------------------------------------------------- ABI info
        abi = AevPeSessionAbiInfo()
        abi.struct_size = 0
        st = lib.aev_pe_session_abi_info(ctypes.byref(abi))
        drv.check(st == 0, "abi_info status")
        drv.check(abi.struct_size == ctypes.sizeof(AevPeSessionAbiInfo),
                  "abi_info fills its own size")
        drv.check(abi.abi_version == 1, "abi_info version")
        drv.check(abi.config_size == ctypes.sizeof(AevPeSessionConfig),
                  "config size")
        drv.check(abi.result_size == ctypes.sizeof(AevPeCallResult),
                  "result size")
        drv.check(abi.info_size == ctypes.sizeof(AevPeModuleInfo),
                  "info size")
        drv.check(abi.counts_size == ctypes.sizeof(AevPeSessionCounts),
                  "counts size")
        drv.check(abi.export_info_size == ctypes.sizeof(AevPeExportInfo),
                  "export info size")
        drv.check(abi.block_info_size == ctypes.sizeof(AevPeBlockInfo),
                  "block info size")
        drv.check(abi.guest_abi_version == 2 and abi.guest_state_size == 296,
                  "guest ABI 2 / 296")
        drv.check(abi.artifact_abi_version == 2 and
                  abi.translator_abi_version == 2 and
                  abi.callback_abi_version == 3 and abi.max_args == 4,
                  "artifact/translator/callback ABI and max args")
        exact = AevPeSessionAbiInfo()
        exact.struct_size = ctypes.sizeof(AevPeSessionAbiInfo)
        drv.check(lib.aev_pe_session_abi_info(ctypes.byref(exact)) == 0,
                  "abi_info accepts exact caller size")
        wrong = AevPeSessionAbiInfo()
        wrong.struct_size = ctypes.sizeof(AevPeSessionAbiInfo) + 1
        drv.check(lib.aev_pe_session_abi_info(ctypes.byref(wrong)) == 4,
                  "abi_info rejects wrong caller size")
        drv.check(lib.aev_pe_session_abi_info(None) == 2,
                  "abi_info rejects NULL")
        drv.check(lib.aev_pe_session_status_name(13) == b"WRONG_THREAD",
                  "status name wrong thread")
        drv.check(lib.aev_pe_session_status_name(14) == b"TRACE",
                  "status name trace")

        # ------------------------------------------------- create validation
        status, handle, _ = drv.create(drv.config(struct_size=1))
        drv.check(status == 4 and handle == 0, "create rejects struct_size")
        status, handle, _ = drv.create(drv.config(abi_version=2))
        drv.check(status == 4 and handle == 0, "create rejects abi_version")
        status, handle, _ = drv.create(drv.config(flags=1))
        drv.check(status == 2 and handle == 0, "create rejects flags")
        status, handle, _ = drv.create(drv.config(reserved0=1))
        drv.check(status == 2 and handle == 0, "create rejects reserved0")
        status, handle, _ = drv.create(drv.config(reserved1=1))
        drv.check(status == 2 and handle == 0, "create rejects reserved1")
        status, handle, _ = drv.create(drv.config(dll=""))
        drv.check(status == 2 and handle == 0, "create rejects empty dll path")
        status, handle, _ = drv.create(drv.config(load_base=0))
        drv.check(status == 2 and handle == 0,
                  "create requires load base without AOT")

        # -------------------------------------------------- partial failures
        truncated = drv.art / "truncated.dll"
        truncated.write_bytes((drv.pe_dir / "aevguest.dll").read_bytes()[:100])
        status, handle, error = drv.create(drv.config(dll=truncated))
        drv.check(status == 3 and handle == 0 and error,
                  f"truncated DLL -> LOAD (got {status}: {error})")

        malformed = drv.art / "bad.aevimports"
        malformed.write_text("# broken\nnot a registry line\n")
        status, handle, error = drv.create(drv.config(registry=malformed))
        drv.check(status == 3 and handle == 0,
                  f"malformed registry -> LOAD (got {status}: {error})")

        empty_registry = drv.art / "empty.aevimports"
        empty_registry.write_text("# empty\n")
        status, handle, error = drv.create(drv.config(registry=empty_registry))
        drv.check(status == 5 and handle == 0 and "unknown import" in error,
                  f"undeclared PE import -> UNKNOWN_IMPORT (got {status}: {error})")

        bad_host = drv.art / "badhost.aevimports"
        bad_host.write_text("aevhost.dll|AevUpdate|2|2|win64-u64\n")
        status, handle, error = drv.create(drv.config(registry=bad_host))
        drv.check(status == 5 and handle == 0,
                  f"registry ID without host impl -> UNKNOWN_IMPORT (got {status}: {error})")

        status, handle, error = drv.create(
            drv.config(aot=drv.art / "missing.dylib"))
        drv.check(status == 3 and handle == 0,
                  f"missing AOT file -> LOAD (got {status}: {error})")

        bogus_aot = drv.art / "bogus_aot.dylib"
        bogus_aot.write_bytes(b"not a Mach-O dylib")
        status, handle, error = drv.create(drv.config(aot=bogus_aot))
        drv.check(status == 3 and handle == 0,
                  f"unloadable AOT file -> LOAD (got {status}: {error})")

        status, handle, error = drv.create(
            drv.config(aot=drv.art / "missing.dylib", entry=None))
        drv.check(status == 2 and handle == 0,
                  f"AOT without entry export -> INVALID_ARGUMENT (got {status}: {error})")

        # A failing create must not leave a half session; a valid create still
        # succeeds immediately afterwards.
        status, handle, error = drv.create(drv.config())
        drv.check(status == 0 and handle != 0,
                  f"valid create after failures (got {status}: {error})")
        if handle:
            drv.check(lib.aev_pe_session_destroy(handle) == 0,
                      "destroy probe session")

        # ------------------------------------------------- handle table limits
        probes = []
        for index in range(8):
            status, handle, error = drv.create(drv.config())
            drv.check(status == 0 and handle != 0,
                      f"create probe session {index} ({status}: {error})")
            if handle:
                probes.append(handle)
        drv.check(len(probes) == 8 and len(set(probes)) == 8,
                  "eight distinct generation handles")
        status, handle, error = drv.create(drv.config())
        drv.check(status == 2 and handle == 0 and "table full" in error,
                  f"ninth session -> INVALID_ARGUMENT table full ({status}: {error})")
        first_probe = probes[0] if probes else 0
        for handle in probes:
            drv.check(lib.aev_pe_session_destroy(handle) == 0,
                      "destroy probe table session")
        st, _, _ = drv.info(first_probe)
        drv.check(st == 9, "destroyed generation token is stale")

        # A trace that cannot be opened is a TRACE-class create failure and
        # must release the loaded module/JIT resources.
        status, handle, error = drv.create(drv.config(
            trace=drv.art / "missing_dir" / "trace.jsonl"))
        drv.check(status == 14 and handle == 0,
                  f"unopenable create trace -> TRACE ({status}: {error})")

        # --------------------------------------------------------- main session
        trace_path = drv.art / "session_main.jsonl"
        session = drv.require_session(trace=trace_path,
                                      scenario="session-test")
        st, c, _ = drv.counts(session)
        drv.check(st == 0 and c.load_count == 1 and c.calls_total == 0 and
                  c.trace_open == 1 and c.trace_records == 0,
                  "load_count=1, trace auto-open at create")
        st, info, _ = drv.info(session)
        drv.check(st == 0, "info status")
        drv.check(info.abi_version == 1 and
                  info.guest_abi_version == 2 and
                  info.artifact_abi_version == 2 and
                  info.translator_abi_version == 2 and
                  info.callback_abi_version == 3, "info ABI fields")
        drv.check(info.file_size == (drv.pe_dir / "aevguest.dll").stat().st_size,
                  "info file size")
        expected_file_sha = hashlib.sha256(
            (drv.pe_dir / "aevguest.dll").read_bytes()).digest()
        drv.check(bytes(info.file_sha256) == expected_file_sha,
                  "info file sha256")
        drv.check(info.load_base == PREFERRED and
                  info.preferred_base == PREFERRED, "info load base")
        drv.check(info.image_size == 0x6000 and info.import_count == 1 and
                  info.relocs_stripped == 0 and info.thunk_base == PREFERRED + 0x6000,
                  "info image/import/thunk")
        drv.check(info.entry_export == b"AevDllCompute" and
                  info.module_kind == b"pe32+amd64-dll" and
                  info.target_triple == b"arm64-apple-macosx14.0.0",
                  "info names/kind/target")
        drv.check(info.all_block_count == 25 and info.selected_count == 0 and
                  info.aot_present == 0, "info JIT-only block counts")
        drv.check(info.region_count >= 8, "info region count")
        drv.check(info.entry_va == PREFERRED + 0x10a0, "info entry VA")

        st, info_bad, _ = drv.info(session, size=ctypes.sizeof(AevPeModuleInfo) + 1)
        drv.check(st == 4, "info rejects wrong struct_size")
        st, counts_bad, _ = drv.counts(
            session, size=ctypes.sizeof(AevPeSessionCounts) + 1)
        drv.check(st == 4, "counts rejects wrong struct_size")
        st, _, _ = drv.call(session, "AevDllState",
                            size=ctypes.sizeof(AevPeCallResult) + 1)
        drv.check(st == 4, "call rejects wrong struct_size")

        count = ctypes.c_uint32()
        err = drv.errbuf()
        st = lib.aev_pe_session_export_count(session, ctypes.byref(count), err, ERR_CAP)
        drv.check(st == 0 and count.value == 9, "export_count == 9")
        names = []
        for i in range(count.value):
            st, e, _ = drv.export_info(session, i)
            drv.check(st == 0 and int(e.ordinal) == i + 1,
                      f"export_info ordinal {i}")
            drv.check(e.va == PREFERRED + e.rva, f"export VA {i}")
            names.append(e.name.decode())
        drv.check(sorted(names) == EXPORTS, "export names match fixture")
        st, e, _ = drv.find_export(session, "AevDllCompute")
        drv.check(st == 0 and e.name == b"AevDllCompute" and e.rva == 0x10a0,
                  "find_export AevDllCompute")
        st, _, _ = drv.find_export(session, "DoesNotExist")
        drv.check(st == 2, "find_export missing -> INVALID_ARGUMENT")
        st, _, _ = drv.export_info(session, 9)
        drv.check(st == 2, "export_info out of range")

        sel = ctypes.c_uint32(99)
        err = drv.errbuf()
        st = lib.aev_pe_session_selected_count(session, ctypes.byref(sel), err, ERR_CAP)
        drv.check(st == 0 and sel.value == 0, "JIT-only selected_count == 0")
        rip = ctypes.c_uint64()
        err = drv.errbuf()
        st = lib.aev_pe_session_selected_rip(session, 0, ctypes.byref(rip), err, ERR_CAP)
        drv.check(st == 2, "selected_rip empty -> INVALID_ARGUMENT")

        all_count = ctypes.c_uint32()
        err = drv.errbuf()
        st = lib.aev_pe_session_all_block_count(session, ctypes.byref(all_count), err, ERR_CAP)
        drv.check(st == 0 and all_count.value == 25, "all_block_count == 25")
        rips = []
        for i in range(all_count.value):
            r = ctypes.c_uint64()
            err = drv.errbuf()
            st = lib.aev_pe_session_all_block_rip(session, i, ctypes.byref(r), err, ERR_CAP)
            drv.check(st == 0, f"all_block_rip {i}")
            rips.append(r.value)
        drv.check(rips == sorted(rips) and len(set(rips)) == len(rips),
                  "all block RIPs sorted and unique")
        err = drv.errbuf()
        last = ctypes.c_uint64()
        drv.check(lib.aev_pe_session_all_block_rip(
            session, all_count.value, ctypes.byref(last), err, ERR_CAP) == 2,
            "all_block_rip out of range -> INVALID_ARGUMENT")
        st, b, _ = drv.block_info(session, rips[0])
        drv.check(st == 0 and b.rip == rips[0] and b.rva < 0x6000 and
                  b.size > 0 and b.aot_selected == 0 and
                  any(b.sha256), "block_info JIT-only block")
        st, _, _ = drv.block_info(session, 0xDEADBEEF)
        drv.check(st == 2, "block_info unknown rip -> INVALID_ARGUMENT")
        # wrong block output size
        bad = AevPeBlockInfo()
        bad.struct_size = ctypes.sizeof(AevPeBlockInfo) + 1
        err = drv.errbuf()
        drv.check(lib.aev_pe_session_block_info(
            session, rips[0], ctypes.byref(bad), err, ERR_CAP) == 4,
            "block_info rejects wrong struct_size")

        # ------------------------------------- stateful calls + 3-input oracle
        oracle = Oracle()
        oracle.reset(0x1234)
        st, r, _ = drv.call(session, "AevDllReset", (0x1234,))
        drv.check(st == 0 and r.status == 0 and r.rax_i32 == 0x1234 and
                  r.call_index == 1 and r.total_calls == 1,
                  "Reset(0x1234) in existing session")
        cases = [(11, 4, 8), (-9, 2, 17), (123456789, 7, 6)]
        for index, (a, b, n) in enumerate(cases, start=1):
            expected = oracle.compute(a, b, n)
            st, r, _ = drv.call(session, "AevDllCompute", (a, b, n))
            drv.check(st == 0 and r.status == 0 and r.rax_i32 == expected,
                      f"Compute case {index} result {r.rax_i32} == {expected}")
            drv.check(r.call_index == 3 * index - 1,
                      f"Compute case {index} call index")
            st, r, _ = drv.call(session, "AevDllState")
            drv.check(st == 0 and r.rax_i32 == oracle.state(),
                      f"State case {index}")
            st, r, _ = drv.call(session, "AevDllCalls")
            drv.check(st == 0 and r.rax_i32 == index,
                      f"Guest Calls case {index}")
        st, c, _ = drv.counts(session)
        drv.check(st == 0 and c.load_count == 1 and c.calls_total == 10 and
                  c.host_import_calls == 3 and c.jit_blocks_total > 0 and
                  c.jit_compiles_total > 0 and c.jit_execs_total > 0 and
                  c.aot_blocks_total == 0,
                  f"counts after 3 compute groups: {c.calls_total}/{c.host_import_calls}")
        drv.check(c.trace_records > 0, "trace records while open")

        # ---------------------------------------- 100 persistent same-session calls
        before_calls = c.calls_total
        before_host = c.host_import_calls
        all_ok = True
        for i in range(100):
            a = (i % 7) + 1
            b = i % 5
            n = i % 9
            expected = oracle.compute(a, b, n)
            st, r, _ = drv.call(session, "AevDllCompute", (a, b, n))
            if not (st == 0 and r.status == 0 and r.rax_i32 == expected):
                all_ok = False
                drv.check(False, f"100-call result {i}: {r.rax_i32} != {expected}")
                break
        drv.check(all_ok, "100 continuous Compute results match oracle")
        st, r, _ = drv.call(session, "AevDllCalls")
        drv.check(st == 0 and r.rax_i32 == 103, "DLL Calls == 103")
        st, r, _ = drv.call(session, "AevDllState")
        drv.check(st == 0 and r.rax_i32 == oracle.state(), "DLL State after 100")
        st, c, _ = drv.counts(session)
        drv.check(c.load_count == 1 and c.calls_total == before_calls + 102 and
                  c.host_import_calls == before_host + 100,
                  "100 calls: load_count stays 1, counts increase")
        drv.check(c.trace_records > 0, "trace still open during 100 calls")

        # --------------------------------------------- memory fault + recovery
        before_calls = c.calls_total
        before_host = c.host_import_calls
        st, r, error = drv.call(session, "AevDllReadAt", (0xDEAD0000,))
        drv.check(st == 6 and r.status == 6 and r.guest_error == 1,
                  f"invalid ReadAt -> MEMORY/6 ({st})")
        drv.check(r.mem_fault_kind == 1 and
                  r.mem_fault_addr == 0xDEAD0000 and
                  r.mem_fault_size == 4 and r.mem_access == 1,
                  f"memory fault fields: kind={r.mem_fault_kind} addr={r.mem_fault_addr:#x}")
        drv.check(r.error_pc != 0, "memory fault error_pc")
        drv.check(error != "", "memory fault error text")
        expected = oracle.compute(37, 5, 3)
        st, r, _ = drv.call(session, "AevDllCompute", (37, 5, 3))
        drv.check(st == 0 and r.rax_i32 == expected,
                  "Compute recovers after memory fault")
        st, c, _ = drv.counts(session)
        drv.check(c.calls_total == before_calls + 2 and
                  c.host_import_calls == before_host + 1,
                  "memory fault counts only entered Guest calls")

        # --------------------------------------------------------- observer
        observed = {"call_end": [], "host": [], "reentry_call": [],
                    "reentry_destroy": [], "reentry_trace": []}
        session_holder = [session]

        @ObsCallEnd
        def on_call_end(ctx, result_ptr):
            result = result_ptr.contents
            observed["call_end"].append((result.status, result.call_index))
            nested = AevPeCallResult()
            nested.struct_size = ctypes.sizeof(AevPeCallResult)
            args_arr = (ctypes.c_uint64 * 4)(0, 0, 0, 0)
            nested_err = ctypes.create_string_buffer(ERR_CAP)
            observed["reentry_call"].append(lib.aev_pe_session_call(
                session_holder[0], b"AevDllState", args_arr,
                ctypes.byref(nested), nested_err, ERR_CAP))
            observed["reentry_destroy"].append(
                lib.aev_pe_session_destroy(session_holder[0]))
            observed["reentry_trace"].append(
                lib.aev_pe_session_trace_close(session_holder[0], nested_err,
                                               ERR_CAP))

        @ObsHostImport
        def on_host_import(ctx, host_id, thunk_index):
            observed["host"].append((host_id, thunk_index))

        @ObsHostImport
        def ignore_host_import(ctx, host_id, thunk_index):
            pass

        obs = AevPeSessionObserver()
        obs.struct_size = ctypes.sizeof(AevPeSessionObserver)
        obs.abi_version = 1
        obs.on_call_end = on_call_end
        obs.on_host_import = on_host_import
        err = drv.errbuf()
        st = lib.aev_pe_session_set_observer(
            session, ctypes.byref(obs), None, err, ERR_CAP)
        drv.check(st == 0, f"set_observer OK ({drv.errtxt(err)})")
        expected = oracle.compute(5, 6, 2)
        st, r, _ = drv.call(session, "AevDllCompute", (5, 6, 2))
        drv.check(st == 0 and r.rax_i32 == expected, "observed Compute result")
        drv.check(len(observed["call_end"]) == 1 and
                  observed["call_end"][0][0] == 0,
                  "observer on_call_end invoked once with OK")
        drv.check(observed["host"] == [(1, 0)], "observer on_host_import id/index")
        drv.check(observed["reentry_call"] == [10],
                  "observer reentrant call -> REENTRY/10")
        drv.check(observed["reentry_destroy"] == [10],
                  "observer reentrant destroy -> REENTRY/10")
        drv.check(observed["reentry_trace"] == [10],
                  "observer reentrant trace_close -> REENTRY/10")
        st, c, _ = drv.counts(session)
        drv.check(st == 0 and c.load_count == 1, "session survives observer")

        # observer exceptions are caught by the ABI and do not change results.
        @ObsCallEnd
        def raising_observer(ctx, result_ptr):
            raise RuntimeError("observer must not escape")

        obs.on_call_end = raising_observer
        obs.on_host_import = ignore_host_import
        err = drv.errbuf()
        drv.check(lib.aev_pe_session_set_observer(
            session, ctypes.byref(obs), None, err, ERR_CAP) == 0,
            "set raising observer")
        expected = oracle.compute(3, 9, 1)
        st, r, _ = drv.call(session, "AevDllCompute", (3, 9, 1))
        drv.check(st == 0 and r.rax_i32 == expected,
                  "observer exception does not propagate")
        err = drv.errbuf()
        drv.check(lib.aev_pe_session_set_observer(
            session, None, None, err, ERR_CAP) == 0, "clear observer")
        st, r, _ = drv.call(session, "AevDllState")
        drv.check(st == 0 and r.rax_i32 == oracle.state(),
                  "cleared observer does not change execution")

        bad_obs = AevPeSessionObserver()
        bad_obs.struct_size = ctypes.sizeof(AevPeSessionObserver) + 1
        bad_obs.abi_version = 1
        err = drv.errbuf()
        drv.check(lib.aev_pe_session_set_observer(
            session, ctypes.byref(bad_obs), None, err, ERR_CAP) == 4,
            "observer struct_size validated")

        # ------------------------------------------------------ trace lifecycle
        st, error = drv.trace_open(session, drv.art / "should_fail.jsonl")
        drv.check(st == 2, f"duplicate trace_open -> INVALID_ARGUMENT ({st})")
        st, error = drv.trace_close(session)
        drv.check(st == 0, "trace_close OK")
        st, c, _ = drv.counts(session)
        records_before = c.trace_records
        drv.check(c.trace_open == 0 and records_before > 0,
                  "trace closed, records retained")
        st, _ = drv.trace_close(session)
        drv.check(st == 0, "trace_close idempotent")
        header = None
        events = []
        for line in trace_path.read_text().splitlines():
            if not line.strip():
                continue
            obj = json.loads(line)
            if obj.get("record") == "header":
                header = obj
            else:
                events.append(obj)
        drv.check(header is not None and header.get("trace_abi") == 4 and
                  header.get("scenario") == "session-test" and
                  header.get("module_kind") == "pe32+amd64-dll",
                  "trace header ABI 4/scenario/module kind")
        drv.check(header and header.get("input_sha256") ==
                  bytes(info.file_sha256).hex(), "trace header input sha")
        drv.check(header and header.get("aot_sha256") == "00" * 32,
                  "JIT-only trace has zero AOT sha")
        drv.check(header and len(header.get("all_blocks", [])) == 25 and
                  header.get("aot_selected") == [], "trace header block tables")
        drv.check(len(events) == records_before,
                  f"trace_records {records_before} == event lines {len(events)}")
        drv.check(any(e.get("record") == "jit_compile" for e in events),
                  "trace has actual jit_compile events")
        drv.check(any(e.get("record") == "host_import" and e.get("id") == 1 and
                      e.get("thunk_index") == 0 for e in events),
                  "trace has actual host_import events")
        drv.check(any(e.get("record") == "call_end" and e.get("call") == 1
                      for e in events), "trace call numbering starts at 1")
        drv.check(any(e.get("record") == "call_end" and e.get("status") == 6
                      for e in events), "trace records memory-fault call_end")

        reopen = drv.art / "session_reopen.jsonl"
        st, error = drv.trace_open(session, reopen, "reopen")
        drv.check(st == 0, f"trace reopen after close ({error})")
        st, c, _ = drv.counts(session)
        drv.check(c.trace_records == 0 and c.trace_open == 1,
                  "trace reopen resets records")
        drv.check(len(reopen.read_text().splitlines()) == 1,
                  "trace reopen truncates to header")
        st, r, _ = drv.call(session, "AevDllState")
        drv.check(st == 0, "call while trace reopened")
        st, _ = drv.trace_close(session)
        lines = [json.loads(x) for x in reopen.read_text().splitlines() if x.strip()]
        drv.check(len(lines) >= 2 and lines[0].get("record") == "header",
                  "trace reopen header + events")
        drv.check(any(e.get("record") == "call_end" and e.get("call") == 1
                      for e in lines[1:]), "trace reopen call numbering restarts")
        st, _ = drv.trace_open(session, "")
        drv.check(st == 2, "trace_open empty path -> INVALID_ARGUMENT")

        # ------------------------------------------------ wrong-thread diagnostics
        thread_status = {}

        def worker():
            tcounts = AevPeSessionCounts()
            tcounts.struct_size = ctypes.sizeof(AevPeSessionCounts)
            terr = ctypes.create_string_buffer(ERR_CAP)
            thread_status["counts"] = lib.aev_pe_session_counts(
                session, ctypes.byref(tcounts), terr, ERR_CAP)
            tout = AevPeCallResult()
            tout.struct_size = ctypes.sizeof(AevPeCallResult)
            targs = (ctypes.c_uint64 * 4)(0, 0, 0, 0)
            thread_status["call"] = lib.aev_pe_session_call(
                session, b"AevDllState", targs, ctypes.byref(tout), terr,
                ERR_CAP)
            thread_status["destroy"] = lib.aev_pe_session_destroy(session)

        thread = threading.Thread(target=worker)
        thread.start()
        thread.join()
        drv.check(thread_status.get("counts") == 13,
                  f"non-owner counts -> WRONG_THREAD ({thread_status})")
        drv.check(thread_status.get("call") == 13,
                  "non-owner call -> WRONG_THREAD")
        drv.check(thread_status.get("destroy") == 13,
                  "non-owner destroy -> WRONG_THREAD")
        st, r, _ = drv.call(session, "AevDllState")
        drv.check(st == 0, "owner still usable after wrong-thread attempts")

        # ---------------------------------------------- session isolation/lifecycle
        other_trace = drv.art / "session_other.jsonl"
        other = drv.require_session(trace=other_trace, scenario="other")
        other_oracle = Oracle()
        other_oracle.reset(0x777)
        st, r, _ = drv.call(other, "AevDllReset", (0x777,))
        drv.check(st == 0 and r.rax_i32 == 0x777, "second session Reset")
        expected = other_oracle.compute(8, 3, 4)
        st, r, _ = drv.call(other, "AevDllCompute", (8, 3, 4))
        drv.check(st == 0 and r.rax_i32 == expected, "second session Compute")
        st, r, _ = drv.call(session, "AevDllState")
        drv.check(st == 0 and r.rax_i32 == oracle.state(),
                  "first session state isolated")
        st, c_first, _ = drv.counts(session)
        st, c_other, _ = drv.counts(other)
        drv.check(c_first.load_count == 1 and c_other.load_count == 1 and
                  c_first.calls_total != c_other.calls_total,
                  "per-session counters isolated")
        drv.check(lib.aev_pe_session_destroy(other) == 0, "destroy second session")
        st, r, _ = drv.call(session, "AevDllState")
        drv.check(st == 0 and r.rax_i32 == oracle.state(),
                  "first session continues after second destroy")
        st, _, _ = drv.info(other)
        drv.check(st == 9, "stale info after destroy")
        drv.check(lib.aev_pe_session_destroy(other) == 9,
                  "double destroy -> STALE_HANDLE")
        drv.check(lib.aev_pe_session_destroy(0) == 9,
                  "handle 0 -> STALE_HANDLE")
        st, _, _ = drv.info(0)
        drv.check(st == 9, "handle 0 info -> STALE_HANDLE")

        # ------------------------------------------------------------- AOT session
        obj, aot_dylib, manifest = drv.translate_full("pe_session_all")
        aot_sha = hashlib.sha256(aot_dylib.read_bytes()).digest()
        aot = drv.require_session(aot=aot_dylib, entry="AevDllCompute")
        aot_oracle = Oracle()
        aot_oracle.reset(0x1234)
        st, ainfo, _ = drv.info(aot)
        drv.check(st == 0 and ainfo.aot_present == 1 and
                  ainfo.selected_count == manifest["aot_block_count"] and
                  ainfo.selected_count > 0, "AOT info selected table")
        drv.check(bytes(ainfo.aot_sha256) == aot_sha, "AOT info artifact sha")
        sel_count = ctypes.c_uint32()
        err = drv.errbuf()
        drv.check(lib.aev_pe_session_selected_count(
            aot, ctypes.byref(sel_count), err, ERR_CAP) == 0 and
            sel_count.value == ainfo.selected_count, "AOT selected_count")
        selected_rip = ctypes.c_uint64()
        err = drv.errbuf()
        drv.check(lib.aev_pe_session_selected_rip(
            aot, 0, ctypes.byref(selected_rip), err, ERR_CAP) == 0,
            "AOT selected_rip(0)")
        st, block, _ = drv.block_info(aot, selected_rip.value)
        drv.check(st == 0 and block.aot_selected == 1,
                  "AOT block_info selected flag")
        st, r, _ = drv.call(aot, "AevDllReset", (0x1234,))
        drv.check(st == 0 and r.rax_i32 == 0x1234, "AOT Reset")
        expected = aot_oracle.compute(11, 4, 8)
        st, r, _ = drv.call(aot, "AevDllCompute", (11, 4, 8))
        drv.check(st == 0 and r.rax_i32 == expected and r.aot_blocks >= 1,
                  "AOT Compute executes AOT blocks")
        st, ac_first, _ = drv.counts(aot)
        drv.check(ac_first.load_count == 1 and ac_first.aot_blocks_total >= 1,
                  "AOT cumulative counts")
        expected = aot_oracle.compute(11, 4, 8)
        st, r, _ = drv.call(aot, "AevDllCompute", (11, 4, 8))
        drv.check(st == 0 and r.rax_i32 == expected and r.aot_blocks >= 1,
                  "AOT Compute second input")
        st, ac_cache, _ = drv.counts(aot)
        drv.check(ac_cache.jit_compiles_total == ac_first.jit_compiles_total,
                  "AOT hybrid JIT fallback cache reuses compiled blocks")
        drv.check(ac_cache.jit_execs_total > ac_first.jit_execs_total,
                  "AOT hybrid JIT fallback actually executes")
        st, r, _ = drv.call(aot, "AevDllState")
        drv.check(st == 0 and r.rax_i32 == aot_oracle.state(), "AOT State")
        st, r, _ = drv.call(aot, "AevDllCalls")
        drv.check(st == 0 and r.rax_i32 == 2, "AOT Guest Calls")
        st, _, _ = drv.info(aot, size=ctypes.sizeof(AevPeModuleInfo) + 1)
        drv.check(st == 4, "AOT info rejects wrong struct_size")
        drv.check(lib.aev_pe_session_destroy(aot) == 0, "destroy AOT session")

        status, handle, error = drv.create(drv.config(
            aot=aot_dylib, load_base=NON_PREFERRED))
        drv.check(status == 4 and handle == 0 and "load_base" in error,
                  f"AOT wrong base -> ABI_MISMATCH ({status}: {error})")

        bad_runtime = drv.art / "aot_runtime_bad_abi.c"
        bad_runtime.write_text(
            (drv.source / "src" / "aot_runtime.c").read_text().replace(
                "const uint64_t aev_aot_abi_version = AEV_GUEST_ABI_VERSION;",
                "const uint64_t aev_aot_abi_version = 0;"))
        bad_obj = drv.art / "aot_runtime_bad_abi.o"
        proc = subprocess.run(
            [drv.clang, "--target=arm64-apple-macos14.0", "-O2", "-I",
             str(drv.source / "include"), "-c", str(bad_runtime),
             "-o", str(bad_obj)], text=True, capture_output=True)
        if proc.returncode != 0:
            raise Failure(f"build bad ABI runtime: {proc.stderr}")
        bad_dylib = drv.art / "libbad_abi_session.dylib"
        proc = subprocess.run(
            [drv.clang, "-arch", "arm64", "-dynamiclib", "-o", str(bad_dylib),
             str(obj), str(bad_obj), str(drv.mem_obj)],
            text=True, capture_output=True)
        if proc.returncode != 0:
            raise Failure(f"link bad ABI artifact: {proc.stderr}")
        status, handle, error = drv.create(drv.config(aot=bad_dylib))
        drv.check(status == 4 and handle == 0 and "ABI" in error,
                  f"AOT bad ABI -> ABI_MISMATCH ({status}: {error})")

        # ------------------------------------------------------ error truncation
        tiny = drv.create(drv.config(dll=truncated), cap=8)
        status, handle, error = tiny
        drv.check(status == 3 and handle == 0 and len(error) <= 7,
                  f"error buffer truncates at NUL ({error!r})")

        # Final cleanup and load_count proof.
        st, final_counts, _ = drv.counts(session)
        drv.check(st == 0 and final_counts.load_count == 1,
                  "main session load_count never grows")
        drv.check(lib.aev_pe_session_destroy(session) == 0,
                  "destroy main session")
        st, final_counts, _ = drv.counts(session)
        drv.check(st == 9, "stale counts after main destroy")

        total = len(drv.checks)
        passed = total - drv.failures
        print(f"PE_SESSION_CHECKS {passed}/{total} passed, {drv.failures} failed")
        (drv.results / "checks.json").write_text(json.dumps(
            [{"ok": ok, "message": msg} for ok, msg in drv.checks], indent=2))
        (drv.results / "hashes.txt").write_text(
            f"libaev_pe_session.dylib "
            f"{hashlib.sha256(drv.lib_path.read_bytes()).hexdigest()}\n"
            f"aevguest.dll "
            f"{hashlib.sha256((drv.pe_dir / 'aevguest.dll').read_bytes()).hexdigest()}\n")
        return 0 if drv.failures == 0 else 1
    except Failure as exc:
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
