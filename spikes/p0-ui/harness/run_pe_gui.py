"""Run the P0-PE-GUI-01 persistent PE session GUI (compat or native).

One window owns one ``libaev_pe_session.dylib`` session created on the AppKit
main thread.  The script records binary identities, dependency/linkage proof,
the automated scenario result, screenshots and a success/failure summary.
``--force-native-failure`` must raise inside the native backend and must not
fall back to compat.
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
from .logging_util import RunLogger
from .native_backend import NativeBackend
from .pe_host import PeGuiHost
from .pe_scenario import PeScenario


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
        info["git_status"] = subprocess.check_output(
            ["git", "-C", root, "status", "--short"], text=True).strip()
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
        "engine_host_loaded": "harness.engine_host" in sys.modules,
        "engine_bridge_loaded": "harness.engine_bridge" in sys.modules,
        "pe_session_loaded": "harness.pe_session" in sys.modules,
    }
    try:
        import objc
        info["pyobjc"] = getattr(objc, "__version__", "unknown")
    except Exception:  # noqa: BLE001
        info["pyobjc"] = None
    return info


def linkage_info(path: str) -> dict:
    info: dict = {"path": path}
    try:
        info["file"] = subprocess.check_output(["file", path], text=True).strip()
    except Exception as exc:  # noqa: BLE001
        info["file_error"] = str(exc)
    try:
        text = subprocess.check_output(["otool", "-L", path], text=True)
    except Exception as exc:  # noqa: BLE001
        info["otool_error"] = str(exc)
        info["has_unicorn"] = None
        info["has_pefile"] = None
        return info
    info["otool_l"] = text
    lowered = text.lower()
    info["has_unicorn"] = "unicorn" in lowered
    info["has_pefile"] = "pefile" in lowered
    info["has_guest_pe"] = "guest_pe" in lowered
    return info


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


def _event_counts(logger: RunLogger) -> dict:
    compat = sum(v for k, v in logger.kind_counts.items() if k.startswith("compat."))
    native = sum(v for k, v in logger.kind_counts.items() if k.startswith("native."))
    return {"native": native, "compat": compat}


def _extra_checks(summary: dict, backend: str, deps: dict,
                   linkage: dict, forced_failure: bool) -> dict:
    scenario_ok = bool(summary.get("success"))
    proof = summary.get("backend_proof") or {}
    event_counts = summary.get("backend_event_counts") or {}
    session = summary.get("session_proof") or {}
    counts = session.get("counts") or {}
    checks = {
        "scenario_success": scenario_ok,
        "screenshots_present": bool(summary.get("screenshots")),
        "no_unicorn": not deps.get("unicorn_loaded"),
        "no_pefile": not deps.get("pefile_loaded"),
        "no_guest_pe": not deps.get("guest_pe_loaded"),
        "no_engine_host": not deps.get("engine_host_loaded"),
        "no_engine_bridge": not deps.get("engine_bridge_loaded"),
        "library_links_no_unicorn": linkage.get("has_unicorn") is False,
        "library_links_no_pefile": linkage.get("has_pefile") is False,
        "library_linkage_readable": "otool_error" not in linkage,
        "session_load_count_one": counts.get("load_count") == 1,
        "session_created_once": session.get("session_create_count") == 1,
        "forced_failure_not_success": not forced_failure,
    }
    if backend == "native":
        checks.update({
            "native_main_thread": proof.get("main_thread") is True,
            "native_no_compat_classes":
                proof.get("compat_classes_used") is False,
            "native_no_compat_events": event_counts.get("compat") == 0,
        })
    else:
        checks.update({
            "compat_no_native_events": event_counts.get("native") == 0,
            "compat_classes":
                all(str(c.get("objc_class", "")).startswith("Compat")
                    for c in proof.get("controls", [])),
        })
    return checks


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=["compat", "native"], required=True)
    parser.add_argument("--lib", required=True)
    parser.add_argument("--dll", required=True)
    parser.add_argument("--registry", required=True)
    parser.add_argument("--aot", required=True)
    parser.add_argument("--load-base", default="0x180000000")
    parser.add_argument("--entry-export", default="AevDllCompute")
    parser.add_argument("--out", required=True)
    parser.add_argument("--trace", default="")
    parser.add_argument("--coverage-profile", choices=["base", "supplemented"],
                        default="base")
    parser.add_argument("--base-aot", default=os.environ.get("AEV_PE_BASE_AOT", ""))
    parser.add_argument("--supplement-manifest", default="")
    parser.add_argument("--force-native-failure", action="store_true")
    args = parser.parse_args(argv)

    lib = os.path.abspath(args.lib)
    dll = os.path.abspath(args.dll)
    registry = os.path.abspath(args.registry)
    aot = os.path.abspath(args.aot)
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    load_base = int(args.load_base, 0)
    if load_base <= 0:
        raise SystemExit("--load-base must be a non-zero Guest VA")
    if args.force_native_failure and args.backend != "native":
        raise SystemExit("--force-native-failure requires --backend native")

    trace_path = os.path.abspath(args.trace) if args.trace else None
    if trace_path:
        os.makedirs(os.path.dirname(trace_path), exist_ok=True)

    base_aot = os.path.abspath(args.base_aot) if args.base_aot else None
    manifest = None
    manifest_path = None
    if args.coverage_profile == "supplemented":
        if not args.supplement_manifest:
            raise SystemExit("supplemented profile requires --supplement-manifest")
        if not base_aot:
            raise SystemExit("supplemented profile requires --base-aot")
        manifest_path = os.path.abspath(args.supplement_manifest)
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest = json.load(f)
        if not isinstance(manifest, dict):
            raise SystemExit("supplement manifest must be a JSON object")
        declared_input = manifest.get("input_sha256") or manifest.get(
            "module_file_sha256")
        if declared_input and str(declared_input).lower() != sha256_file(dll):
            raise SystemExit("supplement manifest input SHA does not match the DLL")
    elif args.supplement_manifest:
        raise SystemExit("base profile does not take --supplement-manifest")

    for path, label, required in (
        (lib, "session library", True),
        (dll, "DLL", True),
        (registry, "registry", True),
        (aot, "AOT dylib", True),
        (base_aot, "base AOT", False),
        (manifest_path, "supplement manifest", False),
    ):
        if required and (not path or not os.path.isfile(path)):
            raise SystemExit(f"{label} not found: {path}")

    app = NSApplication.sharedApplication()
    app.setActivationPolicy_(NSApplicationActivationPolicyRegular)
    app.finishLaunching()

    logger = RunLogger(out_dir, args.backend)
    base_summary = {
        "task": "P0-PE-GUI-01",
        "mode": "pe-session-gui",
        "backend": args.backend,
        "coverage_profile": args.coverage_profile,
        "library": lib,
        "library_sha256": sha256_file(lib),
        "dll": dll,
        "dll_sha256": sha256_file(dll),
        "registry": registry,
        "registry_sha256": sha256_file(registry),
        "aot": aot,
        "aot_sha256": sha256_file(aot),
        "base_aot": base_aot,
        "base_aot_sha256": sha256_file(base_aot) if base_aot else None,
        "supplement_manifest": manifest_path,
        "supplement_manifest_sha256":
            sha256_file(manifest_path) if manifest_path else None,
        "load_base": f"0x{load_base:x}",
        "entry_export": args.entry_export,
        "trace": trace_path,
        "harness_sources": harness_source_hashes(),
        "repo": repo_info(),
        "dependencies": dependency_info(),
        "library_linkage": linkage_info(lib),
        "argv": sys.argv,
        "limitations": [
            "P0 bounded single PE32+ DLL session; no EXE/Win32 message pump",
            "automatic AppKit events and screenshots only; physical keyboard/"
            "focus/IME/manual appearance remain for human acceptance",
            "base/supplemented JIT/AOT evidence binds the supplied artifacts; "
            "offline supplement construction is owned by the compiler task",
            "persistent session is proven by load_count==1 over the scenario, "
            "not a performance measurement",
        ],
    }
    exit_code = 0
    host = None
    scenario = None
    backend = None
    try:
        backend_cls = NativeBackend if args.backend == "native" else CompatBackend
        backend = backend_cls(None, logger, out_dir)
        host = PeGuiHost(
            backend, logger, out_dir, lib, dll, registry, aot, load_base,
            entry_export=args.entry_export, trace_path=trace_path,
        )
        backend.host = host
        scenario = PeScenario(
            host, backend, logger, out_dir, dll, registry, aot, load_base,
            coverage_profile=args.coverage_profile,
            base_aot=base_aot, manifest=manifest,
            manifest_path=manifest_path, trace_path=trace_path,
            entry_export=args.entry_export,
        )
        host.start()
        if args.force_native_failure:
            logger.event("pe.force_native_failure_begin")
            # This must raise from the native backend.  There is no compat
            # fallback path in PeGuiHost or NativeBackend.
            backend.create_control(
                0x9999, "unsupported-kind", "x", 0, 0, 0, 10, 10, 9999
            )
            raise AssertionError("native backend accepted an unsupported control")
        summary = scenario.run()
        summary.update(base_summary)
        summary["dependencies_after"] = dependency_info()
        summary["library_linkage_after"] = linkage_info(lib)
        summary["process"] = {
            "pid": os.getpid(),
            "session_library_in_process": True,
            "runner_process_spawned": False,
            "backend": args.backend,
        }
        summary["screenshots_detail"] = _screenshot_info(
            summary.get("screenshots") or [])
        summary["forced_failure"] = False
        extra = _extra_checks(summary, args.backend,
                              summary["dependencies_after"],
                              summary["library_linkage_after"], False)
        summary["extra_checks"] = extra
        summary["success"] = bool(summary.get("success")) and all(extra.values())
        if not summary["success"]:
            summary["error"] = "one or more GUI/evidence checks failed"
        exit_code = 0 if summary["success"] else 1
    except Exception as exc:  # noqa: BLE001 - evidence must capture hard failures
        logger.error = "".join(
            traceback.format_exception(type(exc), exc, exc.__traceback__))
        logger.event("pe.run_error", error=str(exc),
                     trace=traceback.format_exc())
        summary = dict(base_summary)
        backend_proof = None
        if backend is not None:
            try:
                backend_proof = backend.proof()
            except Exception as proof_exc:  # noqa: BLE001
                backend_proof = {"error": str(proof_exc)}
        counts = _event_counts(logger)
        deps_after = dependency_info()
        summary.update({
            "success": False,
            "error": str(exc),
            "traceback": logger.error,
            "screenshots": list(scenario.screenshots) if scenario else [],
            "screenshots_detail": _screenshot_info(
                list(scenario.screenshots) if scenario else []),
            "forced_failure": bool(args.force_native_failure),
            "backend_proof": backend_proof,
            "backend_event_counts": counts,
            "dependencies_after": deps_after,
            "library_linkage_after": linkage_info(lib),
            "process": {
                "pid": os.getpid(),
                "session_library_in_process": True,
                "runner_process_spawned": False,
                "backend": args.backend,
            },
            "checks": dict(scenario.checks) if scenario else {},
            "states": dict(scenario.states) if scenario else {},
            "negative": dict(scenario.negative) if scenario else {},
            "trace_proof": scenario.trace_proof if scenario else None,
        })
        exit_code = 1
    finally:
        if host is not None:
            try:
                host.close(reason="run_pe_gui cleanup")
            except Exception as exc:  # noqa: BLE001
                logger.event("pe.close_error", error=str(exc))
        logger.close()

    if "summary" not in locals():
        summary = dict(base_summary)
        summary["success"] = False
        summary["error"] = "run aborted before summary"
    path = logger.write_summary(summary)
    print(json.dumps({
        "success": summary.get("success"),
        "backend": args.backend,
        "coverage_profile": args.coverage_profile,
        "summary": path,
        "check_count": summary.get("check_count"),
        "checks": summary.get("checks"),
        "extra_checks": summary.get("extra_checks"),
        "negative": summary.get("negative"),
        "backend_proof": summary.get("backend_proof"),
        "session_proof": summary.get("session_proof"),
        "trace_proof": summary.get("trace_proof"),
        "screenshots": summary.get("screenshots"),
        "error": summary.get("error"),
    }, indent=2, ensure_ascii=False))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
