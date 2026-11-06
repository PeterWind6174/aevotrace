#!/usr/bin/env python3
"""Offline trace -> AOT supplement (P0-TRACE-AOT-01).

The tool validates JSONL records against the actual COFF/translator manifest,
selects blocks that actually executed (AOT or JIT), then invokes the shared
`aev-translate` emitter with explicit --aot-include-rip flags.  It never
recompiles Guest C source and does not link or copy the ORC JIT.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TARGET = "arm64-apple-macosx14.0.0"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def run_translator(args, input_path, function, entry, out_ll, out_header, out_manifest,
                   includes):
    cmd = [str(args.translator), "--input", str(input_path),
           "--function", function, "--entry", entry,
           "--out-ll", str(out_ll), "--out-header", str(out_header),
           "--out-manifest", str(out_manifest)]
    for rip in sorted(includes):
        cmd += ["--aot-include-rip", hex(rip)]
    proc = subprocess.run(cmd, text=True, capture_output=True)
    if proc.returncode != 0:
        raise SystemExit(
            f"translator failed ({proc.returncode}): {proc.stderr.strip()}"
        )


def parse_hex(value, what):
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        raise ValueError(f"{what}: expected hex string")
    return int(value, 0)


def load_manifest(path: Path):
    data = json.loads(path.read_text())
    blocks = {}
    for fn in data.get("functions", []):
        for block in fn.get("blocks", []):
            rip = int(block["rip"])
            if rip in blocks:
                raise ValueError(f"duplicate block in manifest: {hex(rip)}")
            blocks[rip] = block
    return data, blocks


def validate_header(header, manifest, blocks, input_sha, trace_path, expected_aot_sha=None):
    required = ("trace_abi", "scenario", "input_sha256", "input_fnv", "entry",
                "function", "callback_abi", "guest_abi", "aot_artifact_abi",
                "translator_abi", "target", "translator_version",
                "trace_tool_version", "aot_sha256", "all_blocks", "aot_selected")
    for key in required:
        if key not in header:
            raise ValueError(f"{trace_path}: header missing {key}")
    if header["trace_abi"] != 1:
        raise ValueError(f"{trace_path}: unsupported trace ABI")
    if header["input_sha256"] != input_sha:
        raise ValueError(f"{trace_path}: input SHA-256 mismatch")
    if parse_hex(header["input_fnv"], "input_fnv") != int(manifest["input_hash"]):
        raise ValueError(f"{trace_path}: input FNV mismatch")
    if parse_hex(header["entry"], "entry") != int(manifest["entry_address"]):
        raise ValueError(f"{trace_path}: entry mismatch")
    if header["function"] != manifest["entry"]:
        raise ValueError(f"{trace_path}: function mismatch")
    if header.get("callback_abi") != 2 or header.get("guest_abi") != 1:
        raise ValueError(f"{trace_path}: callback/guest ABI mismatch")
    if header["aot_artifact_abi"] != 1 or header["translator_abi"] != 1:
        raise ValueError(f"{trace_path}: artifact/translator ABI mismatch")
    if header.get("translator_version") != "aev-translate-v1" or \
            header.get("trace_tool_version") != "aev-trace-sink-v1":
        raise ValueError(f"{trace_path}: translator/trace tool version mismatch")
    if header["target"] != TARGET or manifest.get("target") != TARGET:
        raise ValueError(f"{trace_path}: target triple mismatch")
    aot_sha = header.get("aot_sha256")
    if not isinstance(aot_sha, str) or not re.fullmatch(r"[0-9a-f]{64}", aot_sha) or \
            aot_sha == "0" * 64:
        raise ValueError(f"{trace_path}: invalid aot_sha256")
    if expected_aot_sha is not None and aot_sha != expected_aot_sha:
        raise ValueError(f"{trace_path}: aot_sha256 does not match base AOT")
    all_blocks = [parse_hex(x, "all_blocks") for x in header["all_blocks"]]
    if all_blocks != sorted(blocks):
        raise ValueError(f"{trace_path}: header all_blocks does not match COFF")
    selected = [parse_hex(x, "aot_selected") for x in header["aot_selected"]]
    if selected != sorted(set(selected)) or any(x not in blocks for x in selected):
        raise ValueError(f"{trace_path}: header aot_selected is not a sorted subset")
    return set(selected)


def validate_events(path: Path, blocks, header_selected):
    call_end = {}
    seen_calls = set()
    selected_here = set()
    last_call = 0
    with path.open("r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                ev = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: invalid JSON: {exc}") from exc
            if not isinstance(ev, dict):
                raise ValueError(f"{path}:{lineno}: record is not an object")
            record = ev.get("record")
            if record == "header":
                continue
            if record not in ("aot", "jit_compile", "jit_exec", "host_import",
                              "dispatch_miss", "call_end"):
                raise ValueError(f"{path}:{lineno}: unknown record {record!r}")
            call = ev.get("call")
            if not isinstance(call, int) or call <= 0:
                raise ValueError(f"{path}:{lineno}: invalid call id")
            if call < last_call:
                raise ValueError(f"{path}:{lineno}: call ids are not monotonic")
            if call > last_call:
                if last_call and last_call not in call_end:
                    raise ValueError(
                        f"{path}:{lineno}: call {last_call} has no call_end before call {call}"
                    )
                last_call = call
            if call in call_end:
                raise ValueError(f"{path}:{lineno}: event after call_end")
            seen_calls.add(call)
            if record == "call_end":
                if call in call_end:
                    raise ValueError(f"{path}:{lineno}: duplicate call_end")
                status = ev.get("status")
                if not isinstance(status, int) or status != 0:
                    raise ValueError(f"{path}:{lineno}: failed/invalid call_end status")
                if "error_pc" not in ev or "guest_error" not in ev:
                    raise ValueError(f"{path}:{lineno}: call_end missing error fields")
                parse_hex(ev["error_pc"], "error_pc")
                guest_error = ev["guest_error"]
                if not isinstance(guest_error, int) or guest_error < 0:
                    raise ValueError(f"{path}:{lineno}: invalid guest_error")
                call_end[call] = status
                continue
            if record in ("aot", "jit_compile", "jit_exec"):
                if "rip" not in ev:
                    raise ValueError(f"{path}:{lineno}: block event missing rip")
                rip = parse_hex(ev["rip"], "rip")
                if rip not in blocks:
                    raise ValueError(f"{path}:{lineno}: unknown block {hex(rip)}")
                if ("offset" not in ev or "size" not in ev or
                        "block_sha256" not in ev):
                    raise ValueError(f"{path}:{lineno}: block identity missing")
                expected = blocks[rip]
                if int(ev["offset"]) != int(expected["section_offset"]) or \
                        int(ev["size"]) != int(expected["size"]) or \
                        ev["block_sha256"] != expected["sha256"]:
                    raise ValueError(f"{path}:{lineno}: block identity mismatch")
                if record == "aot" and rip not in header_selected:
                    raise ValueError(f"{path}:{lineno}: aot event for unselected block")
                if record != "aot" and rip in header_selected:
                    raise ValueError(f"{path}:{lineno}: jit event for AOT-selected block")
                selected_here.add(rip)
            elif record == "host_import":
                if any(k in ev for k in ("a0", "a1", "a2", "a3")):
                    raise ValueError(f"{path}:{lineno}: host_import must be id-only")
                if not isinstance(ev.get("id"), int) or ev["id"] <= 0:
                    raise ValueError(f"{path}:{lineno}: invalid host import id")
            elif record == "dispatch_miss":
                parse_hex(ev.get("rip"), "rip")
                if not isinstance(ev.get("status"), int):
                    raise ValueError(f"{path}:{lineno}: dispatch_miss missing status")
    if not seen_calls:
        raise ValueError(f"{path}: no calls")
    missing = seen_calls - set(call_end)
    if missing:
        raise ValueError(f"{path}: missing call_end for calls {sorted(missing)}")
    return selected_here, set(call_end)


def publish_outputs(mapping):
    """Publish generated files with backups; restore all on any failure."""
    staged = []
    backups = {}
    published = []
    try:
        for final, source in mapping:
            final = Path(final)
            final.parent.mkdir(parents=True, exist_ok=True)
            tmp = final.with_name(f".{final.name}.tmp.{os.getpid()}")
            shutil.copyfile(source, tmp)
            staged.append((final, tmp))
        for final, _tmp in staged:
            if final.exists():
                backup = final.with_name(f".{final.name}.bak.{os.getpid()}")
                os.replace(final, backup)
                backups[final] = backup
            else:
                backups[final] = None
        for final, tmp in staged:
            os.replace(tmp, final)
            published.append(final)
    except BaseException:
        for final, tmp in staged:
            if tmp.exists():
                tmp.unlink()
        for final in published:
            backup = backups.get(final)
            if backup and backup.exists():
                os.replace(backup, final)
            elif final.exists():
                final.unlink()
        for final, backup in backups.items():
            if backup and backup.exists():
                os.replace(backup, final)
        raise
    else:
        for backup in backups.values():
            if backup and backup.exists():
                backup.unlink()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--translator", required=True)
    ap.add_argument("--input", required=True)
    ap.add_argument("--function", required=True)
    ap.add_argument("--entry", required=True)
    ap.add_argument("--entry-addr", required=True)
    ap.add_argument("--base-aot", default="")
    ap.add_argument("--trace", action="append", required=True)
    ap.add_argument("--out-ll", required=True)
    ap.add_argument("--out-header", required=True)
    ap.add_argument("--out-manifest", required=True)
    args = ap.parse_args()

    input_path = Path(args.input).resolve()
    out_ll = Path(args.out_ll).resolve()
    out_header = Path(args.out_header).resolve()
    out_manifest = Path(args.out_manifest).resolve()
    input_sha = sha256_file(input_path)
    entry_addr = int(args.entry_addr, 0)
    expected_aot_sha = None
    if args.base_aot:
        expected_aot_sha = sha256_file(Path(args.base_aot).resolve())

    out_ll.parent.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="aev-supplement-", dir=str(out_ll.parent)))
    try:
        base_ll = work / "base.ll"
        base_header = work / "base.h"
        base_manifest = work / "base.json"
        run_translator(args, input_path, args.function, args.entry,
                       base_ll, base_header, base_manifest, [])
        manifest, blocks = load_manifest(base_manifest)
        if manifest.get("input_sha256") != input_sha:
            raise SystemExit("translator input SHA does not match input file")
        if int(manifest.get("entry_address", -1)) != entry_addr:
            raise SystemExit("entry-addr does not match translator entry")
        selected = set()
        trace_hashes = {}
        for trace_name in args.trace:
            trace_path = Path(trace_name).resolve()
            trace_hashes[str(trace_path)] = sha256_file(trace_path)
            with trace_path.open("r", encoding="utf-8") as f:
                try:
                    lines = [json.loads(line) for line in f if line.strip()]
                except json.JSONDecodeError as exc:
                    raise SystemExit(f"{trace_path}: invalid JSON: {exc}") from exc
            if not lines or not isinstance(lines[0], dict) or \
                    lines[0].get("record") != "header":
                raise SystemExit(f"{trace_path}: missing header")
            headers = [x for x in lines if isinstance(x, dict) and x.get("record") == "header"]
            if len(headers) != 1:
                raise SystemExit(f"{trace_path}: expected exactly one header")
            try:
                header_selected = validate_header(
                    headers[0], manifest, blocks, input_sha, trace_path,
                    expected_aot_sha,
                )
                selected_here, _ = validate_events(trace_path, blocks, header_selected)
            except (ValueError, TypeError, KeyError) as exc:
                raise SystemExit(f"{trace_path}: invalid trace: {exc}") from exc
            selected.update(selected_here)
        if not selected:
            raise SystemExit("no executed blocks selected from traces")
        selected_list = sorted(selected)
        base_selected = {int(b["rip"]) for b in blocks.values() if not b["jit_only"]}
        if not (selected - base_selected):
            raise SystemExit("no JIT-discovered block to supplement")

        final_ll = work / "final.ll"
        final_header = work / "final.h"
        final_manifest = work / "final.json"
        run_translator(args, input_path, args.function, args.entry,
                       final_ll, final_header, final_manifest, selected_list)
        final_data, final_blocks = load_manifest(final_manifest)
        if final_data.get("input_sha256") != input_sha:
            raise SystemExit("supplement manifest input SHA mismatch")
        for rip in selected_list:
            if final_blocks.get(rip, {}).get("jit_only"):
                raise SystemExit(f"selected block still jit_only: {hex(rip)}")
        final_data["selected_blocks"] = selected_list
        final_data["trace_files"] = [
            {"path": p, "sha256": h} for p, h in sorted(trace_hashes.items())
        ]
        final_data["trace_abi"] = 1
        final_data["supplement_tool"] = "aev-aot-supplement-v1"
        final_data["promoted_blocks"] = [
            rip for rip in selected_list if rip not in base_selected
        ]
        final_manifest.write_text(json.dumps(final_data, indent=2) + "\n")
        publish_outputs([
            (out_ll, final_ll),
            (out_header, final_header),
            (out_manifest, final_manifest),
        ])
        print(json.dumps({
            "success": True,
            "input_sha256": input_sha,
            "entry_address": entry_addr,
            "selected_blocks": [hex(x) for x in selected_list],
            "promoted_blocks": [hex(x) for x in final_data["promoted_blocks"]],
            "all_block_count": final_data.get("all_block_count"),
            "trace_count": len(args.trace),
        }, indent=2))
        return 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
