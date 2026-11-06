#!/usr/bin/env python3
"""Run one fixed fixture through libaev_bridge with optional execution trace.

Used by run_trace_aot_test.sh.  Base and supplemented runs are separate OS
processes; this script itself never recompiles Guest C source.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "spikes" / "p0-ui"))
sys.path.insert(0, str(ROOT / "spikes" / "p0-ui" / "harness"))

from harness.engine_bridge import EngineBridge  # noqa: E402


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", required=True)
    ap.add_argument("--coff", required=True)
    ap.add_argument("--aot", required=True)
    ap.add_argument("--entry", required=True)
    ap.add_argument("--function", required=True)
    ap.add_argument("--fixture", choices=["callback", "trace"], required=True)
    ap.add_argument("--mode", choices=["base", "supplemented"], required=True)
    ap.add_argument("--trace", default="")
    ap.add_argument("--scenario", default="A")
    ap.add_argument("--case", choices=["A", "B", "AB"], default="AB")
    ap.add_argument("--expected-aot-table", type=int, required=True)
    ap.add_argument("--promoted-rip", default="")
    ap.add_argument("--expect-promoted-selected", action="store_true")
    args = ap.parse_args()

    entry = int(args.entry, 0)
    bridge = EngineBridge(args.lib)
    bridge.create(args.coff, args.aot, entry, dump_jit=False,
                  function_name=args.function)
    info = bridge.artifact_info()
    if info["aot_table_count"] != args.expected_aot_table:
        raise SystemExit(
            f"AOT table count {info['aot_table_count']} != expected "
            f"{args.expected_aot_table}"
        )
    if info["input_sha256"] != sha256_file(args.coff):
        raise SystemExit("artifact input SHA mismatch")
    selected_blocks = bridge.selected_blocks()
    if len(selected_blocks) != args.expected_aot_table:
        raise SystemExit(
            f"actual selected table length {len(selected_blocks)} != "
            f"{args.expected_aot_table}"
        )
    if args.promoted_rip:
        promoted = int(args.promoted_rip, 0)
        actual = bridge.selected_block_contains(promoted)
        if actual != args.expect_promoted_selected:
            raise SystemExit(
                f"promoted RIP {hex(promoted)} selected={actual}, "
                f"expected {args.expect_promoted_selected}"
            )
    guest = bridge.guest_info()
    if args.fixture == "callback":
        text = "Aevoβ中".encode("utf-16-le")
        text_addr = guest.base + 0x1000
        ctx_addr = guest.base + 0x2000
        counter_addr = guest.base + 0x3000
        bridge.guest_write(text_addr, text)
        bridge.guest_write(ctx_addr, struct.pack("<QQ", 0x140010010, counter_addr))
        calls = [("A", [7, text_addr, len(text) // 2, ctx_addr], 3828036654)]
    else:
        ctx_addr = guest.base + 0x2000
        counter_addr = guest.base + 0x3000
        bridge.guest_write(ctx_addr, struct.pack("<QQ", 0x140010010, counter_addr))
        calls = [
            ("A", [1, 7, ctx_addr], 3922),
            ("B", [0xA5A5, 9, ctx_addr], 45320),
        ]

    def host_update(a0, a1, a2, a3):
        return a2

    bridge.register_import(0x140010010, 1, host_update)
    if args.trace:
        bridge.trace_open(args.trace, args.scenario)
    if args.case != "AB":
        calls = [c for c in calls if c[0] == args.case]
    results = []
    for name, call_args, expected in calls:
        result = bridge.call(entry, call_args, raise_on_error=False)
        results.append({
            "case": name,
            "status": result.status,
            "guest_rax": result.guest_rax,
            "expected": expected,
            "aot_blocks": result.aot_blocks,
            "jit_blocks": result.jit_blocks,
            "jit_compiles": result.jit_compiles,
            "jit_execs": result.jit_execs,
            "call_index": result.call_index,
        })
        if result.status != 0 or result.guest_rax != expected:
            raise SystemExit(
                f"{name}: status={result.status} rax={result.guest_rax} "
                f"expected={expected}"
            )
    if args.trace:
        bridge.trace_close()
    artifact_info = info
    counter = struct.unpack("<Q", bridge.guest_read(counter_addr, 8))[0]
    bridge.destroy()
    print(json.dumps({
        "fixture": args.fixture,
        "mode": args.mode,
        "trace": args.trace or None,
        "artifact_info": artifact_info,
        "results": results,
        "counter": counter,
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
