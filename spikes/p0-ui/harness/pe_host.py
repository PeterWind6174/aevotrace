"""One persistent PE session per compat/native window (P0-PE-GUI-01).

The window is created on the AppKit main thread and owns exactly one
``libaev_pe_session.dylib`` session for its whole lifetime.  Buttons call the
real DLL exports through the in-process session; the Host performs only input
parsing, dispatch, formatting and display.  It never starts ``aev-pe-runner``
and never parses runner JSON.

Business display rules (contract §8):
  * successful Compute commits ``rax_i32``, the real ``AevDllState`` and the
    real ``AevDllCalls``;
  * a failed PE call keeps the previous valid business result/state/calls and
    shows the error separately;
  * Reset is an explicit ``AevDllReset`` call, not a session reload;
  * the negative button calls the frozen ``AevDllReadAt(0xDEAD0000)`` export
    and must produce a MEMORY error without committing a result.
"""
from __future__ import annotations

import hashlib
import os
import re
import subprocess
import threading
from dataclasses import dataclass, field
from typing import Optional

from AppKit import NSThread

from .errors import UnsupportedApi
from .pe_session import (
    AEV_PE_SESSION_OK,
    PeSession,
    PeSessionError,
)

IDC_EDIT_SEED = 1101
IDC_EDIT_A = 1102
IDC_EDIT_B = 1103
IDC_EDIT_N = 1104
IDC_BTN_COMPUTE = 1105
IDC_BTN_RESET = 1106
IDC_RESULT = 1107
IDC_STATE = 1108
IDC_CALLS = 1109
IDC_STATUS = 1110
IDC_LOAD = 1111
IDC_EDIT_BAD_ADDR = 1112
IDC_BTN_READ_BAD = 1113
IDC_HINT = 1114

WM_CLOSE = 0x0010
MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF

_INT_RE = re.compile(r"^[+-]?(?:0[xX][0-9a-fA-F]+|\d+)$")


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


def parse_i32(text: str, field_name: str) -> int:
    value = parse_int(text, field_name)
    if value < -(1 << 31) or value > (1 << 31) - 1:
        raise ValueError(
            f"{field_name} 超出 int32 范围 [-2147483648, 2147483647]: {text!r}"
        )
    return value


def parse_u64(text: str, field_name: str) -> int:
    value = parse_int(text, field_name, signed=False)
    if value < 0 or value > MASK64:
        raise ValueError(f"{field_name} 超出 uint64 范围: {text!r}")
    return value


def parse_int(text: str, field_name: str, signed: bool = True) -> int:
    raw = (text or "").strip()
    if not raw:
        raise ValueError(f"{field_name} 不能为空")
    if not _INT_RE.match(raw):
        raise ValueError(f"{field_name} 不是合法整数: {text!r}")
    try:
        value = int(raw, 0)
    except ValueError:
        # int(..., 0) rejects leading-zero decimals in Python 3; fall back to
        # plain decimal for strings such as "010".
        try:
            value = int(raw, 10)
        except ValueError as exc:
            raise ValueError(f"{field_name} 不是合法整数: {text!r}") from exc
    if signed and (value < -(1 << 63) or value > (1 << 63) - 1):
        raise ValueError(f"{field_name} 超出 64 位有符号范围: {text!r}")
    return value


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


class PeGuiHost:
    """Owns the window controls and the single live PE session."""

    name = "pe-session-gui"

    def __init__(self, backend, logger, evidence_dir: str,
                 session_lib: str, dll: str, registry: str,
                 aot: Optional[str], load_base: int,
                 entry_export: Optional[str] = "AevDllCompute",
                 trace_path: Optional[str] = None):
        self.backend = backend
        self.logger = logger
        self.evidence_dir = evidence_dir
        self.session_lib = os.path.abspath(session_lib)
        self.dll = os.path.abspath(dll)
        self.registry = os.path.abspath(registry)
        self.aot = os.path.abspath(aot) if aot else None
        self.load_base = int(load_base)
        self.entry_export = entry_export
        self.trace_path = os.path.abspath(trace_path) if trace_path else None

        self.windows: dict[int, WindowInfo] = {}
        self.main_hwnd = 0
        self.seed_handle = 0
        self.a_handle = 0
        self.b_handle = 0
        self.n_handle = 0
        self.compute_handle = 0
        self.reset_handle = 0
        self.result_handle = 0
        self.state_handle = 0
        self.calls_handle = 0
        self.status_handle = 0
        self.load_handle = 0
        self.bad_addr_handle = 0
        self.bad_button_handle = 0
        self.hint_handle = 0

        self.session: Optional[PeSession] = None
        self.session_info: dict = {}
        self.session_handle: int = 0
        self.create_count = 0
        self.created_on_main_thread = False
        self.created = False
        self.closed = False
        self.trace_closed = False

        self.last_result: Optional[dict] = None
        self.last_status: Optional[int] = None
        self.last_status_name: str = ""
        self.last_error: str = ""
        self.operation_count = 0

    # -- setup ---------------------------------------------------------
    def start(self) -> int:
        self.created_on_main_thread = bool(NSThread.isMainThread())
        self.main_hwnd = 0x3000
        self._create_window()
        self._create_controls()
        self.backend.show_window(self.main_hwnd)
        self.windows[self.main_hwnd].visible = True
        self.create_count += 1
        self._create_session()
        self._refresh_initial()
        self.created = True
        self.logger.event(
            "pe.host_started",
            session_handle=f"0x{self.session_handle:x}",
            main_thread=bool(NSThread.isMainThread()),
            created_on_main_thread=self.created_on_main_thread,
            load_count=self._counts().get("load_count"),
            module_kind=self.session_info.get("module_kind"),
            entry_export=self.session_info.get("entry_export"),
        )
        return self.main_hwnd

    def _create_window(self) -> None:
        title = f"Aevotrace P0 PE session [{self.backend.name}]"
        self.backend.create_main_window(
            self.main_hwnd, title, 200, 220, 820, 500
        )
        self.windows[self.main_hwnd] = WindowInfo(
            self.main_hwnd, "main", title, visible=False
        )

    def _add_control(self, handle: int, kind: str, text: str,
                     x: int, y: int, width: int, height: int,
                     control_id: int) -> None:
        self.windows[handle] = WindowInfo(
            handle, kind, text, parent=self.main_hwnd,
            control_id=control_id,
        )
        self.backend.create_control(
            handle, kind, text, 0, x, y, width, height, control_id
        )

    def _create_controls(self) -> None:
        self.seed_handle = 0x3001
        self.a_handle = 0x3002
        self.b_handle = 0x3003
        self.n_handle = 0x3004
        self.compute_handle = 0x3005
        self.reset_handle = 0x3006
        self.result_handle = 0x3007
        self.state_handle = 0x3008
        self.calls_handle = 0x3009
        self.status_handle = 0x300A
        self.load_handle = 0x300B
        self.bad_addr_handle = 0x300C
        self.bad_button_handle = 0x300D
        self.hint_handle = 0x300E

        self._add_control(0x3101, "static", "seed", 24, 24, 76, 24, 2101)
        self._add_control(self.seed_handle, "edit", "0x1234", 104, 18, 130, 30,
                          IDC_EDIT_SEED)
        self._add_control(0x3102, "static", "a", 254, 24, 24, 24, 2102)
        self._add_control(self.a_handle, "edit", "11", 280, 18, 100, 30,
                          IDC_EDIT_A)
        self._add_control(0x3103, "static", "b", 398, 24, 24, 24, 2103)
        self._add_control(self.b_handle, "edit", "4", 424, 18, 100, 30,
                          IDC_EDIT_B)
        self._add_control(0x3104, "static", "n", 542, 24, 24, 24, 2104)
        self._add_control(self.n_handle, "edit", "8", 568, 18, 100, 30,
                          IDC_EDIT_N)

        self._add_control(self.compute_handle, "button", "Compute",
                          24, 64, 160, 34, IDC_BTN_COMPUTE)
        self._add_control(self.reset_handle, "button", "Reset",
                          200, 64, 160, 34, IDC_BTN_RESET)

        self._add_control(0x3105, "static", "结果", 24, 120, 70, 24, 2105)
        self._add_control(self.result_handle, "static", "—", 100, 112, 680, 36,
                          IDC_RESULT)
        self._add_control(0x3106, "static", "Guest State", 24, 162, 110, 24,
                          2106)
        self._add_control(self.state_handle, "static", "0", 140, 156, 260, 30,
                          IDC_STATE)
        self._add_control(0x3107, "static", "Guest Calls", 24, 196, 110, 24,
                          2107)
        self._add_control(self.calls_handle, "static", "0", 140, 190, 260, 30,
                          IDC_CALLS)
        self._add_control(0x3108, "static", "load_count", 24, 230, 110, 24,
                          2108)
        self._add_control(self.load_handle, "static", "load_count=—",
                          140, 224, 600, 30, IDC_LOAD)
        self._add_control(0x3109, "static", "错误/状态", 24, 270, 80, 24, 2109)
        self._add_control(self.status_handle, "static", "准备创建 session",
                          110, 264, 660, 48, IDC_STATUS)

        self._add_control(0x310A, "static",
                          "AevDllReadAt 负例地址（测试专用）",
                          24, 330, 220, 24, 2110)
        self._add_control(self.bad_addr_handle, "edit", "0xDEAD0000",
                          250, 324, 150, 30, IDC_EDIT_BAD_ADDR)
        self._add_control(self.bad_button_handle, "button",
                          "读取非法地址（负例）", 416, 320, 230, 34,
                          IDC_BTN_READ_BAD)
        self._add_control(self.hint_handle, "static",
                          "负例参数 0xDEAD0000 仅用于测试 MEMORY 错误；"
                          "失败不提交结果，随后正常 Compute 恢复。",
                          24, 372, 760, 40, IDC_HINT)

    def _control_text(self, handle: int) -> str:
        return str(self.backend.get_text(handle))

    def _create_session(self) -> None:
        if not os.path.isfile(self.session_lib):
            raise FileNotFoundError(f"session library not found: {self.session_lib}")
        if not os.path.isfile(self.dll):
            raise FileNotFoundError(f"DLL not found: {self.dll}")
        if not os.path.isfile(self.registry):
            raise FileNotFoundError(f"registry not found: {self.registry}")
        if self.aot and not os.path.isfile(self.aot):
            raise FileNotFoundError(f"AOT dylib not found: {self.aot}")
        self.session = PeSession(self.session_lib)
        self.session_handle = self.session.create(
            dll_path=self.dll,
            registry_path=self.registry,
            aot_path=self.aot,
            entry_export=self.entry_export,
            load_base=self.load_base,
            trace_path=self.trace_path,
            scenario=f"pe-gui-{self.backend.name}",
        )
        self.session_info = self.session.info()
        if int(self.session_info.get("load_count", 0)) != 1:
            raise UnsupportedApi(
                "session info load_count is not 1 after create: "
                f"{self.session_info.get('load_count')}"
            )
        if self.session_info.get("module_kind") != "pe32+amd64-dll":
            raise UnsupportedApi(
                f"unexpected module kind {self.session_info.get('module_kind')!r}"
            )
        self.logger.event(
            "pe.session_created",
            session_handle=f"0x{self.session_handle:x}",
            dll=self.dll,
            dll_sha256=sha256_file(self.dll),
            registry=self.registry,
            aot=self.aot,
            aot_sha256=sha256_file(self.aot) if self.aot else None,
            load_base=f"0x{self.load_base:x}",
            trace_path=self.trace_path,
            info=self.session_info,
            abi=self.session.abi,
        )

    def _refresh_initial(self) -> None:
        assert self.session is not None
        self._update_load_display()
        state = self.session.call("AevDllState")
        calls = self.session.call("AevDllCalls")
        self._set_text(self.state_handle, str(state["rax_i32"]))
        self._set_text(self.calls_handle, str(calls["rax_i32"]))
        self._set_text(
            self.status_handle,
            "OK: 持久 PE session 已创建（AppKit 主线程），等待操作",
        )
        self.last_status = AEV_PE_SESSION_OK
        self.last_status_name = "OK"
        self.last_error = ""
        self._update_load_display()
        self.logger.event(
            "pe.initial_state",
            state=state["rax_i32"],
            calls=calls["rax_i32"],
            counts=self._counts(),
            main_thread=bool(NSThread.isMainThread()),
        )

    # -- display -------------------------------------------------------
    def _set_text(self, handle: int, text: str) -> None:
        self.backend.set_text(handle, text)

    def _set_result(self, result: dict) -> None:
        self._set_text(
            self.result_handle,
            f"{result['rax_i32']} (rax=0x{result['rax']:016x})",
        )

    def _set_state_calls(self, state: int, calls: int) -> None:
        self._set_text(self.state_handle, str(int(state)))
        self._set_text(self.calls_handle, str(int(calls)))

    def _set_status_ok(self, operation: str, result: dict) -> None:
        self._set_text(
            self.status_handle,
            f"OK: {operation} call_index={result['call_index']} "
            f"steps={result['steps']} aot={result['aot_blocks']} "
            f"jit_compile={result['jit_compiles']} jit_exec={result['jit_execs']}",
        )

    def _update_load_display(self) -> None:
        counts = self._counts()
        self._set_text(self.load_handle, f"load_count={counts.get('load_count', '?')}")

    def _counts(self) -> dict:
        if self.session is None or not self.session.live:
            return {}
        try:
            return self.session.counts()
        except PeSessionError as exc:
            self.logger.event("pe.counts_error", error=str(exc), status=exc.status)
            return {}

    # -- operations ----------------------------------------------------
    def _compute(self, source: str) -> None:
        assert self.session is not None
        try:
            a = parse_i32(self._control_text(self.a_handle), "a")
            b = parse_i32(self._control_text(self.b_handle), "b")
            n = parse_i32(self._control_text(self.n_handle), "n")
        except ValueError as exc:
            self._host_input_error(str(exc))
            return
        self.operation_count += 1
        self.logger.event(
            "pe.guest_enter", export="AevDllCompute", a=a, b=b, n=n,
            source=source, main_thread=bool(NSThread.isMainThread()),
        )
        try:
            result = self.session.call(
                "AevDllCompute", [a & MASK32, b & MASK32, n & MASK32]
            )
            # State/Calls come from real DLL exports, not Host mirrors.
            state = self.session.call("AevDllState")
            calls = self.session.call("AevDllCalls")
        except PeSessionError as exc:
            self._record_failure("AevDllCompute", exc)
            return
        self._set_result(result)
        self._set_state_calls(state["rax_i32"], calls["rax_i32"])
        self._set_status_ok("AevDllCompute", result)
        self._update_load_display()
        self.last_result = result
        self.last_status = AEV_PE_SESSION_OK
        self.last_status_name = "OK"
        self.last_error = ""
        self.logger.event(
            "pe.result_committed",
            export="AevDllCompute",
            args=[a, b, n],
            rax=result["rax"],
            rax_i32=result["rax_i32"],
            state=state["rax_i32"],
            calls=calls["rax_i32"],
            call_index=result["call_index"],
            counts=self._counts(),
        )

    def _reset(self, source: str) -> None:
        assert self.session is not None
        try:
            seed = parse_i32(self._control_text(self.seed_handle), "seed")
        except ValueError as exc:
            self._host_input_error(str(exc))
            return
        self.operation_count += 1
        self.logger.event(
            "pe.guest_enter", export="AevDllReset", seed=seed,
            source=source, main_thread=bool(NSThread.isMainThread()),
        )
        try:
            result = self.session.call("AevDllReset", [seed & MASK32])
            state = self.session.call("AevDllState")
            calls = self.session.call("AevDllCalls")
        except PeSessionError as exc:
            self._record_failure("AevDllReset", exc)
            return
        self._set_result(result)
        self._set_state_calls(state["rax_i32"], calls["rax_i32"])
        self._set_status_ok(f"AevDllReset(seed={seed})", result)
        self._update_load_display()
        self.last_result = result
        self.last_status = AEV_PE_SESSION_OK
        self.last_status_name = "OK"
        self.last_error = ""
        self.logger.event(
            "pe.reset_committed",
            seed=seed,
            rax_i32=result["rax_i32"],
            state=state["rax_i32"],
            calls=calls["rax_i32"],
            counts=self._counts(),
        )

    def _read_bad(self, source: str) -> None:
        assert self.session is not None
        try:
            address = parse_u64(self._control_text(self.bad_addr_handle),
                                "负例地址")
        except ValueError as exc:
            self._host_input_error(str(exc))
            return
        self.operation_count += 1
        self.logger.event(
            "pe.negative_read", export="AevDllReadAt", address=f"0x{address:x}",
            test_only=True, source=source,
            note="0xDEAD0000 is a test-only invalid Guest address",
            main_thread=bool(NSThread.isMainThread()),
        )
        try:
            result = self.session.call("AevDllReadAt", [address])
        except PeSessionError as exc:
            self._record_failure("AevDllReadAt", exc)
            self.logger.event(
                "pe.negative_result", address=f"0x{address:x}",
                status=exc.status, status_name=exc.status_name,
                result_not_committed=True,
            )
            return
        # Reaching this path means the loader accepted an address that the
        # scenario expects to fault.  Surface it; do not commit business state.
        self.last_status = AEV_PE_SESSION_OK
        self.last_status_name = "UNEXPECTED_OK"
        self.last_error = (
            f"AevDllReadAt(0x{address:x}) unexpectedly succeeded "
            f"with rax_i32={result['rax_i32']}"
        )
        self._set_text(self.status_handle, f"UNEXPECTED OK: {self.last_error}")
        self.logger.event(
            "pe.negative_unexpected_ok", address=f"0x{address:x}",
            result=result, result_not_committed=True,
        )

    def _record_failure(self, operation: str, exc: PeSessionError) -> None:
        self.last_status = int(exc.status)
        self.last_status_name = exc.status_name
        self.last_error = str(exc)
        self._set_text(
            self.status_handle,
            f"ERROR {exc}",
        )
        self.logger.event(
            "pe.call_failed", operation=operation, status=exc.status,
            status_name=exc.status_name, error=str(exc),
            result=exc.result,
            previous_result_kept=True,
        )

    def _host_input_error(self, message: str) -> None:
        self.last_status = None
        self.last_status_name = "HOST_INPUT"
        self.last_error = message
        self._set_text(self.status_handle, f"ERROR HOST_INPUT: {message}")
        self.logger.event(
            "pe.input_rejected", error=message, entered_guest=False,
        )

    # -- backend callbacks --------------------------------------------
    def dispatch_command(self, control_id: int, notification: int,
                         source: str) -> int:
        if control_id == IDC_BTN_COMPUTE:
            self._compute(source)
            return 0
        if control_id == IDC_BTN_RESET:
            self._reset(source)
            return 0
        if control_id == IDC_BTN_READ_BAD:
            self._read_bad(source)
            return 0
        if control_id in (IDC_EDIT_SEED, IDC_EDIT_A, IDC_EDIT_B, IDC_EDIT_N,
                          IDC_EDIT_BAD_ADDR):
            self.logger.event(
                "pe.edit_changed", control_id=control_id, source=source,
                main_thread=bool(NSThread.isMainThread()),
            )
            return 0
        raise UnsupportedApi(f"PE host: unknown control id {control_id}")

    def dispatch_menu(self, item_id: int, source: str) -> int:
        raise UnsupportedApi(f"PE host has no menu (item {item_id})")

    def drain_posted_messages(self) -> int:
        return 0

    def send_sync(self, hwnd: int, msg: int, wparam: int, lparam: int,
                  reason: str = "") -> int:
        if msg == WM_CLOSE:
            self.close(reason=reason or "window close")
            return 0
        raise UnsupportedApi(f"PE host send_sync does not support msg=0x{msg:x}")

    # -- evidence ------------------------------------------------------
    def read_state(self) -> dict:
        counts = self._counts()
        return {
            "backend": self.backend.name,
            "window_title": self._control_text(self.main_hwnd),
            "seed_text": self._control_text(self.seed_handle),
            "a_text": self._control_text(self.a_handle),
            "b_text": self._control_text(self.b_handle),
            "n_text": self._control_text(self.n_handle),
            "result_text": self._control_text(self.result_handle),
            "state_text": self._control_text(self.state_handle),
            "calls_text": self._control_text(self.calls_handle),
            "status_text": self._control_text(self.status_handle),
            "load_text": self._control_text(self.load_handle),
            "bad_addr_text": self._control_text(self.bad_addr_handle),
            "counts": counts,
            "session_handle": f"0x{self.session_handle:x}" if self.session_handle else "0x0",
            "session_create_count": self.create_count,
            "session_info": self.session_info,
            "last_result": self.last_result,
            "last_status": self.last_status,
            "last_status_name": self.last_status_name,
            "last_error": self.last_error,
            "operation_count": self.operation_count,
            "main_thread": bool(NSThread.isMainThread()),
            "thread": threading.current_thread().name,
            "closed": self.closed,
        }

    def close_trace(self) -> dict:
        counts: dict = {}
        if self.session is None or not self.session.live:
            return counts
        if self.trace_path and not self.trace_closed:
            try:
                self.session.trace_close()
                self.trace_closed = True
                counts = self.session.counts()
                self.logger.event(
                    "pe.trace_closed", path=self.trace_path,
                    trace_records=counts.get("trace_records"),
                    trace_open=counts.get("trace_open"),
                )
            except PeSessionError as exc:
                self.logger.event("pe.trace_close_error", error=str(exc),
                                  status=exc.status)
                raise
        else:
            counts = self.session.counts()
        return counts

    def session_proof(self) -> dict:
        assert self.session is not None
        info = self.session_info
        counts = self._counts()
        exports = self.session.exports()
        selected = self.session.selected_rips()
        all_rips = self.session.all_block_rips()
        aot_sha_file = sha256_file(self.aot) if self.aot else None
        return {
            "library": self.session_lib,
            "library_sha256": sha256_file(self.session_lib),
            "dll": self.dll,
            "dll_sha256": sha256_file(self.dll),
            "registry": self.registry,
            "registry_sha256": sha256_file(self.registry),
            "aot": self.aot,
            "aot_sha256_file": aot_sha_file,
            "load_base": f"0x{self.load_base:x}",
            "entry_export": self.entry_export,
            "trace_path": self.trace_path,
            "session_handle": f"0x{self.session_handle:x}",
            "session_create_count": self.create_count,
            "created_on_main_thread": self.created_on_main_thread,
            "abi": self.session.abi,
            "info": info,
            "counts": counts,
            "exports": exports,
            "selected_rips": [f"0x{x:x}" for x in selected],
            "all_block_rips": [f"0x{x:x}" for x in all_rips],
            "aot_sha256_info": info.get("aot_sha256"),
            "aot_sha256_matches_file": (
                info.get("aot_sha256") == aot_sha_file
                if self.aot and info.get("aot_present") else None
            ),
            "module_kind": info.get("module_kind"),
            "target_triple": info.get("target_triple"),
            "load_count": counts.get("load_count"),
            "main_thread": bool(NSThread.isMainThread()),
        }

    def screenshot(self, name: str) -> Optional[str]:
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
            self.logger.event("pe.screenshot_error", error=str(exc), name=name)
            return None
        self.logger.event("pe.screenshot", path=path, window_number=number)
        return path

    def close(self, reason: str = "") -> None:
        if self.closed:
            return
        self.logger.event("pe.host_close", reason=reason,
                          session_handle=f"0x{self.session_handle:x}")
        if self.session is not None and self.session.live:
            try:
                self.session.destroy()
            except PeSessionError as exc:
                self.logger.event("pe.session_destroy_error", error=str(exc),
                                  status=exc.status)
            finally:
                self.session = None
                self.session_handle = 0
        self.backend.close_window(self.main_hwnd)
        self.closed = True
