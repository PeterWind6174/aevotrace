#!/usr/bin/env python3
"""Validate and publish a supplemented AOT dylib (P0-TRACE-AOT-01).

This is test/build orchestration, not product runtime.  It loads the freshly
linked dylib, checks its actual exported identity/table symbols, verifies the
ARM64 slice, updates the supplement manifest with the final SHA-256, and only
then publishes both files with backup/rollback.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from aot_supplement import publish_outputs  # noqa: E402

TARGET = "arm64-apple-macosx14.0.0"


class AevBlock(ctypes.Structure):
    _fields_ = [("rip", ctypes.c_uint64), ("fn", ctypes.c_void_p)]


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dylib-tmp", required=True)
    ap.add_argument("--dylib-out", required=True)
    ap.add_argument("--manifest-in", required=True)
    ap.add_argument("--manifest-out", required=True)
    ap.add_argument("--promoted-rip", required=True)
    ap.add_argument("--expected-table", type=int, required=True)
    args = ap.parse_args()

    tmp = Path(args.dylib_tmp).resolve()
    out = Path(args.dylib_out).resolve()
    manifest_in = Path(args.manifest_in).resolve()
    manifest_out = Path(args.manifest_out).resolve()
    promoted = int(args.promoted_rip, 0)
    manifest = json.loads(manifest_in.read_text())
    input_sha = manifest["input_sha256"]
    all_count = int(manifest["all_block_count"])
    manifest_selected = [int(x) for x in manifest["selected_blocks"]]
    if not tmp.exists():
        raise SystemExit(f"temporary dylib missing: {tmp}")

    lib = ctypes.CDLL(str(tmp))
    target = (ctypes.c_char * 48).in_dll(lib, "aev_target_triple").value
    if target != TARGET.encode():
        raise SystemExit(f"dylib target mismatch: {target!r}")
    guest_abi = ctypes.c_uint64.in_dll(lib, "aev_aot_abi_version").value
    artifact_abi = ctypes.c_uint64.in_dll(lib, "aev_artifact_abi_version").value
    translator_abi = ctypes.c_uint64.in_dll(lib, "aev_translator_abi_version").value
    state_size = ctypes.c_uint64.in_dll(lib, "aev_guest_state_size").value
    if guest_abi != 1 or artifact_abi != 1 or translator_abi != 1 or state_size != 256:
        raise SystemExit(
            f"dylib ABI mismatch: guest={guest_abi} artifact={artifact_abi} "
            f"translator={translator_abi} state={state_size}"
        )
    input_bytes = bytes((ctypes.c_ubyte * 32).in_dll(lib, "aev_input_sha256"))
    if input_bytes.hex() != input_sha:
        raise SystemExit("dylib input SHA-256 does not match manifest")
    count = ctypes.c_uint64.in_dll(lib, "aev_block_count").value
    all_block_count = ctypes.c_uint64.in_dll(lib, "aev_all_block_count").value
    if count != args.expected_table or all_block_count != all_count:
        raise SystemExit(
            f"dylib table size mismatch: selected={count} all={all_block_count}"
        )
    blocks = (AevBlock * count).in_dll(lib, "aev_blocks")
    selected = [int(blocks[i].rip) for i in range(count)]
    if selected != sorted(set(selected)):
        raise SystemExit("dylib selected table is not sorted/unique")
    if selected != manifest_selected:
        raise SystemExit(
            f"dylib selected table {[hex(x) for x in selected]} != manifest "
            f"{[hex(x) for x in manifest_selected]}"
        )
    if promoted not in selected:
        raise SystemExit(f"promoted RIP {hex(promoted)} not selected")
    try:
        getattr(lib, f"aev_blk_{promoted:x}")
    except AttributeError as exc:
        raise SystemExit(f"promoted block symbol missing: {exc}") from exc
    all_blocks = (ctypes.c_uint64 * all_block_count).in_dll(lib, "aev_all_blocks")
    if list(all_blocks) != sorted(set(all_blocks)):
        raise SystemExit("dylib all-block table is not sorted/unique")
    arch = subprocess.check_output(["lipo", "-archs", str(tmp)], text=True).strip()
    if "arm64" not in arch.split():
        raise SystemExit(f"dylib is not an ARM64 slice: {arch}")

    manifest["aot_sha256"] = sha256_file(tmp)
    manifest["aot_path"] = str(out)
    manifest_out.parent.mkdir(parents=True, exist_ok=True)
    manifest_tmp = manifest_out.with_name(
        f".{manifest_out.name}.stage.{os.getpid()}"
    )
    manifest_tmp.write_text(json.dumps(manifest, indent=2) + "\n")
    try:
        publish_outputs([(out, tmp), (manifest_out, manifest_tmp)])
    finally:
        if manifest_tmp.exists():
            manifest_tmp.unlink()
    print(
        json.dumps(
            {
                "published": str(out),
                "aot_sha256": manifest["aot_sha256"],
                "selected_blocks": [hex(x) for x in selected],
                "promoted_rip": hex(promoted),
                "arch": arch,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
