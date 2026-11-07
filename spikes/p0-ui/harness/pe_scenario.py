"""Automated PE-session GUI scenario for compat/native (P0-PE-GUI-01).

The business oracle below is an independent Python implementation of the
fixed ``aevguest.dll`` source semantics (``spikes/p0-engine/testdata/pe/
aevguest.c``).  It does not import or call the runner, engine_host,
engine_bridge, Unicorn, pefile or guest_pe.  The GUI path itself executes the
real DLL through ``libaev_pe_session.dylib`` and every check compares actual
control text / session counts against this oracle.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import threading
from typing import Optional

from .pe_session import (
    AEV_PE_SESSION_ERR_MEMORY,
    AEV_PE_SESSION_ERR_WRONG_THREAD,
    PeSessionError,
)

_RESULT_RE = re.compile(
    r"^\s*(-?\d+)\s*(?:\(rax=0x([0-9a-fA-F]+)\))?", re.MULTILINE
)


def u32(value: int) -> int:
    return int(value) & 0xFFFFFFFF


def s32(value: int) -> int:
    value = u32(value)
    return value - 0x100000000 if value >= 0x80000000 else value


class AevGuestOracle:
    """Independent model of aevguest.c (same fixed DLL, no Host reuse)."""

    def __init__(self) -> None:
        self.seed = 0x1234
        self.table = [3, 1, 4, 1, 5, 9, 2, 6]
        self.cursor_index = 2  # &g_state.table[2]
        self.counter = 0
        self.calls = 0
        self.helper_calls = 0

    def reset(self, seed: int) -> int:
        self.seed = s32(seed)
        for i in range(8):
            self.table[i] = s32(
                (u32(self.seed) + (i << 3) - i) ^ 0x5A5A5A5A
            )
        self.cursor_index = 2
        self.counter = 0
        self.calls = 0
        return self.seed

    def _helper(self, a: int, b: int) -> int:
        # aevhost.dll!AevUpdate: (u32)a * 3 + (u32)b + 1, returns int.
        self.helper_calls += 1
        return s32(u32(a) * 3 + u32(b) + 1)

    def compute(self, a: int, b: int, n: int) -> int:
        a = s32(a)
        b = s32(b)
        n = s32(n)
        acc = u32(self.seed) ^ u32(a)
        for i in range(max(0, n)):
            v = self.table[i & 7]
            if u32(v) & 1:
                acc = u32(u32(acc) + u32(v) + u32(v) + u32(v))
            else:
                acc = u32(u32(acc) - (u32(v) + u32(i)))
            acc = u32((acc << 1) | (acc >> 31))
        if u32(a) & 4:
            acc = u32(u32(acc) + u32(self.table[5]))
        else:
            acc = u32(u32(acc) - u32(self.table[6]))
        # C parameter expression a + g_calls; MSVC builds the fixture /Od and
        # the ABI passes the low 32 bits.  Model it with 32-bit wrapping.
        h = self._helper(s32(u32(a) + u32(self.calls)), b)
        self.counter = (
            self.counter + u32(acc) + u32(h)
        ) & 0xFFFFFFFFFFFFFFFF
        self.calls += 1
        acc = u32(acc ^ u32(self.table[self.cursor_index]))
        return s32(acc)

    def state(self) -> int:
        return s32(u32(self.counter) ^ u32(self.calls))

    def cursor_value(self) -> int:
        return self.table[self.cursor_index]


# Same fixed DLL, reset before each case, as printed by the Windows
# reference EXE (testdata/pe/reference_stdout.txt).  This is a self-check of
# the independent Python model, not an input to the GUI path.
WINDOWS_REFERENCE_CASES = {
    "prep": (11, 4, 8, -940722, -1515459203, 1),
    "hidden": (-9, 2, 17, 861539472, 1761625201, 2),
    "empty": (100, 101, 0, 4677, 1515871214, 3),
    "wrap": (2147483647, 1, 3, 1431572309, -1894889651, 4),
}


def oracle_windows_reference_selfcheck() -> dict:
    oracle = AevGuestOracle()
    oracle.reset(0x1234)
    cursor = oracle.cursor_value()
    cases = []
    all_match = True
    for name, (a, b, n, expected_result, expected_state,
               expected_helper) in WINDOWS_REFERENCE_CASES.items():
        oracle.reset(0x1234)
        result = oracle.compute(a, b, n)
        state = oracle.state()
        calls = oracle.calls
        helper = oracle.helper_calls
        match = (result, state, calls, helper) == (
            expected_result, expected_state, 1, expected_helper
        )
        all_match = all_match and match
        cases.append({
            "name": name, "a": a, "b": b, "n": n,
            "result": result, "state": state, "calls": calls,
            "helper_calls": helper, "match": match,
        })
    return {
        "source": "testdata/pe/reference_stdout.txt (same fixed DLL)",
        "cursor_value_after_reset": cursor,
        "cases": cases,
        "all_match": all_match,
    }


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _hash_or_none(path: Optional[str]) -> Optional[str]:
    return sha256_file(path) if path and os.path.isfile(path) else None


def _as_int(value, field: str) -> int:
    if isinstance(value, bool):
        raise ValueError(f"manifest {field} is a boolean")
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError as exc:
            raise ValueError(f"manifest {field} is not an integer: {value!r}") from exc
    raise ValueError(f"manifest {field} has unsupported type {type(value).__name__}")


def _manifest_blocks(manifest: dict, key: str) -> Optional[list[int]]:
    value = manifest.get(key)
    if value is None:
        return None
    if not isinstance(value, list):
        raise ValueError(f"manifest {key} is not a list")
    return [_as_int(item, f"{key}[{i}]") for i, item in enumerate(value)]


def analyze_trace(path: str, dll_sha256: str, load_base: int,
                  aot_sha256: Optional[str]) -> dict:
    records = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            records.append(json.loads(line))
    if not records or records[0].get("record") != "header":
        raise AssertionError(f"trace {path} has no ABI 4 header")
    header = records[0]
    events = records[1:]
    counts: dict[str, int] = {}
    for event in events:
        kind = str(event.get("record"))
        counts[kind] = counts.get(kind, 0) + 1
    jit_compile_rips = {int(e["rip"], 16) for e in events
                        if e.get("record") == "jit_compile" and "rip" in e}
    jit_exec_rips = {int(e["rip"], 16) for e in events
                     if e.get("record") == "jit_exec" and "rip" in e}
    aot_rips = {int(e["rip"], 16) for e in events
                if e.get("record") == "aot" and "rip" in e}
    result = {
        "path": path,
        "sha256": sha256_file(path),
        "size": os.path.getsize(path),
        "header": header,
        "event_counts": counts,
        "jit_compile_rips": [f"0x{x:x}" for x in sorted(jit_compile_rips)],
        "jit_exec_rips": [f"0x{x:x}" for x in sorted(jit_exec_rips)],
        "aot_rips": [f"0x{x:x}" for x in sorted(aot_rips)],
        "jit_compile_events": len(jit_compile_rips),
        "jit_exec_events": len(jit_exec_rips),
        "aot_events": len(aot_rips),
    }
    if int(header.get("trace_abi", 0)) != 4:
        raise AssertionError(f"trace ABI {header.get('trace_abi')} != 4")
    if header.get("module_kind") != "pe32+amd64-dll":
        raise AssertionError(
            f"trace module kind {header.get('module_kind')!r} is not PE"
        )
    if str(header.get("input_sha256") or "").lower() != dll_sha256:
        raise AssertionError("trace input SHA does not match the fixed DLL")
    if _as_int(header.get("load_base", -1), "header.load_base") != load_base:
        raise AssertionError("trace load_base does not match the requested base")
    header_aot = str(header.get("aot_sha256") or "").lower()
    if aot_sha256 and header_aot and header_aot != aot_sha256:
        raise AssertionError("trace aot_sha256 does not match the supplied AOT")
    selected = {int(x, 16) for x in header.get("aot_selected", [])}
    result["aot_selected_rips"] = [f"0x{x:x}" for x in sorted(selected)]
    result["aot_selected_set"] = selected
    result["jit_compile_set"] = jit_compile_rips
    result["jit_exec_set"] = jit_exec_rips
    result["aot_set"] = aot_rips
    return result


class PeScenario:
    CASES = [
        ("prep", 11, 4, 8, None),
        ("hidden", -9, 2, 17, None),
        ("empty", 100, 101, 0, None),
        # Held-out input, not part of the fixture preparation/reference set.
        ("independent", 0x13572468, 0x2468, 5, ("0x13572468", "0x2468", "5")),
    ]

    def __init__(self, host, backend, logger, evidence_dir: str,
                 dll: str, registry: str, aot: str, load_base: int,
                 coverage_profile: str = "base",
                 base_aot: Optional[str] = None,
                 manifest: Optional[dict] = None,
                 manifest_path: Optional[str] = None,
                 trace_path: Optional[str] = None,
                 entry_export: Optional[str] = "AevDllCompute"):
        self.host = host
        self.backend = backend
        self.logger = logger
        self.evidence_dir = evidence_dir
        self.dll = dll
        self.registry = registry
        self.aot = aot
        self.load_base = int(load_base)
        self.coverage_profile = coverage_profile
        self.base_aot = base_aot
        self.manifest = manifest
        self.manifest_path = manifest_path
        self.trace_path = trace_path
        self.entry_export = entry_export
        self.checks: dict[str, bool] = {}
        self.states: dict[str, dict] = {}
        self.negative: dict[str, dict] = {}
        self.screenshots: list[str] = []
        self.notes: list[str] = []
        self.trace_proof: Optional[dict] = None
        self.oracle_record: dict = {}

    # -- helpers -------------------------------------------------------
    def _check(self, name: str, condition: bool, detail: str = "") -> None:
        self.checks[name] = bool(condition)
        self.logger.event("pe.check", name=name, pass_=bool(condition),
                          detail=detail)
        if not condition:
            raise AssertionError(f"check failed: {name} {detail}")

    def _require(self, condition: bool, name: str, detail: str = "") -> None:
        if not condition:
            self.checks[name] = False
            self.logger.event("pe.check", name=name, pass_=False, detail=detail)
            raise AssertionError(f"check failed: {name} {detail}")
        self.checks[name] = True

    def _snap(self, label: str) -> dict:
        state = self.host.read_state()
        self.states[label] = state
        self.logger.event("pe.state", label=label, state=state)
        return state

    def _screenshot(self, name: str) -> None:
        try:
            self.backend.pump(0.1)
        except Exception:  # noqa: BLE001
            pass
        path = self.host.screenshot(name)
        if path:
            self.screenshots.append(path)

    @staticmethod
    def _parse_result(text: str) -> tuple[int, Optional[int]]:
        match = _RESULT_RE.match(text)
        if not match:
            raise AssertionError(f"result control is not numeric: {text!r}")
        value = int(match.group(1))
        raw = int(match.group(2), 16) if match.group(2) else None
        return value, raw

    def _set_inputs(self, a: int, b: int, n: int,
                    literal: Optional[tuple[str, str, str]] = None) -> None:
        if literal is None:
            literal = (str(a), str(b), str(n))
        self.backend.set_edit_text(self.host.a_handle, literal[0])
        self.backend.set_edit_text(self.host.b_handle, literal[1])
        self.backend.set_edit_text(self.host.n_handle, literal[2])

    def _check_business(self, label: str, expected: int,
                        oracle: AevGuestOracle, state: dict,
                        session_handle: str, create_count: int,
                        previous_calls_total: Optional[int] = None,
                        expected_calls_delta: int = 3) -> None:
        result, raw = self._parse_result(state["result_text"])
        self._check(f"{label}_result_text", result == expected,
                    f"{result} != {expected}")
        self._check(f"{label}_state", state["state_text"].strip() == str(oracle.state()),
                    f"{state['state_text']} != {oracle.state()}")
        self._check(f"{label}_calls", state["calls_text"].strip() == str(oracle.calls),
                    f"{state['calls_text']} != {oracle.calls}")
        counts = state.get("counts") or {}
        self._check(f"{label}_load_count", counts.get("load_count") == 1,
                    str(counts.get("load_count")))
        self._check(f"{label}_host_imports",
                    counts.get("host_import_calls") == oracle.helper_calls,
                    f"{counts.get('host_import_calls')} != {oracle.helper_calls}")
        self._check(f"{label}_session_once", state.get("session_create_count") == 1,
                    str(state.get("session_create_count")))
        self._check(f"{label}_session_handle",
                    state.get("session_handle") == session_handle,
                    f"{state.get('session_handle')} != {session_handle}")
        self._check(f"{label}_status_ok", state.get("last_status") == 0,
                    str(state.get("last_status")))
        self._check(f"{label}_status_name", state.get("last_status_name") == "OK",
                    str(state.get("last_status_name")))
        last = state.get("last_result") or {}
        self._check(f"{label}_last_result", last.get("rax_i32") == expected,
                    f"{last.get('rax_i32')} != {expected}")
        if raw is not None:
            self._check(
                f"{label}_raw_diagnostic",
                s32(raw & 0xFFFFFFFF) == expected,
                f"raw 0x{raw:x} low32 {s32(raw & 0xFFFFFFFF)} != {expected}",
            )
        if previous_calls_total is not None:
            self._check(
                f"{label}_calls_total_delta",
                counts.get("calls_total") == previous_calls_total + expected_calls_delta,
                f"{counts.get('calls_total')} != "
                f"{previous_calls_total + expected_calls_delta}",
            )

    # -- trace / manifest / thread checks -----------------------------
    def _analyze_trace(self) -> Optional[dict]:
        if not self.trace_path:
            return None
        counts = self.host.close_trace()
        if not os.path.isfile(self.trace_path):
            raise AssertionError(f"trace file was not written: {self.trace_path}")
        proof = analyze_trace(
            self.trace_path, sha256_file(self.dll), self.load_base,
            _hash_or_none(self.aot),
        )
        proof["counts_at_close"] = counts
        self.trace_proof = proof
        self._check("trace_abi_4", int(proof["header"].get("trace_abi", 0)) == 4)
        self._check("trace_input_sha",
                    str(proof["header"].get("input_sha256") or "").lower()
                    == sha256_file(self.dll))
        self._check("trace_load_base",
                    _as_int(proof["header"].get("load_base", -1), "load_base")
                    == self.load_base)
        session_selected = set(self.host.session.selected_rips())
        self._check("trace_aot_selected_matches_session",
                    set(proof.get("aot_selected_set") or set()) == session_selected,
                    f"trace={sorted(proof.get('aot_selected_set') or set())} "
                    f"session={sorted(session_selected)}")
        if self.coverage_profile == "base":
            self._check("trace_base_has_jit_compile",
                        proof["jit_compile_events"] > 0,
                        str(proof["jit_compile_events"]))
            self._check("trace_base_has_jit_exec",
                        proof["jit_exec_events"] > 0,
                        str(proof["jit_exec_events"]))
        else:
            self._check("trace_supplement_zero_jit_compile",
                        proof["jit_compile_events"] == 0,
                        str(proof["jit_compile_events"]))
            self._check("trace_supplement_zero_jit_exec",
                        proof["jit_exec_events"] == 0,
                        str(proof["jit_exec_events"]))
            self._check("trace_supplement_has_aot", proof["aot_events"] > 0,
                        str(proof["aot_events"]))
            promoted = _manifest_blocks(self.manifest or {}, "promoted_blocks") or []
            promoted_set = set(promoted)
            selected = set(proof.get("aot_selected_set") or set())
            if promoted_set:
                self._check(
                    "trace_promoted_aot_executed",
                    bool(promoted_set & (proof.get("aot_set") or set())),
                    "no promoted rip executed as AOT",
                )
                self._check(
                    "trace_aot_events_selected",
                    (proof.get("aot_set") or set()).issubset(selected),
                    f"aot {sorted(proof.get('aot_set') or [])} not selected",
                )
        for key in ("aot_selected_set", "jit_compile_set", "jit_exec_set",
                    "aot_set"):
            proof.pop(key, None)
        return proof

    def _validate_manifest(self) -> None:
        if self.coverage_profile != "supplemented":
            return
        manifest = self.manifest
        if not isinstance(manifest, dict):
            raise AssertionError("supplemented profile has no manifest")
        info = self.host.session_info
        dll_sha = sha256_file(self.dll)
        aot_sha = sha256_file(self.aot)
        selected = [int(x) for x in self.host.session.selected_rips()]

        if "aot_sha256" in manifest:
            self._check("manifest_aot_sha",
                        str(manifest["aot_sha256"]).lower() == aot_sha,
                        "manifest aot_sha256 does not match dylib")
        if "base_aot_sha256" in manifest:
            base_sha = _hash_or_none(self.base_aot)
            self._check("manifest_base_aot_sha",
                        base_sha is not None and
                        str(manifest["base_aot_sha256"]).lower() == base_sha,
                        "manifest base_aot_sha256 does not match --base-aot")
        for key in ("input_sha256", "module_file_sha256"):
            if key in manifest:
                self._check(f"manifest_{key}",
                            str(manifest[key]).lower() == dll_sha,
                            f"manifest {key} does not match DLL")
        if "module_file_size" in manifest:
            self._check("manifest_file_size",
                        _as_int(manifest["module_file_size"], "module_file_size")
                        == os.path.getsize(self.dll), "file size mismatch")
        if "module_kind" in manifest:
            self._check("manifest_module_kind",
                        manifest["module_kind"] == info.get("module_kind"),
                        f"{manifest['module_kind']} != {info.get('module_kind')}")
        for key, info_key in (
            ("image_sha256", "image_sha256"),
            ("import_binding_sha256", "import_binding_sha256"),
            ("host_binding_sha256", "host_binding_sha256"),
            ("entry_exports_sha256", "entry_exports_sha256"),
        ):
            if key in manifest:
                self._check(f"manifest_{key}",
                            str(manifest[key]).lower()
                            == str(info.get(info_key, "")).lower(),
                            f"manifest {key} != session info")
        if "entry_export" in manifest:
            self._check("manifest_entry_export",
                        manifest["entry_export"] == info.get("entry_export"),
                        f"{manifest['entry_export']} != {info.get('entry_export')}")
        if "target" in manifest:
            self._check("manifest_target",
                        manifest["target"] == info.get("target_triple"),
                        f"{manifest['target']} != {info.get('target_triple')}")
        for key, info_key in (
            ("load_base", "load_base"),
            ("preferred_base", "preferred_base"),
            ("image_size", "image_size"),
            ("thunk_base", "thunk_base"),
            ("import_count", "import_count"),
            ("relocs_stripped", "relocs_stripped"),
            ("all_block_count", "all_block_count"),
        ):
            if key in manifest:
                self._check(f"manifest_{key}",
                            _as_int(manifest[key], key) == int(info.get(info_key, -1)),
                            f"manifest {key} != session info {info_key}")
        selected_manifest = _manifest_blocks(manifest, "selected_blocks")
        if selected_manifest is not None:
            self._check("manifest_selected_blocks",
                        selected_manifest == sorted(selected),
                        f"{selected_manifest} != {sorted(selected)}")
        promoted = _manifest_blocks(manifest, "promoted_blocks") or []
        demoted = _manifest_blocks(manifest, "demoted_blocks") or []
        if promoted:
            self._check("manifest_promoted_selected",
                        set(promoted).issubset(set(selected)),
                        "promoted block is not AOT-selected")
            self._check("manifest_promoted_nonempty",
                        bool(set(promoted)),
                        "promoted set is empty")
        base_selected = _manifest_blocks(manifest, "base_selected_blocks")
        if base_selected is not None:
            self._check("manifest_demoted_base",
                        set(demoted).issubset(set(base_selected)),
                        "demoted block is not base-selected")
            self._check("manifest_promoted_not_base",
                        not (set(promoted) & set(base_selected)),
                        "promoted block was already base-selected")
        self.logger.event(
            "pe.manifest_validated",
            manifest_path=self.manifest_path,
            selected=len(selected), promoted=len(promoted),
            demoted=len(demoted),
        )

    def _wrong_thread_check(self) -> None:
        if self.host.session is None:
            return
        captured: dict = {}

        def worker() -> None:
            try:
                self.host.session.counts()
                captured["status"] = None
            except PeSessionError as exc:
                captured["status"] = exc.status
            except BaseException as exc:  # noqa: BLE001
                captured["error"] = repr(exc)

        thread = threading.Thread(target=worker, name="pe-wrong-thread-probe")
        thread.start()
        thread.join(timeout=5.0)
        self._check("wrong_thread_probe_finished", not thread.is_alive(),
                    "worker thread did not finish")
        self._check("wrong_thread_probe_status",
                    captured.get("status") == AEV_PE_SESSION_ERR_WRONG_THREAD,
                    f"captured={captured}")
        self.logger.event("pe.wrong_thread_probe", captured=captured)

    # -- run -----------------------------------------------------------
    def run(self) -> dict:
        self.logger.event(
            "pe.scenario_begin", backend=self.backend.name,
            coverage_profile=self.coverage_profile,
            entry_export=self.entry_export,
            trace_path=self.trace_path,
        )
        self.backend.pump(0.35)
        self._screenshot(f"01-initial-{self.backend.name}.png")
        initial = self._snap("initial")
        initial_handle = initial["session_handle"]
        initial_counts = initial.get("counts") or {}
        info = initial.get("session_info") or {}
        self._check("initial_state_zero", initial["state_text"].strip() == "0",
                    initial["state_text"])
        self._check("initial_calls_zero", initial["calls_text"].strip() == "0",
                    initial["calls_text"])
        self._check("initial_load_text",
                    initial["load_text"] == "load_count=1",
                    initial["load_text"])
        self._check("initial_counts_load",
                    initial_counts.get("load_count") == 1,
                    str(initial_counts.get("load_count")))
        self._check("initial_session_once",
                    initial.get("session_create_count") == 1,
                    str(initial.get("session_create_count")))
        self._check("initial_main_thread",
                    bool(initial.get("main_thread")) and
                    bool(self.host.created_on_main_thread),
                    f"main={initial.get('main_thread')} "
                    f"created={self.host.created_on_main_thread}")
        self._check("initial_module_kind",
                    info.get("module_kind") == "pe32+amd64-dll",
                    str(info.get("module_kind")))
        self._check("initial_entry_export",
                    info.get("entry_export") == self.entry_export,
                    f"{info.get('entry_export')} != {self.entry_export}")
        self._check("initial_dll_sha",
                    info.get("file_sha256") == sha256_file(self.dll),
                    "session file SHA does not match the input DLL")
        self._check("initial_load_base", int(info.get("load_base", -1)) == self.load_base,
                    f"{info.get('load_base')} != {self.load_base}")
        self._check("initial_guest_abi",
                    int(info.get("guest_abi_version", -1)) == 2 and
                    int(info.get("guest_state_size", -1)) == 296,
                    f"{info.get('guest_abi_version')}/"
                    f"{info.get('guest_state_size')}")
        self._check("initial_registry_bound",
                    info.get("host_binding_sha256") not in (None, "", "0" * 64),
                    str(info.get("host_binding_sha256")))
        self._check("initial_trace_state",
                    int(initial_counts.get("trace_open", 0)) ==
                    (1 if self.trace_path else 0),
                    f"trace_open={initial_counts.get('trace_open')} "
                    f"trace={self.trace_path}")

        # Host-side int32 rejection: invalid input must not enter the Guest.
        invalid_before = self.host.read_state()
        self.backend.set_edit_text(self.host.a_handle, "0x80000000")
        self.backend.set_edit_text(self.host.b_handle, "1")
        self.backend.set_edit_text(self.host.n_handle, "1")
        self.backend.click_button(self.host.compute_handle)
        invalid_after = self._snap("invalid_input")
        invalid_before_counts = invalid_before.get("counts") or {}
        invalid_after_counts = invalid_after.get("counts") or {}
        self.negative["invalid_input"] = {
            "input": {"a": "0x80000000", "b": "1", "n": "1"},
            "status": invalid_after.get("last_status"),
            "status_name": invalid_after.get("last_status_name"),
            "error": invalid_after.get("last_error"),
            "counts_unchanged": (
                invalid_after_counts.get("calls_total") ==
                invalid_before_counts.get("calls_total")
            ),
            "result_unchanged": (
                invalid_after["result_text"] == invalid_before["result_text"]
            ),
            "state_unchanged": (
                invalid_after["state_text"] == invalid_before["state_text"]
            ),
            "calls_unchanged": (
                invalid_after["calls_text"] == invalid_before["calls_text"]
            ),
        }
        self._check("invalid_input_host_status",
                    invalid_after.get("last_status") is None and
                    invalid_after.get("last_status_name") == "HOST_INPUT",
                    f"{invalid_after.get('last_status')}/"
                    f"{invalid_after.get('last_status_name')}")
        self._check("invalid_input_message",
                    "HOST_INPUT" in invalid_after["status_text"].upper(),
                    invalid_after["status_text"])
        self._check("invalid_input_no_guest",
                    invalid_after_counts.get("calls_total") ==
                    invalid_before_counts.get("calls_total"),
                    f"{invalid_after_counts.get('calls_total')} != "
                    f"{invalid_before_counts.get('calls_total')}")
        self._check("invalid_input_result_kept",
                    invalid_after["result_text"] == invalid_before["result_text"])
        self._check("invalid_input_state_kept",
                    invalid_after["state_text"] == invalid_before["state_text"])
        self._check("invalid_input_calls_kept",
                    invalid_after["calls_text"] == invalid_before["calls_text"])

        oracle = AevGuestOracle()
        reference_check = oracle_windows_reference_selfcheck()
        self._check("oracle_matches_windows_reference",
                    bool(reference_check["all_match"]),
                    "Python oracle differs from the Windows reference cases")
        self.oracle_record = {
            "seed": 0x1234,
            "table": list(oracle.table),
            "windows_reference": reference_check,
            "cases": [],
        }

        # Reset(0x1234): explicit AevDllReset call, not a session reload.
        self.backend.set_edit_text(self.host.seed_handle, "0x1234")
        self.backend.click_button(self.host.reset_handle)
        oracle.reset(0x1234)
        self.oracle_record["table_after_reset"] = list(oracle.table)
        reset_state = self._snap("reset")
        self._check_business("reset", 0x1234, oracle, reset_state,
                             initial_handle, 1,
                             previous_calls_total=initial_counts.get("calls_total"),
                             expected_calls_delta=3)
        self.negative["reset"] = {"seed": 0x1234, "load_count":
                                  (reset_state.get("counts") or {}).get("load_count")}
        self.oracle_record["reset_state"] = oracle.state()
        self.oracle_record["reset_calls"] = oracle.calls
        self._check("reset_helper_zero",
                    (reset_state.get("counts") or {}).get("host_import_calls") == 0,
                    str((reset_state.get("counts") or {}).get("host_import_calls")))
        previous_calls_total = (reset_state.get("counts") or {}).get("calls_total")

        # Three prepared cases plus one held-out input.
        for label, a, b, n, literal in self.CASES:
            self._set_inputs(a, b, n, literal)
            expected = oracle.compute(a, b, n)
            self.backend.click_button(self.host.compute_handle)
            state = self._snap(f"compute_{label}")
            self._check_business(label, expected, oracle, state,
                                 initial_handle, 1,
                                 previous_calls_total=previous_calls_total,
                                 expected_calls_delta=3)
            self._check(f"{label}_call_index",
                        (state.get("last_result") or {}).get("call_index", 0) >= 1,
                        str((state.get("last_result") or {}).get("call_index")))
            self.oracle_record["cases"].append({
                "label": label, "a": a, "b": b, "n": n,
                "result": expected, "state": oracle.state(),
                "calls": oracle.calls, "helper_calls": oracle.helper_calls,
                "result_text": state["result_text"],
                "state_text": state["state_text"],
                "calls_text": state["calls_text"],
            })
            previous_calls_total = (state.get("counts") or {}).get("calls_total")
        self.backend.pump(0.2)
        self._screenshot(f"02-after-inputs-{self.backend.name}.png")

        # 100 consecutive real button activations on the same session.
        loop_a, loop_b, loop_n = 5, 7, 3
        self._set_inputs(loop_a, loop_b, loop_n)
        loop_start = self.host.read_state()
        loop_start_counts = loop_start.get("counts") or {}
        loop_start_calls = oracle.calls
        loop_states: dict[str, dict] = {}
        loop_result = None
        for i in range(100):
            expected = oracle.compute(loop_a, loop_b, loop_n)
            self.backend.click_button(self.host.compute_handle)
            state = self.host.read_state()
            self._require(state.get("last_status") == 0,
                          f"loop_100_status_{i}", str(state.get("last_status")))
            result, _raw = self._parse_result(state["result_text"])
            self._require(result == expected, f"loop_100_result_{i}",
                          f"{result} != {expected}")
            self._require(state["state_text"].strip() == str(oracle.state()),
                          f"loop_100_state_{i}",
                          f"{state['state_text']} != {oracle.state()}")
            self._require(state["calls_text"].strip() == str(oracle.calls),
                          f"loop_100_calls_{i}",
                          f"{state['calls_text']} != {oracle.calls}")
            if i % 10 == 0:
                counts = state.get("counts") or {}
                self._require(counts.get("load_count") == 1,
                              f"loop_100_load_{i}",
                              str(counts.get("load_count")))
                self._require(state.get("session_handle") == initial_handle,
                              f"loop_100_handle_{i}",
                              f"{state.get('session_handle')} != {initial_handle}")
            if i % 25 == 0 or i == 99:
                label = f"loop_{i:03d}"
                loop_states[label] = state
                self.states[label] = state
                self.logger.event("pe.state", label=label, state=state)
            loop_result = expected
        self.backend.pump(0.2)
        loop_end = self.host.read_state()
        loop_counts = loop_end.get("counts") or {}
        self._check("loop_100_calls", oracle.calls == loop_start_calls + 100,
                    f"{oracle.calls} != {loop_start_calls + 100}")
        self._check("loop_100_load_count", loop_counts.get("load_count") == 1,
                    str(loop_counts.get("load_count")))
        self._check("loop_100_session_once",
                    loop_end.get("session_create_count") == 1,
                    str(loop_end.get("session_create_count")))
        self._check("loop_100_same_handle",
                    loop_end.get("session_handle") == initial_handle,
                    f"{loop_end.get('session_handle')} != {initial_handle}")
        expected_loop_calls_total = (
            loop_start_counts.get("calls_total") + 100 * 3
        )
        self._check("loop_100_calls_total",
                    loop_counts.get("calls_total") == expected_loop_calls_total,
                    f"{loop_counts.get('calls_total')} != {expected_loop_calls_total}")
        self._check("loop_100_host_imports",
                    loop_counts.get("host_import_calls") == oracle.helper_calls,
                    f"{loop_counts.get('host_import_calls')} != {oracle.helper_calls}")
        self._check("loop_100_final_result",
                    self._parse_result(loop_end["result_text"])[0] == loop_result,
                    f"{loop_end['result_text']} != {loop_result}")
        self._check("loop_100_final_state",
                    loop_end["state_text"].strip() == str(oracle.state()),
                    f"{loop_end['state_text']} != {oracle.state()}")
        self._check("loop_100_final_calls",
                    loop_end["calls_text"].strip() == str(oracle.calls),
                    f"{loop_end['calls_text']} != {oracle.calls}")
        self._screenshot(f"03-after-100-{self.backend.name}.png")

        # Negative real-button path: AevDllReadAt(0xDEAD0000) -> MEMORY.
        before = self.host.read_state()
        self.backend.set_edit_text(self.host.bad_addr_handle, "0xDEAD0000")
        self.backend.click_button(self.host.bad_button_handle)
        after = self._snap("negative_readat")
        self.negative["readat_invalid"] = {
            "address": "0xDEAD0000",
            "status": after.get("last_status"),
            "status_name": after.get("last_status_name"),
            "error": after.get("last_error"),
            "result_unchanged": after["result_text"] == before["result_text"],
            "state_unchanged": after["state_text"] == before["state_text"],
            "calls_unchanged": after["calls_text"] == before["calls_text"],
            "load_count": (after.get("counts") or {}).get("load_count"),
            "note": "test-only invalid address",
        }
        self._check("negative_status_memory",
                    after.get("last_status") == AEV_PE_SESSION_ERR_MEMORY,
                    str(after.get("last_status")))
        self._check("negative_status_name",
                    after.get("last_status_name") == "MEMORY",
                    str(after.get("last_status_name")))
        self._check("negative_error_visible",
                    "MEMORY" in after["status_text"].upper() and
                    "ERROR" in after["status_text"].upper(),
                    after["status_text"])
        self._check("negative_result_not_committed",
                    after["result_text"] == before["result_text"],
                    f"{after['result_text']} != {before['result_text']}")
        self._check("negative_state_not_committed",
                    after["state_text"] == before["state_text"],
                    f"{after['state_text']} != {before['state_text']}")
        self._check("negative_calls_not_committed",
                    after["calls_text"] == before["calls_text"],
                    f"{after['calls_text']} != {before['calls_text']}")
        self._check("negative_load_count",
                    (after.get("counts") or {}).get("load_count") == 1,
                    str((after.get("counts") or {}).get("load_count")))
        self._check("negative_same_session",
                    after.get("session_handle") == initial_handle,
                    f"{after.get('session_handle')} != {initial_handle}")
        before_last = before.get("last_result") or {}
        after_last = after.get("last_result") or {}
        self._check("negative_last_result_kept",
                    before_last.get("rax") == after_last.get("rax") and
                    before_last.get("rax_i32") == after_last.get("rax_i32"),
                    f"{after_last} != {before_last}")
        self._screenshot(f"04-negative-{self.backend.name}.png")

        # Recovery: a normal Compute after the fault must commit again.
        self._set_inputs(7, 3, 2)
        expected = oracle.compute(7, 3, 2)
        self.backend.click_button(self.host.compute_handle)
        recovery = self._snap("recovery")
        self._check_business("recovery", expected, oracle, recovery,
                             initial_handle, 1,
                             previous_calls_total=(after.get("counts") or {}).get(
                                 "calls_total"),
                             expected_calls_delta=3)
        self._screenshot(f"05-final-{self.backend.name}.png")

        # Coverage evidence from the live session (not a Host shadow count).
        final_counts = (recovery.get("counts") or {})
        if self.coverage_profile == "base":
            self._check("base_real_jit_compile",
                        final_counts.get("jit_compiles_total", 0) > 0,
                        str(final_counts.get("jit_compiles_total")))
            self._check("base_real_jit_exec",
                        final_counts.get("jit_execs_total", 0) > 0,
                        str(final_counts.get("jit_execs_total")))
        else:
            self._check("supplement_zero_jit_compile",
                        final_counts.get("jit_compiles_total", 0) == 0,
                        str(final_counts.get("jit_compiles_total")))
            self._check("supplement_zero_jit_exec",
                        final_counts.get("jit_execs_total", 0) == 0,
                        str(final_counts.get("jit_execs_total")))
            self._check("supplement_aot_blocks",
                        final_counts.get("aot_blocks_total", 0) > 0,
                        str(final_counts.get("aot_blocks_total")))
            self._check("supplement_aot_present",
                        int(info.get("aot_present", 0)) == 1,
                        str(info.get("aot_present")))
            self._validate_manifest()

        trace_proof = self._analyze_trace()
        self._wrong_thread_check()

        # Anti-substitution / backend proof.
        self._check("no_unicorn", "unicorn" not in sys.modules)
        self._check("no_pefile", "pefile" not in sys.modules)
        self._check("no_guest_pe", "harness.guest_pe" not in sys.modules)
        self._check("no_engine_host", "harness.engine_host" not in sys.modules)
        self._check("no_engine_bridge", "harness.engine_bridge" not in sys.modules)
        backend_proof = self.backend.proof()
        compat_events = sum(v for k, v in self.logger.kind_counts.items()
                            if k.startswith("compat."))
        native_events = sum(v for k, v in self.logger.kind_counts.items()
                            if k.startswith("native."))
        self._check("backend_main_thread", bool(backend_proof.get("main_thread")))
        if self.backend.name == "native":
            self._check("native_backend_used", native_events > 0,
                        f"native_events={native_events}")
            self._check("native_no_compat_events", compat_events == 0,
                        f"compat_events={compat_events}")
            self._check("native_compat_classes_unused",
                        backend_proof.get("compat_classes_used") is False)
            self._check("native_control_classes",
                        all(not str(c.get("objc_class", "")).startswith("Compat")
                            for c in backend_proof.get("controls", [])))
        else:
            self._check("compat_backend_used", compat_events > 0,
                        f"compat_events={compat_events}")
            self._check("compat_no_native_events", native_events == 0,
                        f"native_events={native_events}")
            self._check("compat_classes",
                        all(str(c.get("objc_class", "")).startswith("Compat")
                            for c in backend_proof.get("controls", [])))

        session_proof = self.host.session_proof()
        self._check("final_load_count",
                    (session_proof.get("counts") or {}).get("load_count") == 1,
                    str((session_proof.get("counts") or {}).get("load_count")))
        self._check("session_created_once", self.host.create_count == 1,
                    str(self.host.create_count))
        self._check("dll_sha_session",
                    info.get("file_sha256") == sha256_file(self.dll),
                    "session info file SHA changed")
        if self.aot:
            self._check("aot_present", int(info.get("aot_present", 0)) == 1,
                        str(info.get("aot_present")))
            self._check("aot_sha_matches",
                        info.get("aot_sha256") == sha256_file(self.aot),
                        "session AOT sha does not match the supplied dylib")

        self.logger.event(
            "pe.scenario_end", checks=self.checks,
            screenshots=self.screenshots,
            coverage_profile=self.coverage_profile,
        )
        return self.summary(backend_proof, session_proof, trace_proof,
                            compat_events, native_events, info, final_counts,
                            loop_states)

    # -- summary -------------------------------------------------------
    def summary(self, backend_proof: dict, session_proof: dict,
                trace_proof: Optional[dict], compat_events: int,
                native_events: int, info: dict, final_counts: dict,
                loop_states: dict) -> dict:
        return {
            "success": all(self.checks.values()) and bool(self.checks) and
                       bool(self.screenshots),
            "coverage_profile": self.coverage_profile,
            "checks": self.checks,
            "check_count": len(self.checks),
            "states": self.states,
            "negative": self.negative,
            "oracle": self.oracle_record,
            "backend_proof": backend_proof,
            "session_proof": session_proof,
            "trace_proof": trace_proof,
            "backend_event_counts": {"native": native_events,
                                     "compat": compat_events},
            "screenshots": self.screenshots,
            "notes": self.notes,
            "loop_states": loop_states,
            "guest_execution": {
                "backend": self.backend.name,
                "coverage_profile": self.coverage_profile,
                "session_handle": session_proof.get("session_handle"),
                "load_count": final_counts.get("load_count"),
                "calls_total": final_counts.get("calls_total"),
                "host_import_calls": final_counts.get("host_import_calls"),
                "aot_blocks_total": final_counts.get("aot_blocks_total"),
                "jit_blocks_total": final_counts.get("jit_blocks_total"),
                "jit_compiles_total": final_counts.get("jit_compiles_total"),
                "jit_execs_total": final_counts.get("jit_execs_total"),
                "trace_records": final_counts.get("trace_records"),
                "module_kind": info.get("module_kind"),
                "entry_export": info.get("entry_export"),
                "aot_sha256": info.get("aot_sha256"),
                "compat_events": compat_events,
                "native_events": native_events,
            },
        }
