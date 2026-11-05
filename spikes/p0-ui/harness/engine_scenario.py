"""Automated three-sequence + 100-callback scenario for the engine GUI path."""
from __future__ import annotations

import re

from .engine_bridge import (
    AEV_BRIDGE_ERR_MEMORY,
    AEV_BRIDGE_ERR_REENTRY,
    AEV_BRIDGE_ERR_UNKNOWN_IMPORT,
    AEV_BRIDGE_OK,
)

# Windows reference values from testdata/callback/reference_stdout.txt for the
# same fixed COFF object and the same helper contract (helper returns op).
WINDOWS_REFERENCE = {
    "alpha": 3828036654,   # op=7
    "beta": 614625053,     # op=9
    "hidden": 651115151,   # op=23130
}


def guest_fnv1a32(text: str) -> int:
    h = 2166136261
    data = text.encode("utf-16-le")
    for i in range(0, len(data), 2):
        unit = data[i] | (data[i + 1] << 8)
        h ^= unit
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def utf16_units(text: str) -> int:
    return len(text.encode("utf-16-le")) // 2


def parse_label(text: str) -> tuple[int, int, int] | None:
    m = re.search(r"guest hash=0x([0-9A-Fa-f]+) len=(\d+) op=(\d+)", text)
    if not m:
        return None
    return int(m.group(1), 16), int(m.group(2)), int(m.group(3))


class EngineScenario:
    def __init__(self, host, backend, logger, evidence_dir: str,
                 bridge_lib: str, coff: str, aot: str):
        self.host = host
        self.backend = backend
        self.logger = logger
        self.evidence_dir = evidence_dir
        self.bridge_lib = bridge_lib
        self.coff = coff
        self.aot = aot
        self.checks: dict[str, bool] = {}
        self.states: dict[str, dict] = {}
        self.screenshots: list[str] = []
        self.notes: list[str] = []
        self.negative: dict[str, dict] = {}

    def _check(self, name: str, condition: bool, detail: str = "") -> None:
        self.checks[name] = bool(condition)
        self.logger.event("engine.check", name=name, pass_=bool(condition),
                          detail=detail)
        if not condition:
            raise AssertionError(f"check failed: {name} {detail}")

    def _snap(self, label: str) -> dict:
        state = self.host.read_state()
        self.states[label] = state
        self.logger.event("engine.state", label=label, state=state)
        return state

    def _expected_label(self, text: str, op: int) -> str:
        # The HostImport receives the Guest-computed FNV hash; it must not
        # compute the Guest's final XOR result.  The final guest_rax is checked
        # separately from the bridge result.
        return f"guest hash=0x{guest_fnv1a32(text):08X} len={utf16_units(text)} op={op}"

    def _expected_guest_rax(self, text: str, op: int) -> int:
        return (guest_fnv1a32(text) ^ op) & 0xFFFFFFFF

    def _screenshot(self, name: str) -> None:
        try:
            self.backend.pump(0.1)
        except Exception:
            pass
        path = self.host.screenshot(name)
        if path:
            self.screenshots.append(path)

    # ------------------------------------------------------------------
    def run(self) -> dict:
        self.logger.event("engine.scenario_begin", backend=self.backend.name)
        self.backend.pump(0.35)
        self._screenshot(f"01-initial-{self.backend.name}.png")
        initial = self._snap("initial")
        self._check("initial_counter", initial["counter"] == 0, str(initial["counter"]))
        self._check("initial_result_pending",
                    initial["result_text"] == "等待 Guest 回调", initial["result_text"])

        edit = self.host.edit_handle
        button = self.host.button_handle
        text_a = "Aevoβ中"
        self.host.button_op = 7
        self.backend.set_edit_text(edit, text_a)
        self.backend.click_button(button)
        state_a = self._snap("sequence_a")
        self._check("sequence_a_counter", state_a["counter"] == 2,
                    str(state_a["counter"]))
        expected_a = self._expected_label(text_a, 7)
        self._check("sequence_a_label", state_a["result_text"] == expected_a,
                    f"{state_a['result_text']} != {expected_a}")
        self._check("sequence_a_guest_rax_windows",
                    (state_a["bridge"] or {}).get("guest_rax") ==
                    WINDOWS_REFERENCE["alpha"],
                    str((state_a["bridge"] or {}).get("guest_rax")))
        self._check("sequence_a_helper", self.host.helper_calls >= 2,
                    str(self.host.helper_calls))

        text_b = "macOS-47"
        self.host.button_op = 9
        self.backend.set_edit_text(edit, text_b)
        self.backend.click_button(button)
        state_b = self._snap("sequence_b")
        self._check("sequence_b_counter", state_b["counter"] == 4,
                    str(state_b["counter"]))
        expected_b = self._expected_label(text_b, 9)
        self._check("sequence_b_label", state_b["result_text"] == expected_b,
                    f"{state_b['result_text']} != {expected_b}")
        self._check("sequence_b_guest_rax_windows",
                    (state_b["bridge"] or {}).get("guest_rax") ==
                    WINDOWS_REFERENCE["beta"],
                    str((state_b["bridge"] or {}).get("guest_rax")))

        # Independent/held-out sequence: not used to choose the fixture or AOT.
        text_c = "独立-Ω-93"
        self.host.button_op = 0x5A5A
        self.backend.set_edit_text(edit, text_c)
        self.backend.click_button(button)
        state_c = self._snap("sequence_c_hidden")
        self._check("sequence_c_counter", state_c["counter"] == 6,
                    str(state_c["counter"]))
        expected_c = self._expected_label(text_c, 0x5A5A)
        self._check("sequence_c_label", state_c["result_text"] == expected_c,
                    f"{state_c['result_text']} != {expected_c}")
        self._check("sequence_c_guest_rax_windows",
                    (state_c["bridge"] or {}).get("guest_rax") ==
                    WINDOWS_REFERENCE["hidden"],
                    str((state_c["bridge"] or {}).get("guest_rax")))

        self.backend.pump(0.2)
        self._screenshot(f"02-after-actions-{self.backend.name}.png")

        # 100 consecutive callbacks on the same bridge instance.
        text_loop = "loop-100"
        self.host.button_op = 2
        self.backend.set_edit_text(edit, text_loop)  # one edit callback
        loop_start = self.host.read_state()["counter"]
        loop_ok = True
        loop_jit_cached = True
        loop_jit_executed = False
        for i in range(100):
            self.backend.click_button(button)
            state = self.host.read_state()
            if state["last_status"] != AEV_BRIDGE_OK:
                loop_ok = False
                self.logger.event("engine.loop_failure", index=i, state=state)
                break
            bridge = state["bridge"] or {}
            loop_jit_cached = loop_jit_cached and bridge.get("jit_compiles") == 0
            loop_jit_executed = loop_jit_executed or bridge.get("jit_execs", 0) > 0
            if i % 20 == 0:
                self.backend.pump(0.01)
        loop_end = self.host.read_state()["counter"]
        self._check("loop_100_status", loop_ok)
        self._check("loop_100_counter", loop_end == loop_start + 100,
                    f"{loop_end} - {loop_start}")
        self._check("loop_100_jit_cached", loop_jit_cached)
        self._check("loop_100_jit_executed", loop_jit_executed)
        last = self.host.read_state()
        expected_loop = self._expected_label(text_loop, 2)
        self._check("loop_100_label", last["result_text"] == expected_loop,
                    f"{last['result_text']} != {expected_loop}")
        self._check("loop_100_guest_rax",
                    (last["bridge"] or {}).get("guest_rax") ==
                    self._expected_guest_rax(text_loop, 2),
                    str((last["bridge"] or {}).get("guest_rax")))
        self._check("loop_total_jit_compiles",
                    (last["bridge"] or {}).get("total_jit_compiles") == 1,
                    str((last["bridge"] or {}).get("total_jit_compiles")))

        self._screenshot(f"03-after-100-{self.backend.name}.png")

        # Negative: unknown HostImport must not update the control or counter.
        label_before = self.host.read_state()["result_text"]
        counter_before = self.host.read_state()["counter"]
        status = self.host.probe_unknown_import()
        self.negative["unknown_import"] = {"status": status}
        self._check("unknown_import_status", status == AEV_BRIDGE_ERR_UNKNOWN_IMPORT,
                    str(status))
        state = self.host.read_state()
        self._check("unknown_import_counter", state["counter"] == counter_before)
        self._check("unknown_import_label", state["result_text"] == label_before)

        # Negative: illegal Guest memory.
        status = self.host.probe_invalid_memory()
        self.negative["invalid_memory"] = {"status": status}
        self._check("invalid_memory_status", status == AEV_BRIDGE_ERR_MEMORY,
                    str(status))
        state = self.host.read_state()
        self._check("invalid_memory_counter", state["counter"] == counter_before)
        self._check("invalid_memory_label", state["result_text"] == label_before)

        # Negative: unsupported reentry; outer call must still complete.
        counter_before = self.host.read_state()["counter"]
        outer = self.host.probe_reentry()
        self.negative["reentry"] = {
            "outer_status": outer,
            "inner_status": self.host.reentry_status,
        }
        self._check("reentry_outer_ok", outer == AEV_BRIDGE_OK, str(outer))
        self._check("reentry_inner_rejected",
                    self.host.reentry_status == AEV_BRIDGE_ERR_REENTRY,
                    str(self.host.reentry_status))
        state = self.host.read_state()
        self._check("reentry_counter", state["counter"] == counter_before + 1,
                    f"{state['counter']} - {counter_before}")

        # Negative: helper succeeds but the following Guest counter access faults;
        # the pending UI result must not be committed.
        label_before = self.host.read_state()["result_text"]
        counter_before = self.host.read_state()["counter"]
        status, helper_called = self.host.probe_helper_then_fault()
        self.negative["helper_then_fault"] = {
            "status": status,
            "helper_called": helper_called,
        }
        self._check("helper_then_fault_status", status == AEV_BRIDGE_ERR_MEMORY,
                    str(status))
        self._check("helper_then_fault_helper_called", helper_called)
        state = self.host.read_state()
        self._check("helper_then_fault_counter", state["counter"] == counter_before)
        self._check("helper_then_fault_label", state["result_text"] == label_before)

        # Recovery after failures: a normal event sequence must commit again.
        self.host.button_op = 7
        self.backend.set_edit_text(edit, text_a)
        self.backend.click_button(button)
        state = self.host.read_state()
        self._check("recovery_status", state["last_status"] == AEV_BRIDGE_OK,
                    str(state["last_status"]))
        self._check("recovery_label", state["result_text"] == expected_a)
        self._check("recovery_guest_rax",
                    (state["bridge"] or {}).get("guest_rax") ==
                    self._expected_guest_rax(text_a, 7),
                    str((state["bridge"] or {}).get("guest_rax")))

        self._screenshot(f"04-final-{self.backend.name}.png")

        backend_proof = self.backend.proof()
        bridge_proof = self.host.bridge_proof()
        compat_events = sum(v for k, v in self.logger.kind_counts.items()
                            if k.startswith("compat."))
        native_events = sum(v for k, v in self.logger.kind_counts.items()
                            if k.startswith("native."))
        if self.backend.name == "native":
            self._check("native_backend_used", native_events > 0,
                        f"native_events={native_events}")
            self._check("native_no_compat_events", compat_events == 0,
                        f"compat_events={compat_events}")
            self._check("native_not_compat_classes",
                        all(not c["objc_class"].startswith("Compat")
                            for c in backend_proof["controls"]))
        else:
            self._check("compat_backend_used", compat_events > 0,
                        f"compat_events={compat_events}")
            self._check("compat_no_native_events", native_events == 0,
                        f"native_events={native_events}")
            self._check("compat_classes",
                        all(c["objc_class"].startswith("Compat")
                            for c in backend_proof["controls"]))
        self._check("bridge_no_unicorn", not bridge_proof["unicorn_loaded"])
        self._check("bridge_no_guest_pe", not bridge_proof["guest_pe_loaded"])

        self.logger.event("engine.scenario_end", checks=self.checks,
                          screenshots=self.screenshots)
        return self.summary(backend_proof, bridge_proof, compat_events, native_events)

    def summary(self, backend_proof: dict, bridge_proof: dict,
                compat_events: int, native_events: int) -> dict:
        return {
            "success": all(self.checks.values()) and bool(self.checks) and
                       bool(self.screenshots),
            "checks": self.checks,
            "states": self.states,
            "negative": self.negative,
            "backend_proof": backend_proof,
            "bridge_proof": bridge_proof,
            "backend_event_counts": {"native": native_events,
                                     "compat": compat_events},
            "screenshots": self.screenshots,
            "notes": self.notes,
            "guest_execution": {
                "counter": self.host.read_state()["counter"],
                "helper_calls": self.host.helper_calls,
                "last_bridge": self.host.read_state()["bridge"],
                "backend": self.backend.name,
                "compat_events": compat_events,
                "native_events": native_events,
            },
        }
