"""Run the P0-UI-01 guest DLL against the compat or native macOS backend.

Usage (from spikes/p0-ui):
    .venv/bin/python -m harness.run --backend native --dll artifacts/p0_gui_test.dll \
        --out evidence/native
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
import traceback

from AppKit import NSApplication, NSApplicationActivationPolicyRegular

from . import guest_pe
from .compat_backend import CompatBackend
from .guest_pe import PEImage, UnsupportedApi
from .logging_util import RunLogger
from .native_backend import NativeBackend
from .scenario import Scenario
from .win32_host import Win32Host


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def harness_source_hashes() -> dict:
    here = os.path.dirname(os.path.abspath(__file__))
    result = {}
    for name in sorted(os.listdir(here)):
        if name.endswith(".py"):
            path = os.path.join(here, name)
            result[name] = sha256_file(path)
    return result


def repo_info() -> dict:
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    info = {"source_root": root}
    try:
        info["git_head"] = subprocess.check_output(
            ["git", "-C", root, "rev-parse", "HEAD"], text=True).strip()
        info["git_branch"] = subprocess.check_output(
            ["git", "-C", root, "branch", "--show-current"], text=True).strip()
    except Exception as exc:  # noqa: BLE001
        info["git_error"] = str(exc)
    return info


def dependency_info() -> dict:
    info = {
        "python": sys.version,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "mac_ver": platform.mac_ver()[0],
        "unicorn": getattr(guest_pe, "__version__", None),
    }
    try:
        import objc
        info["pyobjc"] = getattr(objc, "__version__", "unknown")
    except Exception:  # noqa: BLE001
        info["pyobjc"] = None
    try:
        import pefile
        info["pefile"] = getattr(pefile, "__version__", "unknown")
    except Exception:  # noqa: BLE001
        info["pefile"] = None
    return info


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=["compat", "native"], required=True)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args(argv)

    dll_path = os.path.abspath(args.dll)
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    app = NSApplication.sharedApplication()
    app.setActivationPolicy_(NSApplicationActivationPolicyRegular)
    app.finishLaunching()

    dll_hash = sha256_file(dll_path)
    logger = RunLogger(out_dir, args.backend)
    base_summary = {
        "task": "P0-UI-01",
        "mode": "spike",
        "backend": args.backend,
        "dll": dll_path,
        "dll_sha256": dll_hash,
        "harness_sources": harness_source_hashes(),
        "repo": repo_info(),
        "dependencies": dependency_info(),
        "argv": sys.argv,
        "limitations": [
            "P0 temporary Unicorn x86-64 execution bridge; not the product AOT/JIT core",
            "host-owned message dispatcher; guest GetMessage/DispatchMessage is exercised only by the Windows reference run",
            "compat is a P0 semantic harness, not a bundled final compat runtime; D3 remains open",
            "no TLS/SEH/heap/real loader; declared Win32 import subset only",
        ],
    }
    exit_code = 0
    try:
        image = PEImage(dll_path, logger)
        logger.event("guest.pe_loaded", path=dll_path, sha256=dll_hash,
                     image_base=f"0x{image.base:x}", image_size=image.size,
                     imports=[repr(s) for s in image.imports])
        backend_cls = NativeBackend if args.backend == "native" else CompatBackend
        backend = backend_cls(None, logger, out_dir)
        host = Win32Host(backend, image, logger, out_dir)
        backend.host = host
        scenario = Scenario(host, backend, image, logger, dll_hash)
        host.start()
        summary = scenario.run()
        summary.update(base_summary)
        summary["backend_proof"] = backend.proof()
        shots = []
        for p in sorted(scenario.screenshots):
            if not p or not os.path.exists(p):
                continue
            try:
                file_out = subprocess.check_output(["file", p], text=True).strip()
            except Exception as exc:  # noqa: BLE001
                file_out = f"file failed: {exc}"
            shots.append({"path": p, "sha256": sha256_file(p),
                          "size": os.path.getsize(p), "file": file_out})
        summary["screenshots"] = shots
        summary["success"] = bool(summary.get("success")) and bool(summary["screenshots"])
        exit_code = 0 if summary["success"] else 1
    except Exception as exc:  # noqa: BLE001 - evidence must capture hard failures
        logger.error = "".join(traceback.format_exception(type(exc), exc, exc.__traceback__))
        logger.event("run.error", error=str(exc), trace=traceback.format_exc())
        summary = dict(base_summary)
        summary.update({
            "success": False,
            "error": str(exc),
            "traceback": logger.error,
            "screenshots": [],
        })
        exit_code = 1
    finally:
        logger.close()

    path = logger.write_summary(summary)
    print(json.dumps({
        "success": summary.get("success"),
        "backend": args.backend,
        "summary": path,
        "states": {k: v for k, v in (summary.get("states") or {}).items()},
        "negative_case": summary.get("negative_case"),
        "screenshots": summary.get("screenshots"),
        "error": summary.get("error"),
    }, indent=2, ensure_ascii=False))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
