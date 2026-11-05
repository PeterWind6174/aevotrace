"""Run the P0 engine-backed callback GUI with the compat or native backend.

This entry point deliberately does not import harness.run / guest_pe / unicorn.
It loads libaev_bridge.dylib and the fixed COFF fixture, then drives the
existing compat/native AppKit backends through a shared minimal host.
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

from .compat_backend import CompatBackend
from .engine_host import EngineGuiHost
from .engine_scenario import EngineScenario
from .logging_util import RunLogger
from .native_backend import NativeBackend


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
            result[name] = sha256_file(os.path.join(here, name))
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
        "unicorn_loaded": "unicorn" in sys.modules,
        "pefile_loaded": "pefile" in sys.modules,
        "guest_pe_loaded": "harness.guest_pe" in sys.modules,
    }
    try:
        import objc
        info["pyobjc"] = getattr(objc, "__version__", "unknown")
    except Exception:  # noqa: BLE001
        info["pyobjc"] = None
    return info


def bridge_linkage(path: str) -> dict:
    try:
        text = subprocess.check_output(["otool", "-L", path], text=True)
    except Exception as exc:  # noqa: BLE001
        return {"error": str(exc)}
    return {
        "otool_l": text,
        "has_unicorn": "unicorn" in text.lower(),
        "has_pefile": "pefile" in text.lower(),
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=["compat", "native"], required=True)
    parser.add_argument("--lib", required=True)
    parser.add_argument("--coff", required=True)
    parser.add_argument("--aot", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--force-native-failure", action="store_true")
    args = parser.parse_args(argv)

    lib = os.path.abspath(args.lib)
    coff = os.path.abspath(args.coff)
    aot = os.path.abspath(args.aot)
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    app = NSApplication.sharedApplication()
    app.setActivationPolicy_(NSApplicationActivationPolicyRegular)
    app.finishLaunching()

    logger = RunLogger(out_dir, args.backend)
    base_summary = {
        "task": "P0-UI-INT-01",
        "mode": "engine-callback",
        "backend": args.backend,
        "library": lib,
        "library_sha256": sha256_file(lib),
        "coff": coff,
        "coff_sha256": sha256_file(coff),
        "aot": aot,
        "aot_sha256": sha256_file(aot),
        "harness_sources": harness_source_hashes(),
        "repo": repo_info(),
        "dependencies": dependency_info(),
        "bridge_linkage": bridge_linkage(lib),
        "argv": sys.argv,
        "limitations": [
            "P0 minimal single-level callback; no general PE/Win32 loader",
            "no arbitrary reentry/threading/modal loops; unsupported cases fail explicitly",
            "compat is a P0 semantic harness, not the final compat runtime; D3 remains open",
            "automatic events only; physical keyboard/focus/IME/manual appearance pending",
            "100 callbacks is a stability smoke, not a performance measurement",
        ],
    }
    exit_code = 0
    host = None
    backend = None
    try:
        backend_cls = NativeBackend if args.backend == "native" else CompatBackend
        backend = backend_cls(None, logger, out_dir)
        host = EngineGuiHost(backend, logger, out_dir, lib, coff, aot)
        backend.host = host
        scenario = EngineScenario(host, backend, logger, out_dir, lib, coff, aot)
        host.start()
        if args.force_native_failure:
            if args.backend != "native":
                raise ValueError("--force-native-failure requires --backend native")
            logger.event("engine.force_native_failure_begin")
            # This must raise from the native backend; it must not fall back
            # to the compat backend or silently create compat controls.
            backend.create_control(0x9999, "unsupported-kind", "x", 0, 0, 0,
                                   10, 10, 9999)
            raise AssertionError("native backend accepted an unsupported control")
        summary = scenario.run()
        summary.update(base_summary)
        summary["dependencies_after"] = dependency_info()
        summary["screenshots"] = _screenshot_info(scenario.screenshots)
        summary["success"] = bool(summary.get("success")) and bool(summary["screenshots"])
        exit_code = 0 if summary["success"] else 1
    except Exception as exc:  # noqa: BLE001 - evidence must capture hard failures
        logger.error = "".join(traceback.format_exception(type(exc), exc, exc.__traceback__))
        logger.event("engine.run_error", error=str(exc), trace=traceback.format_exc())
        summary = dict(base_summary)
        compat_events = sum(v for k, v in logger.kind_counts.items()
                            if k.startswith("compat."))
        native_events = sum(v for k, v in logger.kind_counts.items()
                            if k.startswith("native."))
        backend_proof = None
        if backend is not None:
            try:
                backend_proof = backend.proof()
            except Exception as proof_exc:  # noqa: BLE001
                backend_proof = {"error": str(proof_exc)}
        summary.update({
            "success": False,
            "error": str(exc),
            "traceback": logger.error,
            "screenshots": [],
            "forced_failure": bool(args.force_native_failure),
            "backend_proof": backend_proof,
            "backend_event_counts": {"native": native_events,
                                     "compat": compat_events},
        })
        exit_code = 1
    finally:
        if host is not None:
            try:
                host.close(reason="run_engine cleanup")
            except Exception as exc:  # noqa: BLE001
                logger.event("engine.close_error", error=str(exc))
        logger.close()

    path = logger.write_summary(summary)
    print(json.dumps({
        "success": summary.get("success"),
        "backend": args.backend,
        "summary": path,
        "checks": summary.get("checks"),
        "negative": summary.get("negative"),
        "bridge_proof": summary.get("bridge_proof"),
        "screenshots": summary.get("screenshots"),
        "error": summary.get("error"),
    }, indent=2, ensure_ascii=False))
    return exit_code


def _screenshot_info(paths: list[str]) -> list[dict]:
    result = []
    for path in paths:
        if not os.path.exists(path):
            continue
        try:
            file_out = subprocess.check_output(["file", path], text=True).strip()
        except Exception as exc:  # noqa: BLE001
            file_out = f"file failed: {exc}"
        result.append({
            "path": path,
            "sha256": sha256_file(path),
            "size": os.path.getsize(path),
            "file": file_out,
        })
    return result


if __name__ == "__main__":
    raise SystemExit(main())
