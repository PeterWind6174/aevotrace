#!/usr/bin/env python3
"""Shared loader/validator for generated AOT dylibs (P0-PE-DLL-01, ABI 2).

This module only reads the exported artifact symbols with ctypes.  It does
not parse COFF/PE, link the ORC runtime, or dispatch Guest code.  Supplement,
publisher, the PE runner and GUI consumers use it to make the actual
base/new dylibs the provenance authority.
"""
from __future__ import annotations

import ctypes
import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping

TARGET = "arm64-apple-macosx14.0.0"
AEV_GUEST_ABI_VERSION = 2
AEV_ARTIFACT_ABI_VERSION = 2
AEV_TRANSLATOR_ABI_VERSION = 2
AEV_GUEST_STATE_SIZE = 296
AEV_MODULE_KINDS = ("coff", "pe32+amd64-dll")


class ArtifactError(RuntimeError):
    """The actual dylib is missing a symbol or has an invalid artifact table."""


class AevBlock(ctypes.Structure):
    _fields_ = [("rip", ctypes.c_uint64), ("fn", ctypes.c_void_p)]


@dataclass(frozen=True)
class Artifact:
    path: Path
    sha256: str
    input_sha256: str
    input_fnv: int
    entry_addr: int
    function_name_sha256: str
    target_triple: str
    guest_abi_version: int
    artifact_abi_version: int
    translator_abi_version: int
    guest_state_size: int
    all_blocks: tuple[int, ...]
    selected_blocks: tuple[int, ...]
    module_kind: str
    module_file_size: int
    preferred_base: int
    load_base: int
    image_size: int
    image_sha256: str
    import_binding_sha256: str
    host_binding_sha256: str
    entry_exports_sha256: str
    thunk_base: int
    import_count: int
    import_thunks: tuple[int, ...]
    relocs_stripped: int


def sha256_file(path: str | Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _u64(lib, name: str) -> int:
    try:
        return int(ctypes.c_uint64.in_dll(lib, name).value)
    except (ValueError, TypeError) as exc:
        raise ArtifactError(f"artifact missing {name}") from exc


def _bytes32(lib, name: str) -> bytes:
    try:
        return bytes((ctypes.c_ubyte * 32).in_dll(lib, name))
    except (ValueError, TypeError) as exc:
        raise ArtifactError(f"artifact missing {name}") from exc


def _cstr(lib, name: str, size: int = 48) -> str:
    try:
        raw = bytes((ctypes.c_char * size).in_dll(lib, name))
    except (ValueError, TypeError) as exc:
        raise ArtifactError(f"artifact missing {name}") from exc
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="strict")


def _u64_table(lib, name: str, count: int) -> tuple[int, ...]:
    if count == 0:
        return ()
    if count > 10_000_000:
        raise ArtifactError(f"{name} count is unreasonable: {count}")
    try:
        table = (ctypes.c_uint64 * count).in_dll(lib, name)
    except (ValueError, TypeError) as exc:
        raise ArtifactError(f"artifact missing {name}") from exc
    return tuple(int(x) for x in table)


def load_artifact(path: str | Path) -> Artifact:
    """Load and validate the actual exported table/identity of one dylib."""
    path = Path(path).resolve()
    if not path.is_file():
        raise ArtifactError(f"artifact does not exist: {path}")
    try:
        lib = ctypes.CDLL(str(path), mode=ctypes.RTLD_LOCAL)
    except OSError as exc:
        raise ArtifactError(f"cannot load artifact {path}: {exc}") from exc

    try:
        run = getattr(lib, "aev_aot_run")
    except AttributeError as exc:
        raise ArtifactError(f"artifact missing aev_aot_run: {path}") from exc
    if run is None:
        raise ArtifactError(f"artifact has null aev_aot_run: {path}")

    guest_abi = _u64(lib, "aev_aot_abi_version")
    artifact_abi = _u64(lib, "aev_artifact_abi_version")
    translator_abi = _u64(lib, "aev_translator_abi_version")
    guest_state_size = _u64(lib, "aev_guest_state_size")
    if guest_abi != AEV_GUEST_ABI_VERSION:
        raise ArtifactError(f"guest ABI {guest_abi} != {AEV_GUEST_ABI_VERSION}")
    if artifact_abi != AEV_ARTIFACT_ABI_VERSION:
        raise ArtifactError(
            f"artifact ABI {artifact_abi} != {AEV_ARTIFACT_ABI_VERSION}")
    if translator_abi != AEV_TRANSLATOR_ABI_VERSION:
        raise ArtifactError(
            f"translator ABI {translator_abi} != {AEV_TRANSLATOR_ABI_VERSION}")
    if guest_state_size != AEV_GUEST_STATE_SIZE:
        raise ArtifactError(
            f"guest state size {guest_state_size} != {AEV_GUEST_STATE_SIZE}")

    target = _cstr(lib, "aev_target_triple")
    if target != TARGET:
        raise ArtifactError(f"target triple mismatch: {target!r}")
    input_sha = _bytes32(lib, "aev_input_sha256").hex()
    function_sha = _bytes32(lib, "aev_function_name_sha256").hex()

    module_kind = _cstr(lib, "aev_module_kind", 32)
    if module_kind not in AEV_MODULE_KINDS:
        raise ArtifactError(f"unknown module kind: {module_kind!r}")
    module_file_size = _u64(lib, "aev_module_file_size")
    preferred_base = _u64(lib, "aev_preferred_base")
    load_base = _u64(lib, "aev_load_base")
    image_size = _u64(lib, "aev_image_size")
    image_sha = _bytes32(lib, "aev_image_sha256").hex()
    import_sha = _bytes32(lib, "aev_import_binding_sha256").hex()
    host_sha = _bytes32(lib, "aev_host_binding_sha256").hex()
    entry_sha = _bytes32(lib, "aev_entry_exports_sha256").hex()
    thunk_base = _u64(lib, "aev_thunk_base")
    import_count = _u64(lib, "aev_import_count")
    if import_count > 256:
        raise ArtifactError(f"import count is unreasonable: {import_count}")
    import_thunks = _u64_table(lib, "aev_import_thunks", import_count)
    relocs_stripped = _u64(lib, "aev_relocs_stripped")

    if module_file_size == 0:
        raise ArtifactError("module_file_size is zero")
    if module_kind == "coff":
        if (preferred_base or load_base or image_size or thunk_base or
                import_count or import_thunks or relocs_stripped):
            raise ArtifactError("COFF artifact has non-zero PE module fields")
        if (image_sha != "0" * 64 or import_sha != "0" * 64 or
                host_sha != "0" * 64 or entry_sha != "0" * 64):
            raise ArtifactError("COFF artifact has non-zero module hashes")
    else:
        if load_base == 0 or image_size == 0:
            raise ArtifactError("PE artifact missing load_base/image_size")
        if image_sha == "0" * 64 or import_sha == "0" * 64 or \
                host_sha == "0" * 64 or entry_sha == "0" * 64:
            raise ArtifactError("PE artifact has zero module identity hash")
        if import_count == 0:
            if thunk_base != 0 or import_thunks:
                raise ArtifactError("PE artifact has thunk table without imports")
        else:
            if thunk_base == 0 or len(import_thunks) != import_count:
                raise ArtifactError("PE artifact thunk table/count mismatch")
            expected = tuple(thunk_base + i * 16 for i in range(import_count))
            if import_thunks != expected:
                raise ArtifactError("PE artifact thunk table is not canonical")
        if input_sha != _bytes32(lib, "aev_input_sha256").hex():
            raise ArtifactError("input_sha256 changed while reading artifact")

    all_count = _u64(lib, "aev_all_block_count")
    selected_count = _u64(lib, "aev_block_count")
    if all_count > 10_000_000 or selected_count > all_count:
        raise ArtifactError(
            f"invalid table counts selected={selected_count} all={all_count}")
    all_blocks = _u64_table(lib, "aev_all_blocks", all_count)
    if all_blocks != tuple(sorted(set(all_blocks))):
        raise ArtifactError("all-block table is not sorted/unique")

    try:
        table = (AevBlock * selected_count).in_dll(lib, "aev_blocks")
    except (ValueError, TypeError) as exc:
        raise ArtifactError("artifact missing aev_blocks") from exc
    selected: list[int] = []
    for i in range(selected_count):
        block = table[i]
        rip = int(block.rip)
        if not block.fn:
            raise ArtifactError(f"selected block {rip:#x} has null fn")
        if rip not in all_blocks:
            raise ArtifactError(f"selected block {rip:#x} is not in all table")
        symbol_name = f"aev_blk_{rip:x}"
        try:
            symbol = getattr(lib, symbol_name)
        except AttributeError as exc:
            raise ArtifactError(
                f"selected block symbol missing: {symbol_name}") from exc
        if ctypes.cast(block.fn, ctypes.c_void_p).value != ctypes.cast(
                symbol, ctypes.c_void_p).value:
            raise ArtifactError(
                f"selected fn pointer mismatch at {rip:#x}")
        selected.append(rip)
    if selected != sorted(set(selected)):
        raise ArtifactError("selected table is not sorted/unique")

    return Artifact(
        path=path,
        sha256=sha256_file(path),
        input_sha256=input_sha,
        input_fnv=_u64(lib, "aev_input_hash"),
        entry_addr=_u64(lib, "aev_entry_addr"),
        function_name_sha256=function_sha,
        target_triple=target,
        guest_abi_version=guest_abi,
        artifact_abi_version=artifact_abi,
        translator_abi_version=translator_abi,
        guest_state_size=guest_state_size,
        all_blocks=all_blocks,
        selected_blocks=tuple(selected),
        module_kind=module_kind,
        module_file_size=module_file_size,
        preferred_base=preferred_base,
        load_base=load_base,
        image_size=image_size,
        image_sha256=image_sha,
        import_binding_sha256=import_sha,
        host_binding_sha256=host_sha,
        entry_exports_sha256=entry_sha,
        thunk_base=thunk_base,
        import_count=import_count,
        import_thunks=import_thunks,
        relocs_stripped=relocs_stripped,
    )


def require_identity(artifact: Artifact, expected: Mapping[str, object]) -> None:
    """Compare an actual artifact with a trusted expected identity.

    Keys use the manifest/trace vocabulary.  Missing/None keys are skipped;
    callers decide which fields their trust boundary can provide.
    """
    checks = {
        "input_sha256": artifact.input_sha256,
        "input_fnv": artifact.input_fnv,
        "entry_addr": artifact.entry_addr,
        "function_name_sha256": artifact.function_name_sha256,
        "target_triple": artifact.target_triple,
        "guest_abi_version": artifact.guest_abi_version,
        "artifact_abi_version": artifact.artifact_abi_version,
        "translator_abi_version": artifact.translator_abi_version,
        "guest_state_size": artifact.guest_state_size,
        "module_kind": artifact.module_kind,
        "module_file_size": artifact.module_file_size,
        "preferred_base": artifact.preferred_base,
        "load_base": artifact.load_base,
        "image_size": artifact.image_size,
        "image_sha256": artifact.image_sha256,
        "import_binding_sha256": artifact.import_binding_sha256,
        "host_binding_sha256": artifact.host_binding_sha256,
        "entry_exports_sha256": artifact.entry_exports_sha256,
        "thunk_base": artifact.thunk_base,
        "import_count": artifact.import_count,
        "relocs_stripped": artifact.relocs_stripped,
    }
    for key, actual in checks.items():
        if key not in expected or expected[key] is None:
            continue
        if actual != expected[key]:
            raise ArtifactError(
                f"{key} mismatch: actual={actual!r} expected={expected[key]!r}")
    expected_thunks = expected.get("import_thunks")
    if expected_thunks is not None:
        if tuple(int(x) for x in expected_thunks) != artifact.import_thunks:
            raise ArtifactError(
                "import thunk table mismatch: actual="
                f"{[hex(x) for x in artifact.import_thunks]} expected="
                f"{[hex(int(x)) for x in expected_thunks]}")
    expected_all = expected.get("all_blocks")
    if expected_all is not None:
        if tuple(int(x) for x in expected_all) != artifact.all_blocks:
            raise ArtifactError(
                "all-block table mismatch: actual="
                f"{[hex(x) for x in artifact.all_blocks]} expected="
                f"{[hex(int(x)) for x in expected_all]}")
    expected_selected = expected.get("selected_blocks")
    if expected_selected is not None:
        if tuple(int(x) for x in expected_selected) != artifact.selected_blocks:
            raise ArtifactError(
                "selected table mismatch: actual="
                f"{[hex(x) for x in artifact.selected_blocks]} expected="
                f"{[hex(int(x)) for x in expected_selected]}")


def manifest_identity(manifest: Mapping[str, object]) -> dict:
    """Translate a supplement manifest into require_identity keys."""
    imports = manifest.get("imports")
    thunk_vals = manifest.get("import_thunks")
    if thunk_vals is None and isinstance(imports, list):
        thunk_vals = [int(item["thunk_va"]) for item in imports]
    return {
        "input_sha256": manifest.get("input_sha256"),
        "input_fnv": int(manifest["input_hash"])
        if manifest.get("input_hash") is not None else None,
        "entry_addr": int(manifest["entry_address"])
        if manifest.get("entry_address") is not None else None,
        "function_name_sha256": manifest.get("entry_function_sha256"),
        "target_triple": manifest.get("target"),
        "artifact_abi_version": manifest.get("artifact_abi"),
        "translator_abi_version": manifest.get("translator_abi"),
        "module_kind": manifest.get("module_kind"),
        "module_file_size": manifest.get("module_file_size"),
        "preferred_base": manifest.get("preferred_base"),
        "load_base": manifest.get("load_base"),
        "image_size": manifest.get("image_size"),
        "image_sha256": manifest.get("image_sha256"),
        "import_binding_sha256": manifest.get("import_binding_sha256"),
        "host_binding_sha256": manifest.get("host_binding_sha256"),
        "entry_exports_sha256": manifest.get("entry_exports_sha256"),
        "thunk_base": manifest.get("thunk_base"),
        "import_count": manifest.get("import_count"),
        "import_thunks": thunk_vals,
        "relocs_stripped": manifest.get("relocs_stripped"),
    }


def manifest_all_blocks(manifest: Mapping[str, object]) -> tuple[int, ...]:
    values: Iterable[int] = (
        int(block["rip"])
        for fn in manifest.get("functions", [])
        for block in fn.get("blocks", [])
    )
    return tuple(sorted(values))


def manifest_selected(manifest: Mapping[str, object]) -> tuple[int, ...]:
    return tuple(int(x) for x in manifest.get("selected_blocks", []))
