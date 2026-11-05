"""Declared Win32 import subset for the P0-UI-01 GUI spike.

The host implements GUI services and a deliberately small API surface. Guest
business code is never reimplemented here: handlers create/read host widgets,
route messages to the guest WndProc, and report every call. Unsupported calls
raise UnsupportedApi instead of returning invented success values.
"""
from __future__ import annotations

import ctypes
import os
import queue
import subprocess
import threading
import time
from dataclasses import dataclass, field

from . import guest_abi as abi
from .guest_pe import (
    GuestExitThread,
    GuestMachine,
    TEMP_BASE,
    UnsupportedApi,
)

WS_VISIBLE = 0x10000000
WS_CHILD = 0x40000000
MF_POPUP = 0x00000010
MF_SEPARATOR = 0x00000800
MF_STRING = 0x00000000

CONTROL_CLASSES = {"STATIC", "EDIT", "BUTTON"}


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


class Win32Host:
    def __init__(self, backend, image, logger, evidence_dir: str):
        self.backend = backend
        self.image = image
        self.logger = logger
        self.evidence_dir = evidence_dir
        self.machine: GuestMachine | None = None
        self.worker_machines: list[GuestMachine] = []
        self.windows: dict[int, WindowInfo] = {}
        self.main_hwnd = 0
        self.wndproc = 0
        self.next_window_handle = 0x1000
        self.menu_specs: dict[int, list[dict]] = {}
        self.next_menu_handle = 0x3000
        self.last_error = 0
        self.post_queue: queue.Queue = queue.Queue()
        self.quit_posted = False
        self.show_requested = False
        self.worker_seq = 0
        self.worker_handles: dict[int, int] = {}
        self.call_depth = 0
        self.max_call_depth = 0
        self.sync_reentries = 0
        self.api_call_count = 0
        self.unsupported: list[str] = []
        self.message_sequence: list[dict] = []
        self.closed = False

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------
    def start(self) -> int:
        self.machine = GuestMachine(self.image, self, self.logger,
                                    thread_name="main", thread_id=0xA001)
        # The manual loader does not run the PE loader, so call DllMain first.
        self.machine.call(self.image.entry_va, [self.image.base, 1, 0],
                          purpose="DllMain")
        self.machine.call(self.image.export("P0Reset"), [], purpose="P0Reset")
        hwnd = self.machine.call(self.image.export("P0CreateMainWindow"), [],
                                 purpose="P0CreateMainWindow")
        if hwnd == 0:
            raise UnsupportedApi("guest P0CreateMainWindow returned NULL")
        self.main_hwnd = hwnd
        self.logger.event("guest.window_created", handle=f"0x{hwnd:x}",
                          title=self.windows[hwnd].title)
        # On Windows CreateWindowExW sends WM_CREATE internally. This spike
        # intentionally uses a host-owned dispatcher, so the host sends the
        # same message synchronously immediately after window creation.
        self.send_sync(hwnd, abi.WM_CREATE, 0, 0, reason="host WM_CREATE after manual load")
        self.backend.show_window(hwnd)
        self.windows[hwnd].visible = True
        return hwnd

    def send_sync(self, hwnd: int, msg: int, wparam: int, lparam: int,
                  reason: str = "") -> int:
        if self.machine is None:
            raise UnsupportedApi("guest machine not started")
        if self.machine.in_emulation:
            raise UnsupportedApi(
                f"nested Host->Guest dispatch blocked while guest is executing "
                f"({abi.msg_name(msg)} {reason})"
            )
        info = self.windows.get(hwnd)
        if info is not None and info.kind != "main":
            raise UnsupportedApi(f"WndProc send to non-main handle 0x{hwnd:x}")
        self.call_depth += 1
        self.max_call_depth = max(self.max_call_depth, self.call_depth)
        self.logger.event(
            "host.guest_enter", msg=abi.msg_name(msg), msg_id=msg,
            wparam=f"0x{wparam:x}", lparam=f"0x{lparam:x}",
            handle=f"0x{hwnd:x}", depth=self.call_depth, reason=reason,
        )
        try:
            ret = self.machine.call(
                self.wndproc, [hwnd, msg, wparam & 0xFFFFFFFFFFFFFFFF,
                               lparam & 0xFFFFFFFFFFFFFFFF],
                purpose=f"WndProc {abi.msg_name(msg)}",
            )
            self.message_sequence.append({
                "msg": abi.msg_name(msg), "msg_id": msg,
                "wparam": wparam & 0xFFFFFFFFFFFFFFFF,
                "lparam": lparam & 0xFFFFFFFFFFFFFFFF,
                "ret": ret & 0xFFFFFFFFFFFFFFFF,
                "reason": reason,
            })
            self.logger.event(
                "host.guest_exit", msg=abi.msg_name(msg), msg_id=msg,
                ret=f"0x{ret & 0xFFFFFFFFFFFFFFFF:x}", depth=self.call_depth,
                reason=reason,
            )
            return ret
        finally:
            self.call_depth -= 1

    def dispatch_command(self, control_id: int, notification: int,
                         source: str) -> int:
        wparam = (notification << 16) | (control_id & 0xFFFF)
        return self.send_sync(self.main_hwnd, abi.WM_COMMAND, wparam, 0,
                              reason=source)

    def dispatch_menu(self, item_id: int, source: str) -> int:
        return self.send_sync(self.main_hwnd, abi.WM_COMMAND, item_id & 0xFFFF, 0,
                              reason=source)

    def read_state(self) -> dict:
        self.logger.event("host.guest_state_read", thread="main")
        return self.machine.read_guest_state()

    def drain_posted_messages(self) -> int:
        if self.machine is None or self.machine.in_emulation:
            return 0
        processed = 0
        while True:
            try:
                hwnd, msg, wparam, lparam = self.post_queue.get_nowait()
            except queue.Empty:
                break
            self.logger.event("host.drain_post", msg=abi.msg_name(msg),
                              handle=f"0x{hwnd:x}", wparam=f"0x{wparam:x}")
            info = self.windows.get(hwnd)
            if info is not None and info.kind == "main":
                self.send_sync(hwnd, msg, wparam, lparam, reason="posted message drain")
            elif info is not None:
                self._apply_control_message(info, msg, wparam, lparam)
            processed += 1
        return processed

    def screenshot(self, name: str, target_handle: int | None = None,
                   window_number: int | None = None) -> str | None:
        handle = target_handle if target_handle is not None else self.main_hwnd
        number = window_number if window_number is not None else self.backend.window_number(handle)
        if number is None:
            self.logger.event("screenshot.skip", name=name, reason="no window number")
            return None
        path = os.path.join(self.evidence_dir, name)
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        try:
            proc = subprocess.run(
                ["screencapture", "-x", "-o", f"-l{number}", path],
                capture_output=True, text=True, timeout=10,
            )
        except Exception as exc:  # noqa: BLE001 - evidence path must be reported
            self.logger.event("screenshot.error", name=name, error=str(exc))
            return None
        exists = os.path.exists(path)
        self.logger.event(
            "screenshot", name=name, path=path, rc=proc.returncode,
            exists=exists, size=os.path.getsize(path) if exists else 0,
            stderr=proc.stderr.strip(), window_number=number,
        )
        return path

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------
    def _alloc_window_handle(self) -> int:
        handle = self.next_window_handle
        self.next_window_handle += 0x10
        return handle

    def _window(self, handle: int) -> WindowInfo:
        info = self.windows.get(handle)
        if info is None:
            raise UnsupportedApi(f"unknown guest HWND 0x{handle:x}")
        return info

    def _text(self, info: WindowInfo) -> str:
        return self.backend.get_text(info.handle)

    def _set_text(self, info: WindowInfo, text: str, notify: bool = False):
        self.backend.set_text(info.handle, text)
        info.title = text
        self.logger.event("host.control_set_text", handle=f"0x{info.handle:x}",
                          control_kind=info.kind, text=text, notify=notify)

    def _apply_control_message(self, info: WindowInfo, msg: int,
                               wparam: int, lparam: int) -> int:
        if msg == abi.WM_SETTEXT:
            text = self.machine.read_wstring(lparam)
            self._set_text(info, text, notify=False)
            return 1
        if msg == abi.WM_GETTEXT:
            return self.machine.write_wstring(lparam, self._text(info), wparam)
        if msg == abi.WM_GETTEXTLENGTH:
            return len(self._text(info))
        raise UnsupportedApi(
            f"SendMessageW to control 0x{info.handle:x} ({info.kind}) "
            f"message {abi.msg_name(msg)} is outside the P0 subset"
        )

    def _read_menu_items(self, menu_handle: int) -> list[dict]:
        items = self.menu_specs.get(menu_handle)
        if items is None:
            raise UnsupportedApi(f"unknown menu handle 0x{menu_handle:x}")
        result = []
        for item in items:
            if item["kind"] == "popup":
                result.append({
                    "kind": "popup",
                    "label": item["label"],
                    "items": self._read_menu_items(item["menu"]),
                })
            elif item["kind"] == "separator":
                result.append({"kind": "separator"})
            else:
                result.append({"kind": "item", "label": item["label"],
                               "id": item["id"]})
        return result

    # ------------------------------------------------------------------
    # API dispatch
    # ------------------------------------------------------------------
    def dispatch_api(self, machine: GuestMachine, symbol, args: list[int]) -> int:
        self.api_call_count += 1
        name = symbol.name
        handler = getattr(self, f"api_{name}", None)
        arg_preview = [f"0x{a:x}" for a in args[:8]]
        during_guest = self.call_depth > 0
        if during_guest:
            # Host -> Guest (send_sync) -> Host (this import call) is the
            # synchronous reentry chain under test.
            self.sync_reentries += 1
        self.logger.event(
            "api.call", dll=symbol.dll, api=name, args=arg_preview,
            thread=machine.thread_name, depth=self.call_depth,
            tid=f"0x{machine.thread_id:x}",
            during_guest_call=during_guest,
            direction=("guest->host" if during_guest else "thread->host"),
        )
        if handler is None:
            msg = f"unimplemented API {symbol.dll}!{name}"
            self.unsupported.append(msg)
            self.logger.event("api.unsupported", dll=symbol.dll, api=name)
            raise UnsupportedApi(msg)
        try:
            ret = handler(machine, args)
        except GuestExitThread:
            raise
        except Exception as exc:
            self.logger.event("api.error", api=name, error=str(exc),
                              thread=machine.thread_name)
            raise
        self.logger.event("api.return", api=name, ret=f"0x{ret & 0xFFFFFFFFFFFFFFFF:x}",
                          thread=machine.thread_name)
        return ret

    # kernel32 ---------------------------------------------------------
    def api_GetModuleHandleW(self, machine, args):
        return self.image.base

    def api_GetLastError(self, machine, args):
        return self.last_error

    def api_GetCurrentThreadId(self, machine, args):
        return machine.thread_id

    def api_Sleep(self, machine, args):
        ms = args[0] & 0xFFFFFFFF
        self.logger.event("guest.sleep", thread=machine.thread_name, ms=ms)
        time.sleep(ms / 1000.0)
        return 0

    def api_CreateThread(self, machine, args):
        start = args[2]
        param = args[3]
        tid_va = args[5]
        self.worker_seq += 1
        tid = 0xB000 + self.worker_seq
        handle = 0x2000 + self.worker_seq
        if tid_va:
            machine.write_bytes(tid_va, (tid & 0xFFFFFFFF).to_bytes(4, "little"))
        self.worker_handles[handle] = tid
        self.logger.event("guest.thread_create", handle=f"0x{handle:x}",
                          guest_start=f"0x{start:x}", param=f"0x{param:x}",
                          tid=f"0x{tid:x}", requested_by=machine.thread_name)

        def runner():
            worker = GuestMachine(self.image, self, self.logger,
                                  thread_name=f"worker-0x{tid:x}", thread_id=tid)
            self.worker_machines.append(worker)
            try:
                worker.run_thread(start, param)
            except GuestExitThread:
                pass
            except Exception as exc:  # noqa: BLE001
                self.logger.event("worker.error", tid=f"0x{tid:x}", error=str(exc))
            finally:
                self.logger.event(
                    "worker.finished", tid=f"0x{tid:x}",
                    instructions=worker.instruction_count,
                    api_counts=worker.api_counts,
                )

        thread = threading.Thread(target=runner, name=f"guest-worker-0x{tid:x}",
                                  daemon=True)
        thread.start()
        return handle

    def api_ExitThread(self, machine, args):
        self.logger.event("guest.exit_thread", thread=machine.thread_name,
                          code=args[0] & 0xFFFFFFFF)
        raise GuestExitThread()

    # user32: window/control -------------------------------------------
    def api_RegisterClassExW(self, machine, args):
        wc_va = args[0]
        if wc_va == 0:
            raise UnsupportedApi("RegisterClassExW(NULL)")
        proc = machine.read_u64(wc_va + 8)
        name_ptr = machine.read_u64(wc_va + 64)
        class_name = machine.read_wstring(name_ptr)
        if proc == 0:
            raise UnsupportedApi("RegisterClassExW with NULL WndProc")
        self.wndproc = proc
        self.last_error = 0
        self.logger.event("host.register_class", class_name=class_name,
                          wndproc=f"0x{proc:x}")
        return 1

    def api_CreateWindowExW(self, machine, args):
        class_name = machine.read_wstring(args[1])
        title = machine.read_wstring(args[2])
        style = args[3] & 0xFFFFFFFF
        x, y, width, height = (args[4] & 0xFFFFFFFF, args[5] & 0xFFFFFFFF,
                               args[6] & 0xFFFFFFFF, args[7] & 0xFFFFFFFF)
        parent = args[8]
        menu_or_id = args[9]
        if class_name == abi.P0_WINDOW_CLASS and parent == 0:
            handle = self._alloc_window_handle()
            info = WindowInfo(handle=handle, kind="main", title=title,
                              style=style, visible=bool(style & WS_VISIBLE))
            self.windows[handle] = info
            self.main_hwnd = handle
            self.backend.create_main_window(handle, title, x, y, width, height)
            self.show_requested = info.visible
            self.logger.event("host.create_window", handle=f"0x{handle:x}",
                              title=title, width=width, height=height,
                              visible=info.visible)
            return handle
        if class_name in CONTROL_CLASSES and parent != 0:
            parent_info = self._window(parent)
            if parent_info.kind != "main":
                raise UnsupportedApi("child control parent is not the main window")
            handle = self._alloc_window_handle()
            control_id = menu_or_id & 0xFFFF
            info = WindowInfo(handle=handle, kind=class_name.lower(),
                              title=title, parent=parent,
                              control_id=control_id, style=style,
                              visible=bool(style & WS_VISIBLE))
            self.windows[handle] = info
            self.backend.create_control(handle, info.kind, title, style,
                                        x, y, width, height, control_id)
            self.logger.event("host.create_control", handle=f"0x{handle:x}",
                              control_kind=info.kind, control_id=control_id, text=title,
                              x=x, y=y, width=width, height=height)
            return handle
        raise UnsupportedApi(
            f"CreateWindowExW class={class_name!r} parent=0x{parent:x} unsupported"
        )

    def api_DestroyWindow(self, machine, args):
        info = self._window(args[0])
        info.destroyed = True
        self.backend.close_window(info.handle)
        self.logger.event("host.destroy_window", handle=f"0x{info.handle:x}",
                          control_kind=info.kind)
        if info.handle == self.main_hwnd:
            self.closed = True
        return 1

    def api_ShowWindow(self, machine, args):
        info = self._window(args[0])
        previous = 1 if info.visible else 0
        info.visible = True
        self.show_requested = True
        self.logger.event("host.show_window", handle=f"0x{info.handle:x}")
        return previous

    def api_UpdateWindow(self, machine, args):
        return 1

    def api_SetMenu(self, machine, args):
        hwnd = args[0]
        menu = args[1]
        info = self._window(hwnd)
        items = self._read_menu_items(menu) if menu else []
        info.extra["menu"] = items
        self.backend.set_menu(hwnd, items)
        self.logger.event("host.set_menu", handle=f"0x{hwnd:x}",
                          items=[i.get("label") for i in items])
        return 1

    def api_CreateMenu(self, machine, args):
        handle = self.next_menu_handle
        self.next_menu_handle += 0x10
        self.menu_specs[handle] = []
        return handle

    def api_CreatePopupMenu(self, machine, args):
        return self.api_CreateMenu(machine, args)

    def api_AppendMenuW(self, machine, args):
        menu = args[0]
        flags = args[1] & 0xFFFFFFFF
        value = args[2]
        text = machine.read_wstring(args[3]) if args[3] else ""
        if menu not in self.menu_specs:
            raise UnsupportedApi(f"AppendMenuW unknown menu 0x{menu:x}")
        if flags & MF_SEPARATOR:
            item = {"kind": "separator"}
        elif flags & MF_POPUP:
            if text == "":
                text = f"popup-0x{value:x}"
            item = {"kind": "popup", "menu": value, "label": text}
        else:
            item = {"kind": "item", "id": value & 0xFFFF, "label": text}
        self.menu_specs[menu].append(item)
        self.logger.event("host.append_menu", menu=f"0x{menu:x}",
                          flags=f"0x{flags:x}", value=value, text=text)
        return 1

    # user32: text/messages --------------------------------------------
    def api_SetWindowTextW(self, machine, args):
        info = self._window(args[0])
        text = machine.read_wstring(args[1])
        self._set_text(info, text, notify=False)
        return 1

    def api_GetWindowTextW(self, machine, args):
        info = self._window(args[0])
        text = self._text(info)
        n = machine.write_wstring(args[1], text, args[2] & 0xFFFFFFFF)
        self.logger.event("host.get_text", handle=f"0x{info.handle:x}",
                          text=text, copied=n)
        return n

    def api_SendMessageW(self, machine, args):
        hwnd = args[0]
        msg = args[1] & 0xFFFFFFFF
        wparam = args[2]
        lparam = args[3]
        info = self._window(hwnd)
        if info.kind != "main":
            self.logger.event("host.send_message", handle=f"0x{hwnd:x}",
                              msg=abi.msg_name(msg), control_kind=info.kind)
            return self._apply_control_message(info, msg, wparam, lparam)
        raise UnsupportedApi(
            f"SendMessageW(0x{hwnd:x}, {abi.msg_name(msg)}) to a guest window "
            "would require a nested guest callback; the P0 host dispatcher "
            "rejects it instead of faking synchronous semantics"
        )

    def api_PostMessageW(self, machine, args):
        hwnd = args[0]
        msg = args[1] & 0xFFFFFFFF
        wparam = args[2]
        lparam = args[3]
        self._window(hwnd)
        self.post_queue.put((hwnd, msg, wparam, lparam))
        self.logger.event("host.post_message", handle=f"0x{hwnd:x}",
                          msg=abi.msg_name(msg), wparam=f"0x{wparam:x}",
                          from_thread=machine.thread_name)
        return 1

    def api_MessageBoxW(self, machine, args):
        hwnd = args[0]
        text = machine.read_wstring(args[1])
        caption = machine.read_wstring(args[2])
        style = args[3] & 0xFFFFFFFF
        self.logger.event("host.modal_enter", handle=f"0x{hwnd:x}",
                          caption=caption, text=text, style=f"0x{style:x}")
        result = self.backend.show_message_box(hwnd, text, caption, style,
                                               capture=self.screenshot)
        self.logger.event("host.modal_exit", result=result)
        return result

    def api_DefWindowProcW(self, machine, args):
        # The fixture only calls DefWindowProcW for messages it does not
        # handle (WM_NCCREATE, WM_ERASEBKGND, WM_NCPAINT, ...). Returning 0
        # is the declared subset behaviour; it is not a stand-in for user32.
        return 0

    def api_GetMessageW(self, machine, args):
        raise UnsupportedApi(
            "GetMessageW: P0-UI-01 macOS harness uses a host-owned dispatcher; "
            "the guest loop is only run as the Windows reference"
        )

    def api_TranslateMessage(self, machine, args):
        return 1

    def api_DispatchMessageW(self, machine, args):
        return 0

    def api_PostQuitMessage(self, machine, args):
        self.quit_posted = True
        self.logger.event("host.post_quit", code=args[0] & 0xFFFFFFFF)
        return 0
