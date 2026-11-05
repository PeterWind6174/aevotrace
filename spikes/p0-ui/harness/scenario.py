"""Automated P0-UI-01 scenario, shared by compat and native backends.

The scenario drives host controls, which synchronously enter the guest WndProc.
Every business mutation is performed by guest x64 code; the host only transfers
text and dispatches messages. State assertions recompute the guest's FNV hash
in Python to show the guest actually processed the text.
"""
from __future__ import annotations

import os
import traceback

from . import guest_abi as abi
from .errors import UnsupportedApi

FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK64 = 0xFFFFFFFFFFFFFFFF


def guest_fnv1a(text: str) -> int:
    h = FNV_OFFSET
    for ch in text:
        h ^= ord(ch) & 0xFFFF
        h = (h * FNV_PRIME) & MASK64
    return h


class Scenario:
    def __init__(self, host, backend, image, logger, dll_sha256: str):
        self.host = host
        self.backend = backend
        self.image = image
        self.logger = logger
        self.dll_sha256 = dll_sha256
        self.screenshots: list[str] = []
        self.states: dict[str, dict] = {}
        self.checks: dict[str, bool] = {}
        self.notes: list[str] = []
        self.negative: dict = {}
        self.messages: list[dict] = []

    # ------------------------------------------------------------------
    def _control(self, control_id: int) -> int:
        for info in self.host.windows.values():
            if info.control_id == control_id:
                return info.handle
        raise AssertionError(f"control id {control_id} was not created by the guest")

    def _snap(self, label: str) -> dict:
        state = self.host.read_state()
        self.states[label] = state
        self.logger.event("scenario.state", label=label, state=state)
        return state

    def _check(self, name: str, condition: bool, detail: str = ""):
        self.checks[name] = bool(condition)
        self.logger.event("scenario.check", name=name, pass_=bool(condition),
                          detail=detail)
        if not condition:
            raise AssertionError(f"check failed: {name} {detail}")

    def _wait_for(self, predicate, timeout=4.0, label="condition"):
        import time
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            self.backend.pump(0.05)
            last = self.host.read_state()
            if predicate(last):
                self.logger.event("scenario.wait_ok", label=label, state=last)
                return last
        self.logger.event("scenario.wait_timeout", label=label, state=last)
        raise AssertionError(f"timeout waiting for {label}: {last}")

    # ------------------------------------------------------------------
    def run(self) -> dict:
        self.logger.event("scenario.begin", backend=self.backend.name,
                          dll_sha256=self.dll_sha256)
        self.backend.pump(0.35)
        path = self.host.screenshot(f"01-initial-{self.backend.name}.png")
        if path:
            self.screenshots.append(path)

        initial = self._snap("initial")
        self._check("initial_reset", initial["resets"] == 1,
                    f"resets={initial['resets']}")
        self._check("initial_clicks", initial["clicks"] == 0)

        edit = self._control(abi.IDC_EDIT_INPUT)
        button = self._control(abi.IDC_BTN_INCREMENT)
        label = self._control(abi.IDC_STATIC_LABEL)

        # Unprepared input variant A: text did not exist in the fixture.
        text_a = "macOS 验收输入-β-2026-11"
        self.backend.set_edit_text(edit, text_a)
        state_a = self._snap("after_edit_a")
        self._check("edit_a_guest_saw_text", state_a["last_text"] == text_a,
                    state_a["last_text"])
        self._check("edit_a_hash", state_a["text_hash"] == guest_fnv1a(text_a),
                    f"{state_a['text_hash']:#x} vs {guest_fnv1a(text_a):#x}")
        self._check("edit_a_len", state_a["text_len"] == len(text_a))

        self.backend.click_button(button)
        state_click_1 = self._snap("after_click_1")
        self._check("click_1_count", state_click_1["clicks"] == 1)
        self._check("click_1_label_updated_by_guest",
                    "同步" in state_click_1["status_text"] or
                    state_click_1["status_text"].startswith("点击=1"),
                    state_click_1["status_text"])

        # Variant sequence B: a second, different text and another click.
        text_b = "第二次输入-λ-47"
        self.backend.set_edit_text(edit, text_b)
        self.backend.click_button(button)
        click_2 = self._snap("after_click_2")
        self._check("click_2_count", click_2["clicks"] == 2)
        self._check("text_b_guest_saw", click_2["last_text"] == text_b,
                    click_2["last_text"])
        self._check("text_b_hash", click_2["text_hash"] == guest_fnv1a(text_b),
                    f"{click_2['text_hash']:#x} vs {guest_fnv1a(text_b):#x}")

        self.backend.pump(0.2)
        path = self.host.screenshot(f"02-after-actions-{self.backend.name}.png")
        if path:
            self.screenshots.append(path)

        # Host -> Guest synchronous message with checked eax.
        echo = self.host.send_sync(self.host.main_hwnd, abi.WM_P0_ECHO, 0x5A5A, 0,
                                   reason="scenario synchronous echo")
        self._check("sync_echo_return", echo == 0x5A5A, f"ret={echo:#x}")
        echo_state = self._snap("after_echo")
        self._check("sync_echo_state", echo_state["sync_echo"] == 0x5A5A)

        # Minimal modal scene. The backend auto-dismisses with IDOK after
        # capturing a screenshot inside the modal run loop.
        self.backend.click_menu(abi.IDM_ABOUT)
        modal_state = self._snap("after_modal")
        self._check("modal_count", modal_state["modal_count"] == 1,
                    f"modal={modal_state['modal_count']}")
        self._check("modal_result", modal_state["modal_result"] == abi.IDOK,
                    f"result={modal_state['modal_result']}")

        # Worker thread: guest CreateThread -> Sleep -> PostMessage -> ExitThread.
        self.backend.click_menu(abi.IDM_RUN_WORKER)
        worker_state = self._wait_for(
            lambda s: (s["worker_created"] >= 1 and
                       s["worker_posts_sent"] >= 1 and
                       s["worker_posts_received"] >= 1 and
                       s["worker_exits"] >= 1),
            label="worker post/exit",
        )
        self._check("worker_created", worker_state["worker_created"] == 1)
        self._check("worker_posts", worker_state["worker_posts_sent"] == 1 and
                    worker_state["worker_posts_received"] == 1)
        self._check("worker_exits", worker_state["worker_exits"] == 1)

        path = self.host.screenshot(f"04-final-{self.backend.name}.png")
        if path:
            self.screenshots.append(path)
        modal_path = os.path.join(self.host.evidence_dir,
                                  f"03-modal-{self.backend.name}.png")
        if os.path.exists(modal_path) and modal_path not in self.screenshots:
            self.screenshots.append(modal_path)
            self._check("modal_screenshot_saved", True, modal_path)
        final_state = self._snap("final_before_close")

        self._check("final_clicks", final_state["clicks"] == 2)
        self._check("final_edit_changes", final_state["edit_changes"] >= 2,
                    f"editChanges={final_state['edit_changes']}")
        self._check("final_text", final_state["last_text"] == text_b)
        self._check("final_wndproc", final_state["wndproc_calls"] >= 9,
                    f"wndproc={final_state['wndproc_calls']}")

        # Close through WM_CLOSE, which must be a guest decision.
        self.host.send_sync(self.host.main_hwnd, abi.WM_CLOSE, 0, 0,
                            reason="scenario close")
        self.backend.pump(0.2)
        self._check("window_closed", self.host.closed)

        # Negative case: an API outside the declared subset must fail
        # explicitly. This is run last because the exception aborts the
        # current guest call; it also proves there is no compat fallback.
        before = self.host.machine.instruction_count
        try:
            self.host.machine.call(
                self.image.export("P0ProbeUnsupported"), [],
                purpose="negative unsupported API probe")
        except UnsupportedApi as exc:
            self.negative = {
                "expected": "unsupported_api",
                "observed": str(exc),
                "guest_instructions_before_abort":
                    self.host.machine.instruction_count - before,
                "host_unsupported": self.host.unsupported[-1] if self.host.unsupported else None,
                "backend": self.backend.name,
            }
            self.logger.event("scenario.negative_pass", **self.negative)
        else:
            self.negative = {
                "expected": "unsupported_api",
                "observed": "call returned successfully",
                "backend": self.backend.name,
            }
            self.logger.event("scenario.negative_fail", **self.negative)
            self._check("negative_unsupported_api", False,
                        "MessageBeep unexpectedly implemented")
        self._check("negative_unsupported_api",
                    self.negative.get("observed") != "call returned successfully")

        self.messages = list(self.host.message_sequence)
        compat_events = sum(v for k, v in self.logger.kind_counts.items()
                            if k.startswith("compat."))
        native_events = sum(v for k, v in self.logger.kind_counts.items()
                            if k.startswith("native."))
        if self.backend.name == "native":
            self._check("native_no_compat_events", compat_events == 0,
                        f"compat_events={compat_events}")
        else:
            self._check("compat_no_native_events", native_events == 0,
                        f"native_events={native_events}")
        self.logger.event("scenario.end", checks=self.checks,
                          screenshots=self.screenshots)
        return self.summary()

    def summary(self) -> dict:
        proof = self.backend.proof()
        compat_events = sum(v for k, v in self.logger.kind_counts.items()
                            if k.startswith("compat."))
        native_events = sum(v for k, v in self.logger.kind_counts.items()
                            if k.startswith("native."))
        return {
            "success": all(self.checks.values()) and bool(self.checks),
            "checks": self.checks,
            "notes": self.notes,
            "states": self.states,
            "message_sequence": self.messages,
            "guest_execution": {
                "dll_sha256": self.dll_sha256,
                "main_instructions": self.host.machine.instruction_count,
                "main_api_counts": self.host.machine.api_counts,
                "worker_machines": [
                    {"thread_id": f"0x{w.thread_id:x}",
                     "instructions": w.instruction_count,
                     "api_counts": w.api_counts}
                    for w in self.host.worker_machines
                ],
                "wndproc_calls_in_guest_state":
                    self.states.get("final_before_close", {}).get("wndproc_calls", 0),
                "host_message_entries": len(self.messages),
                "host_api_calls": self.host.api_call_count,
                "sync_reentries_guest_to_host": self.host.sync_reentries,
                "max_host_guest_depth": self.host.max_call_depth,
            },
            "negative_case": self.negative,
            "backend_proof": proof,
            "backend_event_counts": {
                "native": native_events,
                "compat": compat_events,
            },
            "screenshots": self.screenshots,
        }
