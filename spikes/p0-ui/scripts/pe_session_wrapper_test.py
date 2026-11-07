#!/usr/bin/env python3
"""P0-PE-SESSION-FIX-01/F1: headless observer lifetime regression.

This test drives the real ``harness.pe_session.PeSession`` ctypes wrapper
against the real ``libaev_pe_session.dylib`` and the fixed PE fixture
``spikes/p0-engine/testdata/pe/aevguest.dll`` (load base ``0x180000000``).

It covers the observer ownership contract in PE-SESSION-CONTRACT-20261107
section 3.5:

* rejected non-owner ``clear_observer``/``set_observer`` calls return
  ``WRONG_THREAD=13`` and must keep the previous Python callbacks strongly
  referenced by the wrapper (otherwise the C ABI keeps dangling pointers);
* rejected reentrant ``clear_observer``/``set_observer`` calls made from
  ``on_call_end``/``on_host_import`` return ``REENTRY=10`` and must also keep
  the previous binding;
* successful replacement/clear/destroy release the old callback objects;
* repeated create/clear and GC pressure do not grow a keep-alive list.

The test uses only the standard library (no venv/PyObjC).  It intentionally
does not keep its own long-lived strong reference to a callback: liveness is
asserted with ``weakref`` plus the wrapper's own ``_observer_callbacks``
state.  On the old wrapper the first rejected clear/set already drops the
old callback, so the test stops before invoking Guest code with a dangling
C function pointer, safely clears/destroys on the owner thread, and exits
non-zero with the failure recorded.
"""
from __future__ import annotations

import argparse
import gc
import hashlib
import json
import sys
import threading
import weakref
from pathlib import Path
from typing import Callable, Iterable


LOAD_BASE = 0x180000000
SEED = 0x1234
MAX_U32 = 0xFFFFFFFF


def u32(value: int) -> int:
    return value & MAX_U32


def s32(value: int) -> int:
    value &= MAX_U32
    return value - 0x100000000 if value & 0x80000000 else value


class Oracle:
    """Independent model of spikes/p0-engine/testdata/pe/aevguest.c."""

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


class AbortRun(Exception):
    """Stop before Guest execution could dereference a released callback."""


class Checks:
    def __init__(self) -> None:
        self.items: list[dict] = []
        self.passed = 0
        self.failed = 0

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        ok = bool(ok)
        self.items.append({"name": name, "ok": ok, "detail": detail})
        if ok:
            self.passed += 1
            print(f"PASS  {name}")
        else:
            self.failed += 1
            suffix = f" [{detail}]" if detail else ""
            print(f"FAIL  {name}{suffix}")
        return ok

    def require(self, name: str, ok: bool, detail: str = "") -> None:
        if not self.check(name, ok, detail):
            raise AbortRun(name)

    def summary(self) -> dict:
        return {
            "success": self.failed == 0,
            "checks_total": len(self.items),
            "checks_passed": self.passed,
            "checks_failed": self.failed,
            "checks": self.items,
        }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def capture_status(fn: Callable[[], None]):
    """Run fn; return its status code or a diagnostic string."""
    try:
        fn()
        return 0
    except PeSessionError as exc:  # noqa: F821 - imported after path setup
        return int(exc.status)
    except BaseException as exc:  # noqa: BLE001 - reported as a failed check
        return f"exception:{type(exc).__name__}:{exc}"


def run_in_thread(fn: Callable[[], None]) -> list[object]:
    box: list[object] = []

    def target() -> None:
        box.append(capture_status(fn))

    thread = threading.Thread(target=target, name="wrapper-wrong-thread")
    thread.start()
    thread.join()
    return box


def observer_snapshot(session) -> tuple[list[weakref.ref], weakref.ref | None]:
    """Weak references only; never pin the wrapper's callbacks."""
    callbacks = [weakref.ref(cb) for cb in session._observer_callbacks]
    observer_ref = weakref.ref(session._observer) if session._observer else None
    return callbacks, observer_ref


def wrapper_holds(session, callbacks: list[weakref.ref],
                  observer_ref: weakref.ref | None) -> tuple[bool, str]:
    current = session._observer_callbacks
    alive = [ref() is not None for ref in callbacks]
    matched = (
        len(current) == len(callbacks)
        and all(alive)
        and all(ref() is cb for ref, cb in zip(callbacks, current))
    )
    observer_alive = bool(
        observer_ref is not None
        and observer_ref() is not None
        and session._observer is observer_ref()
    )
    detail = (
        f"alive={alive} wrapper_callbacks={len(current)} "
        f"observer_alive={observer_alive}"
    )
    return matched and observer_alive, detail


def install_counting(session, config: str, events: dict, tag: str):
    kwargs = {}
    if config in ("call_end", "both"):
        kwargs["on_call_end"] = (
            lambda result: events[tag + "_call_end"].append(
                result["call_index"]
            )
        )
    if config in ("host_import", "both"):
        kwargs["on_host_import"] = (
            lambda _ctx, host_id, thunk: events[tag + "_host"].append(
                (int(host_id), int(thunk))
            )
        )
    session.set_observer(**kwargs)
    return observer_snapshot(session)


def install_reentrant(session, config: str, attempts: list):
    def attempt(site: str, call_index: int) -> None:
        results = []
        results.append((
            "replace",
            capture_status(lambda: session.set_observer(
                on_call_end=(lambda _result: None),
                on_host_import=(lambda _ctx, _id, _thunk: None),
            )),
        ))
        results.append(("clear", capture_status(session.clear_observer)))
        attempts.append((site, call_index, results))

    kwargs = {}
    if config in ("call_end", "both"):
        def on_end(result):
            attempt("call_end", result["call_index"])
        kwargs["on_call_end"] = on_end
    if config in ("host_import", "both"):
        def on_import(_ctx, _host_id, _thunk):
            attempt("host_import", 0)
        kwargs["on_host_import"] = on_import
    session.set_observer(**kwargs)
    return observer_snapshot(session)


def install_raising(session):
    def on_end(_result):
        raise RuntimeError("observer exception must be contained by the C ABI")

    session.set_observer(
        on_call_end=on_end,
        on_host_import=lambda _ctx, _host_id, _thunk: None,
    )
    return observer_snapshot(session)


def check_counts(checks: Checks, session, label: str,
                 expected_calls: int, expected_host: int) -> None:
    counts = session.counts()
    checks.check(
        f"{label}: calls_total={expected_calls}",
        counts["calls_total"] == expected_calls,
        f"actual={counts['calls_total']}",
    )
    checks.check(
        f"{label}: host_import_calls={expected_host}",
        counts["host_import_calls"] == expected_host,
        f"actual={counts['host_import_calls']}",
    )
    checks.check(
        f"{label}: load_count=1", counts["load_count"] == 1,
        f"actual={counts['load_count']}",
    )


def event_delta(value: list, before: int, expected: int) -> bool:
    return len(value) - before == expected


def safe_owner_cleanup(session) -> None:
    """Never invoke Guest with a registration we may have released."""
    if not session.live:
        return
    try:
        session.clear_observer()
    except BaseException:  # noqa: BLE001 - best-effort safety cleanup
        pass
    if session.live:
        try:
            session.destroy()
        except BaseException:  # noqa: BLE001
            pass


def exercise_config(config: str, session, checks: Checks) -> None:
    oracle = Oracle()
    oracle.reset(SEED)
    expected_calls = 0
    expected_host = 0

    checks.require(
        f"{config}: session created",
        bool(session.live) and session.info()["load_count"] == 1,
    )

    reset = session.call("AevDllReset", [SEED])
    expected_calls += 1
    checks.require(
        f"{config}: Reset enters Guest",
        reset["status"] == 0 and reset["rax_i32"] == s32(SEED),
        f"result={reset['status']}/{reset['rax_i32']}",
    )
    check_counts(checks, session, f"{config}: after Reset",
                 expected_calls, expected_host)

    events = {
        "old_call_end": [], "old_host": [],
        "new_call_end": [], "new_host": [],
    }
    old_refs, old_obs = install_counting(session, config, events, "old")
    checks.check(
        f"{config}: initial observer installed",
        len(session._observer_callbacks) == 2,
        f"callbacks={len(session._observer_callbacks)}",
    )
    check_counts(checks, session, f"{config}: after observer install",
                 expected_calls, expected_host)

    # 1. Non-owner clear must fail with WRONG_THREAD and leave the binding.
    statuses = run_in_thread(session.clear_observer)
    checks.check(
        f"{config}: non-owner clear -> 13", statuses == [13],
        f"statuses={statuses}",
    )
    gc.collect()
    held, detail = wrapper_holds(session, old_refs, old_obs)
    checks.check(
        f"{config}: rejected clear keeps old wrapper binding", held, detail,
    )
    if not held:
        checks.check(
            f"{config}: ABORT before Guest call (dangling C callback)", False,
            "wrapper released the old CFUNCTYPE while C still registers it",
        )
        safe_owner_cleanup(session)
        raise AbortRun("rejected clear released callbacks")
    check_counts(checks, session, f"{config}: rejected clear counts",
                 expected_calls, expected_host)

    before_end = len(events["old_call_end"])
    before_host = len(events["old_host"])
    expected = oracle.compute(5, 6, 2)
    expected_calls += 1
    expected_host += 1
    result = session.call("AevDllCompute", [5, 6, 2])
    checks.check(
        f"{config}: Compute after rejected clear result",
        result["status"] == 0 and result["rax_i32"] == expected,
        f"status={result['status']} actual={result['rax_i32']} expected={expected}",
    )
    checks.check(
        f"{config}: old on_call_end still registered",
        event_delta(events["old_call_end"], before_end,
                    1 if config in ("call_end", "both") else 0),
        f"events={events['old_call_end']}",
    )
    checks.check(
        f"{config}: old on_host_import still registered",
        event_delta(events["old_host"], before_host,
                    1 if config in ("host_import", "both") else 0),
        f"events={events['old_host']}",
    )
    check_counts(checks, session, f"{config}: after Compute following clear",
                 expected_calls, expected_host)

    # 1b. Non-owner replacement must also fail with WRONG_THREAD.
    def non_owner_replace() -> None:
        kwargs = {}
        if config in ("call_end", "both"):
            kwargs["on_call_end"] = (
                lambda result: events["new_call_end"].append(
                    result["call_index"]
                )
            )
        if config in ("host_import", "both"):
            kwargs["on_host_import"] = (
                lambda _ctx, host_id, thunk: events["new_host"].append(
                    (int(host_id), int(thunk))
                )
            )
        session.set_observer(**kwargs)

    statuses = run_in_thread(non_owner_replace)
    checks.check(
        f"{config}: non-owner replace -> 13", statuses == [13],
        f"statuses={statuses}",
    )
    gc.collect()
    held, detail = wrapper_holds(session, old_refs, old_obs)
    checks.check(
        f"{config}: rejected replace keeps old wrapper binding", held, detail,
    )
    if not held:
        checks.check(
            f"{config}: ABORT before Guest call (dangling C callback)", False,
            "wrapper released the old CFUNCTYPE while C still registers it",
        )
        safe_owner_cleanup(session)
        raise AbortRun("rejected replace released callbacks")
    checks.check(
        f"{config}: rejected replace did not run new callbacks",
        not events["new_call_end"] and not events["new_host"],
        f"new_end={events['new_call_end']} new_host={events['new_host']}",
    )
    check_counts(checks, session, f"{config}: rejected replace counts",
                 expected_calls, expected_host)

    before_end = len(events["old_call_end"])
    before_host = len(events["old_host"])
    expected = oracle.compute(2, 7, 4)
    expected_calls += 1
    expected_host += 1
    result = session.call("AevDllCompute", [2, 7, 4])
    checks.check(
        f"{config}: Compute after rejected replace result",
        result["status"] == 0 and result["rax_i32"] == expected,
        f"status={result['status']} actual={result['rax_i32']} expected={expected}",
    )
    checks.check(
        f"{config}: old callback survives rejected replace",
        event_delta(events["old_call_end"], before_end,
                    1 if config in ("call_end", "both") else 0),
        f"events={events['old_call_end']}",
    )
    checks.check(
        f"{config}: new callback never ran after rejected replace",
        not events["new_call_end"] and not events["new_host"],
        f"new_end={events['new_call_end']} new_host={events['new_host']}",
    )
    check_counts(checks, session, f"{config}: after Compute following replace",
                 expected_calls, expected_host)

    # 1c. Local handle validation must also happen before touching the binding.
    saved_handle = session._handle
    try:
        session._handle = 0
        stale_status = capture_status(
            lambda: session.set_observer(on_call_end=lambda _res: None)
        )
    finally:
        session._handle = saved_handle
    checks.check(
        f"{config}: stale handle set_observer -> 9", stale_status == 9,
        f"status={stale_status}",
    )
    gc.collect()
    held, detail = wrapper_holds(session, old_refs, old_obs)
    checks.check(
        f"{config}: handle validation failure keeps old binding", held, detail,
    )
    if not held:
        checks.check(
            f"{config}: ABORT before Guest call (dangling C callback)", False,
            "wrapper released the old CFUNCTYPE while C still registers it",
        )
        safe_owner_cleanup(session)
        raise AbortRun("handle failure released callbacks")
    check_counts(checks, session, f"{config}: after handle validation failure",
                 expected_calls, expected_host)

    # 1d. A candidate-construction error must not touch the binding either.
    construction_status = capture_status(
        lambda: session.set_observer(on_host_import=object())
    )
    checks.check(
        f"{config}: candidate construction error is a TypeError",
        isinstance(construction_status, str)
        and construction_status.startswith("exception:TypeError:"),
        f"status={construction_status}",
    )
    gc.collect()
    held, detail = wrapper_holds(session, old_refs, old_obs)
    checks.check(
        f"{config}: construction failure keeps old binding", held, detail,
    )
    if not held:
        checks.check(
            f"{config}: ABORT before Guest call (dangling C callback)", False,
            "wrapper released the old CFUNCTYPE while C still registers it",
        )
        safe_owner_cleanup(session)
        raise AbortRun("construction failure released callbacks")
    check_counts(checks, session, f"{config}: after construction failure",
                 expected_calls, expected_host)

    # 2. Reentrant replace/clear from each registered callback -> REENTRY.
    attempts: list = []
    reentrant_refs, reentrant_obs = install_reentrant(session, config, attempts)
    gc.collect()
    checks.check(
        f"{config}: successful replacement released old callbacks",
        all(ref() is None for ref in old_refs) and old_obs() is None,
        f"old_alive={[ref() is not None for ref in old_refs]}",
    )
    held, detail = wrapper_holds(session, reentrant_refs, reentrant_obs)
    checks.check(
        f"{config}: reentrant observer installed", held, detail,
    )

    expected_sites = (
        {"call_end", "host_import"} if config == "both" else {config}
    )
    expected = oracle.compute(9, 3, 1)
    expected_calls += 1
    expected_host += 1
    result = session.call("AevDllCompute", [9, 3, 1])
    checks.check(
        f"{config}: Compute inside reentrant observer result",
        result["status"] == 0 and result["rax_i32"] == expected,
        f"status={result['status']} actual={result['rax_i32']} expected={expected}",
    )
    checks.check(
        f"{config}: both callback sites attempted reentrant set/clear",
        {entry[0] for entry in attempts} == expected_sites,
        f"attempts={attempts}",
    )
    for site, _call_index, results in attempts:
        checks.check(
            f"{config}: {site} reentrant replace/clear both -> 10",
            results == [("replace", 10), ("clear", 10)],
            f"results={results}",
        )
    held, detail = wrapper_holds(session, reentrant_refs, reentrant_obs)
    checks.check(
        f"{config}: reentrant rejection keeps old binding", held, detail,
    )
    check_counts(checks, session, f"{config}: after reentrant Compute",
                 expected_calls, expected_host)

    before_attempts = len(attempts)
    expected = oracle.compute(11, 4, 8)
    expected_calls += 1
    expected_host += 1
    result = session.call("AevDllCompute", [11, 4, 8])
    checks.check(
        f"{config}: subsequent Compute after reentry result",
        result["status"] == 0 and result["rax_i32"] == expected,
        f"status={result['status']} actual={result['rax_i32']} expected={expected}",
    )
    checks.check(
        f"{config}: reentrant observer still active on next call",
        len(attempts) - before_attempts == (2 if config == "both" else 1),
        f"attempts_delta={len(attempts) - before_attempts}",
    )
    check_counts(checks, session, f"{config}: after second reentrant Compute",
                 expected_calls, expected_host)

    # 3. Successful replacement: only the new callbacks run.
    success_events = {"new_call_end": [], "new_host": []}
    success_refs, success_obs = install_counting(
        session, config, success_events, "new"
    )
    gc.collect()
    checks.check(
        f"{config}: reentrant callbacks released after successful replace",
        all(ref() is None for ref in reentrant_refs) and reentrant_obs() is None,
        f"reentrant_alive={[ref() is not None for ref in reentrant_refs]}",
    )
    held, detail = wrapper_holds(session, success_refs, success_obs)
    checks.check(
        f"{config}: successful replacement binding", held, detail,
    )
    old_attempts = len(attempts)
    expected = oracle.compute(37, 5, 3)
    expected_calls += 1
    expected_host += 1
    result = session.call("AevDllCompute", [37, 5, 3])
    checks.check(
        f"{config}: successful replacement Compute result",
        result["status"] == 0 and result["rax_i32"] == expected,
        f"status={result['status']} actual={result['rax_i32']} expected={expected}",
    )
    checks.check(
        f"{config}: only new on_call_end runs",
        len(success_events["new_call_end"]) ==
        (1 if config in ("call_end", "both") else 0),
        f"events={success_events['new_call_end']}",
    )
    checks.check(
        f"{config}: only new on_host_import runs",
        len(success_events["new_host"]) ==
        (1 if config in ("host_import", "both") else 0),
        f"events={success_events['new_host']}",
    )
    checks.check(
        f"{config}: old reentrant callback no longer runs",
        len(attempts) == old_attempts,
        f"old_attempts={len(attempts)} baseline={old_attempts}",
    )
    check_counts(checks, session, f"{config}: after successful replace Compute",
                 expected_calls, expected_host)

    # 4. Successful clear: no callbacks at all.
    before_clear_end = len(success_events["new_call_end"])
    before_clear_host = len(success_events["new_host"])
    session.clear_observer()
    gc.collect()
    checks.check(
        f"{config}: successful clear drops wrapper callbacks",
        session._observer is None and session._observer_callbacks == [],
        f"observer={session._observer} count={len(session._observer_callbacks)}",
    )
    checks.check(
        f"{config}: successful clear releases callbacks",
        all(ref() is None for ref in success_refs),
        f"alive={[ref() is not None for ref in success_refs]}",
    )
    expected = oracle.compute(6, 2, 5)
    expected_calls += 1
    expected_host += 1
    result = session.call("AevDllCompute", [6, 2, 5])
    checks.check(
        f"{config}: Compute after clear result",
        result["status"] == 0 and result["rax_i32"] == expected,
        f"status={result['status']} actual={result['rax_i32']} expected={expected}",
    )
    checks.check(
        f"{config}: no callbacks after successful clear",
        len(success_events["new_call_end"]) == before_clear_end
        and len(success_events["new_host"]) == before_clear_host,
        f"events={success_events}",
    )
    check_counts(checks, session, f"{config}: after clear Compute",
                 expected_calls, expected_host)

    # 5. A raising observer is contained by the C ABI and changes no business.
    raise_refs, raise_obs = install_raising(session)
    expected = oracle.compute(3, 9, 1)
    expected_calls += 1
    expected_host += 1
    result = session.call("AevDllCompute", [3, 9, 1])
    checks.check(
        f"{config}: raising callback does not change result",
        result["status"] == 0 and result["rax_i32"] == expected,
        f"status={result['status']} actual={result['rax_i32']} expected={expected}",
    )
    check_counts(checks, session, f"{config}: after raising callback",
                 expected_calls, expected_host)
    held, detail = wrapper_holds(session, raise_refs, raise_obs)
    checks.check(
        f"{config}: raising observer stays registered", held, detail,
    )
    session.clear_observer()
    gc.collect()
    checks.check(
        f"{config}: raising observer released after clear",
        all(ref() is None for ref in raise_refs) and raise_obs() is None,
        f"alive={[ref() is not None for ref in raise_refs]}",
    )

    # 6. Successful destroy releases the observer; stale operations are safe.
    destroy_events = {"new_call_end": [], "new_host": []}
    destroy_refs, destroy_obs = install_counting(
        session, config, destroy_events, "new"
    )
    statuses = run_in_thread(session.destroy)
    checks.check(
        f"{config}: non-owner destroy -> 13", statuses == [13],
        f"statuses={statuses}",
    )
    gc.collect()
    held, detail = wrapper_holds(session, destroy_refs, destroy_obs)
    checks.check(
        f"{config}: rejected destroy keeps old binding", held, detail,
    )
    if not held:
        checks.check(
            f"{config}: ABORT before Guest call (dangling C callback)", False,
            "wrapper released the old CFUNCTYPE while C still registers it",
        )
        safe_owner_cleanup(session)
        raise AbortRun("rejected destroy released callbacks")
    before_end = len(destroy_events["new_call_end"])
    before_host = len(destroy_events["new_host"])
    expected = oracle.compute(8, 3, 4)
    expected_calls += 1
    expected_host += 1
    result = session.call("AevDllCompute", [8, 3, 4])
    checks.check(
        f"{config}: Compute after rejected destroy result",
        result["status"] == 0 and result["rax_i32"] == expected,
        f"status={result['status']} actual={result['rax_i32']} expected={expected}",
    )
    checks.check(
        f"{config}: observer survives rejected destroy",
        event_delta(destroy_events["new_call_end"], before_end,
                    1 if config in ("call_end", "both") else 0)
        and event_delta(destroy_events["new_host"], before_host,
                        1 if config in ("host_import", "both") else 0),
        f"events={destroy_events}",
    )
    check_counts(checks, session, f"{config}: after rejected destroy Compute",
                 expected_calls, expected_host)
    session.destroy()
    gc.collect()
    checks.check(
        f"{config}: destroy releases observer struct",
        destroy_obs() is None and session._observer is None,
        f"observer={session._observer}",
    )
    checks.check(
        f"{config}: destroy releases callbacks",
        all(ref() is None for ref in destroy_refs),
        f"alive={[ref() is not None for ref in destroy_refs]}",
    )
    checks.check(f"{config}: session is no longer live", not session.live)
    stale = capture_status(session.destroy)
    checks.check(
        f"{config}: double destroy -> 9", stale == 9, f"status={stale}",
    )
    stale_set = capture_status(
        lambda: session.set_observer(on_call_end=lambda _res: None)
    )
    checks.check(
        f"{config}: set_observer on destroyed session -> 9",
        stale_set == 9,
        f"status={stale_set}",
    )
    statuses = run_in_thread(session.clear_observer)
    checks.check(
        f"{config}: clear_observer on destroyed session -> 9",
        statuses == [9],
        f"statuses={statuses}",
    )


def repeated_lifecycle(lib: str, dll: str, registry: str,
                       checks: Checks) -> None:
    """Repeated sessions, replace/clear churn, and GC pressure."""
    cycles = 6
    churn = 8
    previous_refs: list[weakref.ref] = []
    for cycle in range(cycles):
        session = PeSession(lib)
        try:
            session.create(dll, registry, load_base=LOAD_BASE)
            events = {"old_call_end": [], "old_host": []}
            for index in range(churn):
                refs, obs = install_counting(
                    session, "both", events, "old"
                )
                gc.collect()
                if previous_refs:
                    stale_alive = [ref() is not None for ref in previous_refs]
                    checks.check(
                        f"churn {cycle}/{index}: previous callbacks released",
                        not any(stale_alive),
                        f"alive={stale_alive}",
                    )
                held, detail = wrapper_holds(session, refs, obs)
                checks.check(
                    f"churn {cycle}/{index}: wrapper binding consistent",
                    held, detail,
                )
                checks.check(
                    f"churn {cycle}/{index}: callback list stays 2",
                    len(session._observer_callbacks) == 2,
                    f"actual={len(session._observer_callbacks)}",
                )
                previous_refs = list(refs)
                session.clear_observer()
                gc.collect()
                checks.check(
                    f"churn {cycle}/{index}: clear releases callbacks",
                    all(ref() is None for ref in refs),
                    f"alive={[ref() is not None for ref in refs]}",
                )
                previous_refs = []
            events = {"new_call_end": [], "new_host": []}
            refs, obs = install_counting(session, "both", events, "new")
            session.destroy()
            gc.collect()
            checks.check(
                f"cycle {cycle}: destroy releases observer",
                obs() is None and all(ref() is None for ref in refs)
                and not session.live,
                f"alive={[ref() is not None for ref in refs]}",
            )
        finally:
            safe_owner_cleanup(session)

    # Create/destroy churn without an observer must not leak a keep-alive list.
    for index in range(cycles):
        session = PeSession(lib)
        try:
            session.create(dll, registry, load_base=LOAD_BASE)
            checks.check(
                f"create/destroy {index}: empty observer state",
                session._observer is None and session._observer_callbacks == [],
            )
            session.destroy()
            checks.check(
                f"create/destroy {index}: destroyed", not session.live,
            )
        finally:
            safe_owner_cleanup(session)


def parse_args(argv: Iterable[str]):
    script = Path(__file__).resolve()
    # spikes/p0-ui/scripts/pe_session_wrapper_test.py -> repository root
    default_source = script.parents[3]
    parser = argparse.ArgumentParser(
        description="F1 wrapper observer lifetime regression (real dylib)"
    )
    parser.add_argument("--build", type=Path,
                        help="build dir containing libaev_pe_session.dylib")
    parser.add_argument("--source", type=Path, default=default_source,
                        help="source tree root (default: derived from script)")
    parser.add_argument("--results", type=Path,
                        help="results dir (default: <build>/wrapper-observer-results)")
    parser.add_argument(
        "legacy", nargs="*",
        help="optional positional BUILD [SOURCE [RESULTS]]",
    )
    args = parser.parse_args(list(argv))
    if args.legacy:
        if len(args.legacy) > 3:
            parser.error("at most three positional args: BUILD [SOURCE [RESULTS]]")
        if args.build is None:
            args.build = Path(args.legacy[0])
        if len(args.legacy) > 1 and args.source == default_source:
            args.source = Path(args.legacy[1])
        if len(args.legacy) > 2 and args.results is None:
            args.results = Path(args.legacy[2])
    if args.build is None:
        parser.error("--build (or positional BUILD) is required")
    if args.results is None:
        args.results = args.build / "wrapper-observer-results"
    return args


def main(argv: Iterable[str]) -> int:
    args = parse_args(argv)
    build = args.build.resolve()
    source = args.source.resolve()
    results = args.results.resolve()
    results.mkdir(parents=True, exist_ok=True)

    lib_path = build / "libaev_pe_session.dylib"
    dll_path = source / "spikes/p0-engine/testdata/pe/aevguest.dll"
    registry_path = source / "spikes/p0-engine/testdata/pe/aevimports.txt"
    missing = [str(path) for path in (lib_path, dll_path, registry_path)
               if not path.is_file()]
    if missing:
        print("FAIL: required inputs missing: " + ", ".join(missing),
              file=sys.stderr)
        return 2

    sys.path.insert(0, str(source / "spikes/p0-ui"))
    global PeSession, PeSessionError
    from harness.pe_session import PeSession, PeSessionError  # noqa: F811

    metadata = {
        "library": str(lib_path),
        "library_sha256": sha256_file(lib_path),
        "dll": str(dll_path),
        "dll_sha256": sha256_file(dll_path),
        "registry": str(registry_path),
        "registry_sha256": sha256_file(registry_path),
        "load_base": hex(LOAD_BASE),
        "python": sys.version.split()[0],
        "platform": sys.platform,
    }
    print("== F1 wrapper observer lifecycle regression ==")
    print(json.dumps(metadata, indent=2, sort_keys=True))

    checks = Checks()
    configs = ("call_end", "host_import", "both")
    aborted = False
    for config in configs:
        print(f"\n-- observer config: {config} --")
        session = PeSession(str(lib_path))
        try:
            session.create(
                str(dll_path), str(registry_path), load_base=LOAD_BASE
            )
            exercise_config(config, session, checks)
        except AbortRun as exc:
            aborted = True
            print(f"ABORT {config}: {exc}")
            break
        except BaseException as exc:  # noqa: BLE001 - report and clean up
            checks.check(
                f"{config}: unexpected exception", False,
                f"{type(exc).__name__}: {exc}",
            )
            aborted = True
            break
        finally:
            safe_owner_cleanup(session)

    if not aborted:
        print("\n-- repeated create/clear + GC pressure --")
        try:
            repeated_lifecycle(
                str(lib_path), str(dll_path), str(registry_path), checks
            )
        except BaseException as exc:  # noqa: BLE001
            checks.check(
                "repeated lifecycle: unexpected exception", False,
                f"{type(exc).__name__}: {exc}",
            )

    summary = checks.summary()
    summary.update(metadata)
    summary["observer_configs"] = list(configs)
    summary["completed_all_configs"] = not aborted
    checks_path = results / "checks.json"
    checks_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(
        f"\nSUMMARY checks={summary['checks_total']} "
        f"passed={summary['checks_passed']} failed={summary['checks_failed']} "
        f"results={checks_path}"
    )
    if summary["checks_failed"] or aborted:
        print("RESULT: FAIL")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
