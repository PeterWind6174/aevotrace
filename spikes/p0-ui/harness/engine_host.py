"""Minimal shared host for the engine-backed compat/native GUI paths.

The host creates one window/edit/button/result chain, writes the UTF-16 text
into checked Guest memory, calls the same fixed COFF fixture through
libaev_bridge.dylib, and applies the HostImport pending result only after the
bridge returns OK.  It imports no Unicorn/pefile/guest_pe.
"""
from __future__ import annotations

import os
import struct
import subprocess
from dataclasses import dataclass, field

from AppKit import NSThread

from .engine_bridge import (
    AEV_BRIDGE_IMPORT_BASE,
    AEV_BRIDGE_IMPORT_STRIDE,
    AEV_BRIDGE_OK,
    BridgeError,
    EngineBridge,
)
from .errors import UnsupportedApi

ENTRY_ADDR = 0x140001000
IMPORT_UPDATE = AEV_BRIDGE_IMPORT_BASE + 1 * AEV_BRIDGE_IMPORT_STRIDE
IMPORT_REENTRY = AEV_BRIDGE_IMPORT_BASE + 2 * AEV_BRIDGE_IMPORT_STRIDE
UNKNOWN_IMPORT = AEV_BRIDGE_IMPORT_BASE + 0x0F * AEV_BRIDGE_IMPORT_STRIDE
BAD_ADDRESS = 0xDEAD0000

IDC_BTN_COMPUTE = 1001
IDC_EDIT_INPUT = 1002
IDC_RESULT = 1003
WM_CLOSE = 0x0010

TEXT_OFFSET = 0x1000
CTX_OFFSET = 0x3000
COUNTER_OFFSET = 0x4000
TEXT_CAP_UNITS = 2048


@dataclass
class WindowInfo:
    handle: int
    kind: str  # main/static/edit/button
    title: str
    parent: int = 0
    control_id: int = 0
    style: int = 0
    visible: bool = False
    destroyed: bool = False
    extra: dict = field(default_factory=dict)


class EngineGuiHost:
    def __init__(self, backend, logger, evidence_dir: str, bridge_lib: str,
                 coff: str, aot: str):
        self.backend = backend
        self.logger = logger
        self.evidence_dir = evidence_dir
        self.bridge_lib = bridge_lib
        self.coff = coff
        self.aot = aot
        self.windows: dict[int, WindowInfo] = {}
        self.main_hwnd = 0
        self.edit_handle = 0
        self.button_handle = 0
        self.result_handle = 0
        self.bridge: EngineBridge | None = None
        self.base = 0
        self.size = 0
        self.stack_base = 0
        self.stack_top = 0
        self.helper_calls = 0
        self.last_helper: tuple[int, int, int, int] | None = None
        self._pending_text: str | None = None
        self.result_text = "等待 Guest 回调"
        self.last_result = None
        self.last_status = None
        self.last_error = ""
        self.reentry_status = None
        self.reentry_outer_status = None
        self.button_op = 2
        self.closed = False
        self.created = False

    # ------------------------------------------------------------------
    def start(self) -> int:
        self.backend.create_main_window(
            0x1000, "Aevotrace P0 回调集成", 240, 280, 560, 210
        )
        self.main_hwnd = 0x1000
        self.windows[self.main_hwnd] = WindowInfo(
            self.main_hwnd, "main", "Aevotrace P0 回调集成", visible=False
        )
        self.edit_handle = 0x1001
        self.button_handle = 0x1002
        self.result_handle = 0x1003
        self.windows[self.edit_handle] = WindowInfo(
            self.edit_handle, "edit", "Aevoβ中", parent=self.main_hwnd,
            control_id=IDC_EDIT_INPUT,
        )
        self.windows[self.button_handle] = WindowInfo(
            self.button_handle, "button", "回调计算", parent=self.main_hwnd,
            control_id=IDC_BTN_COMPUTE,
        )
        self.windows[self.result_handle] = WindowInfo(
            self.result_handle, "static", self.result_text, parent=self.main_hwnd,
            control_id=IDC_RESULT,
        )
        self.backend.create_control(
            self.edit_handle, "edit", "Aevoβ中", 0, 24, 30, 360, 30,
            IDC_EDIT_INPUT,
        )
        self.backend.create_control(
            self.button_handle, "button", "回调计算", 0, 24, 78, 140, 32,
            IDC_BTN_COMPUTE,
        )
        self.backend.create_control(
            self.result_handle, "static", self.result_text, 0, 24, 130, 500, 26,
            IDC_RESULT,
        )
        self.backend.show_window(self.main_hwnd)
        self.windows[self.main_hwnd].visible = True

        self.bridge = EngineBridge(self.bridge_lib)
        self.bridge.create(self.coff, self.aot, ENTRY_ADDR, trace_aot=True)
        info = self.bridge.guest_info()
        self.base = info.base
        self.size = info.size
        self.stack_base = info.stack_base
        self.stack_top = info.stack_top
        if self.base + TEXT_OFFSET + TEXT_CAP_UNITS * 2 >= self.stack_base:
            raise UnsupportedApi("guest data area overlaps the guest stack")
        self._write_ctx(IMPORT_UPDATE, self.base + COUNTER_OFFSET)
        self._write_counter(0)
        self.bridge.register_import(IMPORT_UPDATE, 1, self._host_update)
        self.bridge.register_import(IMPORT_REENTRY, 2, self._host_reentry)
        self.created = True
        self.logger.event(
            "engine.host_started", base=f"0x{self.base:x}",
            size=self.size, stack_base=f"0x{self.stack_base:x}",
            stack_top=f"0x{self.stack_top:x}", entry=f"0x{ENTRY_ADDR:x}",
            import_update=f"0x{IMPORT_UPDATE:x}",
            main_thread=bool(NSThread.isMainThread()),
        )
        return self.main_hwnd

    # ------------------------------------------------------------------
    def _write_ctx(self, update_addr: int, counter_addr: int) -> None:
        assert self.bridge is not None
        self.bridge.guest_write(
            self.base + CTX_OFFSET, struct.pack("<QQ", update_addr, counter_addr)
        )

    def _write_counter(self, value: int) -> None:
        assert self.bridge is not None
        self.bridge.guest_write(
            self.base + COUNTER_OFFSET, struct.pack("<Q", value & 0xFFFFFFFFFFFFFFFF)
        )

    def _read_counter(self) -> int:
        assert self.bridge is not None
        return struct.unpack("<Q", self.bridge.guest_read(self.base + COUNTER_OFFSET, 8))[0]

    def _write_text(self, text: str) -> int:
        assert self.bridge is not None
        data = text.encode("utf-16-le")
        units = len(data) // 2
        if units > TEXT_CAP_UNITS:
            raise UnsupportedApi(f"edit text too long: {units} UTF-16 units")
        self.bridge.guest_write(self.base + TEXT_OFFSET, data)
        return units

    def _host_update(self, a0: int, a1: int, a2: int, a3: int) -> int:
        self.helper_calls += 1
        self.last_helper = (a0, a1, a2, a3)
        self._pending_text = f"guest hash=0x{a0:08X} len={a1} op={a2}"
        self.logger.event(
            "engine.host_import", hash=f"0x{a0:08x}", length=a1, op=a2,
            a3=f"0x{a3:x}", pending=self._pending_text,
        )
        return a2  # frozen helper contract

    def _host_reentry(self, a0: int, a1: int, a2: int, a3: int) -> int:
        assert self.bridge is not None
        nested = self.bridge.call(ENTRY_ADDR, [0, 0, 0, 0], raise_on_error=False)
        self.reentry_status = nested.status
        self.logger.event(
            "engine.reentry_probe", status=nested.status,
            status_name=nested.status_name if hasattr(nested, "status_name") else nested.status,
        )
        self._pending_text = f"reentry rejected status={nested.status}"
        return a2

    # ------------------------------------------------------------------
    def _call_guest(self, op: int, text: str | None = None,
                    update_addr: int = IMPORT_UPDATE,
                    counter_addr: int | None = None,
                    text_addr: int | None = None,
                    len_units: int | None = None,
                    raise_on_error: bool = False):
        assert self.bridge is not None
        if text is not None:
            units = self._write_text(text)
        else:
            units = len_units if len_units is not None else 0
        if text_addr is None:
            text_addr = self.base + TEXT_OFFSET
        if counter_addr is None:
            counter_addr = self.base + COUNTER_OFFSET
        self._write_ctx(update_addr, counter_addr)
        self._pending_text = None
        result = self.bridge.call(
            ENTRY_ADDR, [op, text_addr, units, self.base + CTX_OFFSET],
            raise_on_error=raise_on_error,
        )
        self.last_result = result
        self.last_status = result.status
        if result.status != AEV_BRIDGE_OK:
            self.last_error = f"status={result.status} error_pc=0x{result.error_pc:x}"
            self.logger.event("engine.guest_error", status=result.status,
                              error_pc=f"0x{result.error_pc:x}",
                              guest_error=result.guest_error)
        return result

    def dispatch_command(self, control_id: int, notification: int, source: str) -> int:
        if control_id not in (IDC_BTN_COMPUTE, IDC_EDIT_INPUT):
            raise UnsupportedApi(f"engine host: unknown control id {control_id}")
        text = self.backend.get_text(self.edit_handle)
        op = self.button_op if control_id == IDC_BTN_COMPUTE else 1
        self.logger.event("engine.guest_enter", control_id=control_id,
                          op=op, text=text, source=source,
                          main_thread=bool(NSThread.isMainThread()))
        try:
            result = self._call_guest(op, text=text, raise_on_error=False)
        except Exception as exc:  # noqa: BLE001 - report, do not update UI
            self.last_error = str(exc)
            self.logger.event("engine.guest_exception", error=str(exc))
            return 0
        if result.status != AEV_BRIDGE_OK:
            # Failure must not commit a result-control update.
            self.logger.event("engine.result_not_committed", status=result.status)
            return 0
        if self._pending_text is None:
            self.last_error = "bridge returned OK without a host pending result"
            self.logger.event("engine.pending_missing")
            return 0
        self.result_text = self._pending_text
        self.backend.set_text(self.result_handle, self.result_text)
        self.logger.event("engine.result_committed", text=self.result_text,
                          guest_rax=f"0x{result.guest_rax:x}",
                          call_index=result.call_index,
                          aot_blocks=result.aot_blocks,
                          jit_compiles=result.jit_compiles,
                          jit_execs=result.jit_execs)
        return result.guest_rax

    def probe_unknown_import(self) -> int:
        result = self._call_guest(3, text="unknown-import", update_addr=UNKNOWN_IMPORT)
        self._write_ctx(IMPORT_UPDATE, self.base + COUNTER_OFFSET)
        return result.status

    def probe_invalid_memory(self) -> int:
        result = self._call_guest(3, text=None, len_units=4,
                                  text_addr=BAD_ADDRESS)
        return result.status

    def probe_reentry(self) -> int:
        self.reentry_status = None
        result = self._call_guest(3, text="reentry", update_addr=IMPORT_REENTRY)
        self.reentry_outer_status = result.status
        self._write_ctx(IMPORT_UPDATE, self.base + COUNTER_OFFSET)
        return result.status

    def probe_helper_then_fault(self) -> tuple[int, bool]:
        before = self.helper_calls
        result = self._call_guest(3, text="helper-fault",
                                  counter_addr=BAD_ADDRESS)
        self._write_ctx(IMPORT_UPDATE, self.base + COUNTER_OFFSET)
        return result.status, self.helper_calls > before

    # ------------------------------------------------------------------
    def drain_posted_messages(self) -> int:
        return 0

    def dispatch_menu(self, item_id: int, source: str) -> int:
        raise UnsupportedApi(f"engine host has no menu (item {item_id})")

    def send_sync(self, hwnd: int, msg: int, wparam: int, lparam: int,
                  reason: str = "") -> int:
        if msg == WM_CLOSE:
            self.close(reason=reason)
            return 0
        raise UnsupportedApi(f"engine host send_sync does not support msg=0x{msg:x}")

    def read_state(self) -> dict:
        counter = self._read_counter() if self.bridge else 0
        return {
            "counter": counter,
            "helper_calls": self.helper_calls,
            "result_text": self.result_text,
            "last_status": self.last_status,
            "last_error": self.last_error,
            "reentry_status": self.reentry_status,
            "reentry_outer_status": self.reentry_outer_status,
            "closed": self.closed,
            "bridge": None if self.last_result is None else {
                "call_index": self.last_result.call_index,
                "guest_rax": self.last_result.guest_rax,
                "aot_blocks": self.last_result.aot_blocks,
                "jit_blocks": self.last_result.jit_blocks,
                "jit_compiles": self.last_result.jit_compiles,
                "jit_execs": self.last_result.jit_execs,
                "total_calls": self.last_result.total_calls,
                "total_jit_compiles": self.last_result.total_jit_compiles,
                "total_jit_execs": self.last_result.total_jit_execs,
            },
        }

    def screenshot(self, name: str) -> str | None:
        number = self.backend.window_number(self.main_hwnd)
        if number is None:
            return None
        path = os.path.join(self.evidence_dir, name)
        os.makedirs(self.evidence_dir, exist_ok=True)
        try:
            subprocess.check_output(
                ["screencapture", "-x", "-o", f"-l{number}", path],
                stderr=subprocess.STDOUT,
            )
        except Exception as exc:  # noqa: BLE001
            self.logger.event("engine.screenshot_error", error=str(exc), name=name)
            return None
        self.logger.event("engine.screenshot", path=path, window_number=number)
        return path

    def close(self, reason: str = "") -> None:
        if self.closed:
            return
        self.logger.event("engine.host_close", reason=reason)
        if self.bridge is not None:
            try:
                self.bridge.destroy()
            except BridgeError as exc:
                self.logger.event("engine.bridge_destroy_error", error=str(exc))
            self.bridge = None
        self.backend.close_window(self.main_hwnd)
        self.closed = True

    def bridge_proof(self) -> dict:
        return {
            "library": self.bridge_lib,
            "library_sha256": _sha256(self.bridge_lib),
            "coff": self.coff,
            "coff_sha256": _sha256(self.coff),
            "aot": self.aot,
            "aot_sha256": _sha256(self.aot),
            "entry": f"0x{ENTRY_ADDR:x}",
            "guest_base": f"0x{self.base:x}",
            "guest_size": self.size,
            "stack_base": f"0x{self.stack_base:x}",
            "stack_top": f"0x{self.stack_top:x}",
            "helper_calls": self.helper_calls,
            "unicorn_loaded": "unicorn" in __import__("sys").modules,
            "guest_pe_loaded": "harness.guest_pe" in __import__("sys").modules,
        }


def _sha256(path: str) -> str:
    import hashlib

    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()
