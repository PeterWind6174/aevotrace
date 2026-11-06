#!/usr/bin/env python3
"""Trace API lifecycle checks (open/close/reopen/in-call/unwritable/destroy)."""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "spikes" / "p0-ui"))
from harness.engine_bridge import BridgeError, EngineBridge  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", required=True)
    ap.add_argument("--coff", required=True)
    ap.add_argument("--aot", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    trace = out / "api.jsonl"

    bridge = EngineBridge(args.lib)
    bridge.create(args.coff, args.aot, 0x140001000, dump_jit=False)
    guest = bridge.guest_info()
    text_addr = guest.base + 0x1000
    ctx_addr = guest.base + 0x2000
    counter_addr = guest.base + 0x3000
    text = "Aevoβ中".encode("utf-16-le")
    bridge.guest_write(text_addr, text)
    bridge.guest_write(ctx_addr, struct.pack("<QQ", 0x140010010, counter_addr))
    state = {"probe": False, "status": None, "reserved0": 0}

    def host_update(a0, a1, a2, a3):
        if state["probe"]:
            state["probe"] = False
            state["reserved0"] = bridge.debug_reserved0()
            try:
                bridge.trace_open(str(out / "inflight.jsonl"), "inflight")
            except BridgeError as exc:
                state["status"] = exc.status
        return a2

    bridge.register_import(0x140010010, 1, host_update)

    bridge.trace_open(str(trace), "api")
    try:
        bridge.trace_open(str(trace), "again")
    except BridgeError as exc:
        assert exc.status == 1, exc.status
    else:
        raise SystemExit("trace_open twice unexpectedly succeeded")
    bridge.trace_close()
    bridge.trace_close()
    bridge.trace_open(str(trace), "reopen")
    bridge.trace_close()
    try:
        bridge.trace_open("/nonexistent/aev-trace.jsonl", "bad")
    except BridgeError as exc:
        assert exc.status != 0
    else:
        raise SystemExit("unwritable trace path unexpectedly succeeded")

    # Attempt open from inside a call; must be REENTRY.
    bridge.trace_open(str(trace), "probe")
    assert bridge.debug_reserved0() == 0
    state["probe"] = True
    result = bridge.call(0x140001000, [7, text_addr, len(text) // 2, ctx_addr],
                         raise_on_error=False)
    assert result.status == 0 and state["status"] == 9, (result.status, state)
    assert state["reserved0"] != 0, "trace sink was not installed during the call"
    assert bridge.debug_reserved0() == 0, "reserved0 was not cleared after the call"
    bridge.trace_close()

    # After close, a normal call must not append trace events or crash.
    bridge.trace_open(str(trace), "events")
    bridge.call(0x140001000, [7, text_addr, len(text) // 2, ctx_addr],
                raise_on_error=False)
    bridge.trace_close()
    block_events = [
        e for e in (json.loads(line) for line in trace.read_text().splitlines())
        if e.get("record") in ("aot", "jit_compile", "jit_exec")
    ]
    assert block_events, "trace has no block events"
    for ev in block_events:
        assert "offset" in ev and "section_offset" not in ev, ev
    size_after_close = trace.stat().st_size
    bridge.call(0x140001000, [7, text_addr, len(text) // 2, ctx_addr],
                raise_on_error=False)
    assert trace.stat().st_size == size_after_close, "trace grew after close"
    bridge.destroy()

    # destroy while trace is open must flush/close without crashing.
    bridge2 = EngineBridge(args.lib)
    bridge2.create(args.coff, args.aot, 0x140001000, dump_jit=False)
    bridge2.trace_open(str(out / "destroy.jsonl"), "destroy")
    bridge2.destroy()
    assert (out / "destroy.jsonl").exists()
    print("TRACE_API_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
