#!/usr/bin/env python3
"""Validate and publish a supplemented AOT dylib (P0-PROMOTION-FIX-01).

The publisher trusts the supplement manifest for COFF identity.  It verifies
both the actual base and new dylibs' exported symbols/tables and the base
provenance before publishing either file.  It never reparses COFF and never
acts as a second ORC dispatcher.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from aot_artifact import (  # noqa: E402
    Artifact,
    ArtifactError,
    load_artifact,
    manifest_all_blocks,
    manifest_identity,
    manifest_selected,
    require_identity,
    sha256_file,
)
from aot_supplement import publish_outputs  # noqa: E402

REQUIRED_MANIFEST = (
    "input_sha256", "input_hash", "entry_address", "entry_function_sha256",
    "target", "artifact_abi", "translator_abi", "all_block_count",
    "module_kind", "module_file_size", "preferred_base", "load_base",
    "image_size", "image_sha256", "import_binding_sha256",
    "host_binding_sha256", "entry_exports_sha256", "entry_exports", "thunk_base",
    "import_count", "imports", "relocs_stripped",
    "base_aot_sha256", "base_selected_blocks", "selected_blocks",
    "promoted_blocks", "demoted_blocks",
)


def _int_list(manifest, key, *, allow_empty: bool = True) -> list[int]:
    value = manifest.get(key)
    if not isinstance(value, list):
        raise SystemExit(f"manifest {key} must be a list")
    result = []
    for item in value:
        if type(item) is not int or item < 0:
            raise SystemExit(f"manifest {key} must contain non-negative integers")
        result.append(item)
    if result != sorted(set(result)):
        raise SystemExit(f"manifest {key} must be sorted and unique")
    if not allow_empty and not result:
        raise SystemExit(f"manifest {key} must not be empty")
    return result


def _arm64(path: Path) -> str:
    try:
        arch = subprocess.check_output(["lipo", "-archs", str(path)], text=True).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"cannot inspect artifact architecture {path}: {exc}") from exc
    if "arm64" not in arch.split():
        raise SystemExit(f"dylib is not an ARM64 slice: {path}: {arch}")
    return arch


def _expected(manifest: dict) -> dict:
    expected = manifest_identity(manifest)
    expected["guest_state_size"] = 296
    expected["all_blocks"] = manifest_all_blocks(manifest)
    return expected


def _verify_artifact(artifact: Artifact, expected: dict, label: str,
                     expected_selected: list[int]) -> None:
    try:
        require_identity(artifact, expected)
    except ArtifactError as exc:
        raise SystemExit(f"{label} artifact identity mismatch: {exc}") from exc
    if list(artifact.selected_blocks) != expected_selected:
        raise SystemExit(
            f"{label} selected table {[hex(x) for x in artifact.selected_blocks]} "
            f"!= manifest {[hex(x) for x in expected_selected]}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dylib-tmp", required=True)
    ap.add_argument("--dylib-out", required=True)
    ap.add_argument("--base-aot", required=True)
    ap.add_argument("--manifest-in", required=True)
    ap.add_argument("--manifest-out", required=True)
    ap.add_argument("--promoted-rip", default="")
    ap.add_argument("--expected-table", type=int, required=True)
    args = ap.parse_args()

    tmp = Path(args.dylib_tmp).resolve()
    out = Path(args.dylib_out).resolve()
    base_path = Path(args.base_aot).resolve()
    manifest_in = Path(args.manifest_in).resolve()
    manifest_out = Path(args.manifest_out).resolve()
    if not tmp.is_file():
        raise SystemExit(f"temporary dylib missing: {tmp}")
    if not base_path.is_file():
        raise SystemExit(f"base AOT missing: {base_path}")
    if out == base_path:
        raise SystemExit("refusing to overwrite the base AOT path")

    manifest = json.loads(manifest_in.read_text())
    missing = [key for key in REQUIRED_MANIFEST if key not in manifest]
    if missing:
        raise SystemExit(f"manifest missing required fields: {', '.join(missing)}")
    all_count = int(manifest["all_block_count"])
    manifest_all = list(manifest_all_blocks(manifest))
    if len(manifest_all) != all_count or manifest_all != sorted(set(manifest_all)):
        raise SystemExit("manifest all-block table/count is inconsistent")
    manifest_selected_blocks = _int_list(manifest, "selected_blocks",
                                         allow_empty=False)
    manifest_base_selected = _int_list(manifest, "base_selected_blocks")
    manifest_promoted = _int_list(manifest, "promoted_blocks")
    manifest_demoted = _int_list(manifest, "demoted_blocks")
    if tuple(manifest_selected_blocks) != manifest_selected(manifest):
        raise SystemExit("manifest selected_blocks is not a sorted integer table")
    if manifest["base_aot_sha256"] != sha256_file(base_path):
        raise SystemExit("manifest base_aot_sha256 does not match actual base AOT")

    try:
        base = load_artifact(base_path)
        new = load_artifact(tmp)
    except ArtifactError as exc:
        raise SystemExit(f"cannot validate actual AOT artifact: {exc}") from exc

    expected = _expected(manifest)
    _verify_artifact(base, expected, "base", manifest_base_selected)
    _verify_artifact(new, expected, "new", manifest_selected_blocks)

    if new.sha256 != sha256_file(tmp):
        raise SystemExit("new dylib SHA changed during validation")
    if len(new.selected_blocks) != args.expected_table:
        raise SystemExit(
            f"dylib table size mismatch: selected={len(new.selected_blocks)} "
            f"expected={args.expected_table}")
    if all_count != len(new.all_blocks):
        raise SystemExit("manifest all_block_count does not match actual new artifact")
    if list(base.all_blocks) != manifest_all:
        raise SystemExit("base all-block table does not match manifest")
    if list(new.all_blocks) != manifest_all:
        raise SystemExit("new all-block table does not match manifest")

    selected_set = set(manifest_selected_blocks)
    base_set = set(manifest_base_selected)
    expected_promoted = sorted(selected_set - base_set)
    expected_demoted = sorted(base_set - selected_set)
    if manifest_promoted != expected_promoted:
        raise SystemExit(
            f"manifest promoted_blocks {[hex(x) for x in manifest_promoted]} "
            f"!= {[hex(x) for x in expected_promoted]}")
    if manifest_demoted != expected_demoted:
        raise SystemExit(
            f"manifest demoted_blocks {[hex(x) for x in manifest_demoted]} "
            f"!= {[hex(x) for x in expected_demoted]}")

    promoted_rip = int(args.promoted_rip, 0) if args.promoted_rip else None
    if promoted_rip is not None:
        if promoted_rip not in set(manifest_promoted) or promoted_rip not in selected_set:
            raise SystemExit(
                f"promoted RIP {hex(promoted_rip)} is not an actual promoted "
                "and selected block")

    base_arch = _arm64(base_path)
    new_arch = _arm64(tmp)
    manifest["aot_sha256"] = new.sha256
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
                "base_aot_sha256": base.sha256,
                "base_selected_blocks": [hex(x) for x in manifest_base_selected],
                "selected_blocks": [hex(x) for x in manifest_selected_blocks],
                "promoted_blocks": [hex(x) for x in manifest_promoted],
                "demoted_blocks": [hex(x) for x in manifest_demoted],
                "promoted_rip": hex(promoted_rip) if promoted_rip is not None else None,
                "arch": new_arch,
                "base_arch": base_arch,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
