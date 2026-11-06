#!/usr/bin/env python3
"""P0-PE-DLL-01: end-to-end PE32+ DLL / shared AOT+JIT verification.

This driver uses the committed self-built DLL/helper and the same DLL bytes on
macOS.  It does not recompile Guest C source for conversion; the Python oracle
below only models the fixture's expected business results for the 100-call
stability check.
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
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "spikes" / "p0-engine" / "scripts"))
from aot_artifact import load_artifact  # noqa: E402

PREFERRED = 0x180000000
NON_PREFERRED = 0x190000000
SENTINEL = 0xFFFFFFFFFFFFFF00


class Failure(RuntimeError):
    pass


class Driver:
    def __init__(self, build: Path, source: Path, results: Path) -> None:
        self.build = build.resolve()
        self.source = source.resolve()
        self.results = results.resolve()
        self.pe_dir = self.source / "testdata" / "pe"
        self.art = self.results / "artifacts"
        self.logs = self.results / "logs"
        self.art.mkdir(parents=True, exist_ok=True)
        self.logs.mkdir(parents=True, exist_ok=True)
        self.runner = self.build / "aev-pe-runner"
        self.translator = self.build / "aev-translate"
        self.rt_obj = self.build / "generated" / "aot_runtime.o"
        self.mem_obj = self.build / "generated" / "aev_mem_aot.o"
        self.clang = shutil.which("clang") or "/usr/bin/clang"
        self.checks: list[tuple[bool, str]] = []
        self.failures = 0
        for path in (self.runner, self.translator, self.rt_obj, self.mem_obj):
            if not path.is_file():
                raise Failure(f"missing build artifact: {path}")

    def check(self, condition: bool, message: str) -> None:
        self.checks.append((bool(condition), message))
        if not condition:
            self.failures += 1
            print(f"FAIL: {message}")

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            raise Failure(message)

    def log(self, name: str, text: str) -> Path:
        path = self.logs / name
        path.write_text(text)
        return path

    def run(self, cmd: list[str], name: str, timeout: int = 180) -> subprocess.CompletedProcess:
        proc = subprocess.run(cmd, text=True, capture_output=True, timeout=timeout)
        self.log(name + ".stdout", proc.stdout)
        self.log(name + ".stderr", proc.stderr)
        self.log(name + ".command", " ".join(str(x) for x in cmd) + "\n")
        return proc

    def runner_json(self, args: list[str], name: str, expect_error: bool = False):
        cmd = [str(self.runner), *args]
        if expect_error:
            cmd.append("--expect-error")
        proc = self.run(cmd, name)
        try:
            data = json.loads(proc.stdout)
        except json.JSONDecodeError as exc:
            raise Failure(f"{name}: runner did not emit JSON: {exc}: {proc.stdout[:500]}")
        if not expect_error and proc.returncode != 0:
            raise Failure(f"{name}: runner rc={proc.returncode}: {data}")
        return proc.returncode, data

    def translate(self, load_base: int, includes=None, excludes=None,
                  tag: str = "translate") -> tuple[Path, Path, dict]:
        exports = self.exports()
        ll = self.art / f"{tag}.ll"
        header = self.art / f"{tag}.h"
        manifest = self.art / f"{tag}.json"
        cmd = [str(self.translator), "--pe", str(self.pe_dir / "aevguest.dll"),
               "--load-base", hex(load_base),
               "--registry", str(self.pe_dir / "aevimports.txt"),
               "--entry", "AevDllCompute",
               "--out-ll", str(ll), "--out-header", str(header),
               "--out-manifest", str(manifest)]
        for e in exports:
            cmd += ["--export", e]
        for r in sorted(includes or []):
            cmd += ["--aot-include-rip", hex(r)]
        for r in sorted(excludes or []):
            cmd += ["--aot-exclude-rip", hex(r)]
        proc = self.run(cmd, tag + ".translate")
        self.require(proc.returncode == 0, f"translator {tag} failed: {proc.stderr}")
        return ll, manifest, json.loads(manifest.read_text())

    def build_dylib(self, ll: Path, tag: str) -> Path:
        obj = self.art / f"{tag}.o"
        dylib = self.art / f"lib{tag}.dylib"
        proc = self.run([self.clang, "--target=arm64-apple-macos14.0", "-O2",
                         "-c", str(ll), "-o", str(obj)], tag + ".clang-ir")
        self.require(proc.returncode == 0, f"clang IR {tag}: {proc.stderr}")
        proc = self.run([self.clang, "-arch", "arm64", "-dynamiclib", "-o",
                         str(dylib), str(obj), str(self.rt_obj), str(self.mem_obj)],
                        tag + ".clang-link")
        self.require(proc.returncode == 0, f"clang link {tag}: {proc.stderr}")
        return dylib

    def exports(self) -> list[str]:
        # Declared fixture interface; the names are part of the fixture ABI,
        # not file offsets or instruction boundaries.
        return sorted([
            "AevDllCallPtr", "AevDllCalls", "AevDllCompute",
            "AevDllCursorAddress", "AevDllCursorValue", "AevDllReadAt",
            "AevDllReset", "AevDllState", "AevDllWriteAt",
        ])

    def supplement(self, base_dylib: Path, traces: list[Path], load_base: int,
                   tag: str):
        exports = self.exports()
        ll = self.art / f"{tag}.ll"
        header = self.art / f"{tag}.h"
        manifest = self.art / f"{tag}.json"
        all_manifest = json.loads((self.art / "all_pe.json").read_text())
        entry_rva = all_manifest["entry_address"] - all_manifest["load_base"]
        cmd = [sys.executable, str(self.source / "scripts" / "aot_supplement.py"),
               "--translator", str(self.translator), "--input-kind", "pe",
               "--input", str(self.pe_dir / "aevguest.dll"),
               "--load-base", hex(load_base),
               "--registry", str(self.pe_dir / "aevimports.txt"),
               "--entry-export", "AevDllCompute",
               "--entry-addr", hex(load_base + entry_rva)]
        for e in exports:
            cmd += ["--export", e]
        cmd += ["--base-aot", str(base_dylib)]
        for trace in traces:
            cmd += ["--trace", str(trace)]
        cmd += ["--out-ll", str(ll), "--out-header", str(header),
                "--out-manifest", str(manifest)]
        proc = self.run(cmd, tag + ".supplement")
        self.require(proc.returncode == 0, f"supplement {tag}: {proc.stderr}")
        return ll, json.loads(manifest.read_text())

    def info(self, load_base: int, tag: str):
        _, data = self.runner_json(
            ["--dll", str(self.pe_dir / "aevguest.dll"),
             "--registry", str(self.pe_dir / "aevimports.txt"),
             "--load-base", hex(load_base), "--entry-export", "AevDllCompute",
             "--info"], tag)
        return data["iteration_0"]

    def calls(self, load_base: int, calls: list[str], tag: str, aot: Path | None = None,
              expect_error: bool = False, extra: list[str] | None = None):
        args = ["--dll", str(self.pe_dir / "aevguest.dll"),
                "--registry", str(self.pe_dir / "aevimports.txt"),
                "--load-base", hex(load_base),
                "--entry-export", "AevDllCompute"]
        if aot is not None:
            args += ["--aot", str(aot)]
        for c in calls:
            args += ["--call", c]
        if extra:
            args += extra
        return self.runner_json(args, tag, expect_error=expect_error)


def s32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value >= 0x80000000 else value


def u32(value: int) -> int:
    return value & 0xFFFFFFFF


class Oracle:
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


def parse_reference(path: Path):
    reset = None
    cases = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if line.startswith("reset "):
            reset = dict(re.findall(r"(\w+)=([-0-9xa-fA-F]+)", line))
            reset = {k: int(v, 0) for k, v in reset.items()}
        elif line.startswith("case="):
            match = re.match(r"case=(\w+)", line)
            if not match:
                continue
            name = match.group(1)
            fields = dict(re.findall(r"(\w+)=(-?[0-9xa-fA-F]+)", line))
            cases[name] = {k: int(v, 0) for k, v in fields.items() if k != "case"}
    if reset is None or not cases:
        raise Failure("cannot parse reference_stdout.txt")
    return reset, cases


def pe_headers(data: bytes):
    pe = int.from_bytes(data[0x3C:0x40], "little")
    if data[pe:pe + 4] != b"PE\0\0":
        raise Failure("bad PE in helper")
    nsec = int.from_bytes(data[pe + 6:pe + 8], "little")
    opt_size = int.from_bytes(data[pe + 20:pe + 22], "little")
    opt = pe + 24
    sec_table = opt + opt_size
    sections = {}
    for i in range(nsec):
        off = sec_table + i * 40
        name = data[off:off + 8].split(b"\0", 1)[0].decode("latin1")
        vsize = int.from_bytes(data[off + 8:off + 12], "little")
        va = int.from_bytes(data[off + 12:off + 16], "little")
        raw_size = int.from_bytes(data[off + 16:off + 20], "little")
        raw_ptr = int.from_bytes(data[off + 20:off + 24], "little")
        sections[name] = (va, vsize, raw_ptr, raw_size)
    data_dir = opt + 112
    return {
        "pe": pe,
        "opt": opt,
        "sections": sections,
        "export_rva": int.from_bytes(data[data_dir:data_dir + 4], "little"),
        "export_size": int.from_bytes(data[data_dir + 4:data_dir + 8], "little"),
    }


def rva_to_file(hdr, rva: int) -> int:
    for name, (va, vsize, raw_ptr, raw_size) in hdr["sections"].items():
        if va <= rva < va + max(vsize, raw_size):
            return raw_ptr + (rva - va)
    if rva < hdr["opt"]:
        return rva
    raise Failure(f"rva {rva:#x} not mapped")


def make_variant(driver: Driver, name: str, mutate) -> Path:
    data = bytearray((driver.pe_dir / "aevguest.dll").read_bytes())
    mutate(data)
    path = driver.art / name
    path.write_bytes(data)
    return path


def expect_load_error(driver: Driver, name: str, dll: Path, contains: str | None = None,
                      aot: Path | None = None, load_base: int = PREFERRED):
    args = ["--dll", str(dll), "--registry", str(driver.pe_dir / "aevimports.txt"),
            "--load-base", hex(load_base), "--entry-export", "AevDllCompute"]
    if aot is not None:
        args += ["--aot", str(aot)]
    args += ["--call", "AevDllCompute:1,2,3"]
    rc, data = driver.runner_json(args, name, expect_error=True)
    ok = data.get("status") == 3 and (contains is None or contains in data.get("error", ""))
    driver.check(ok, f"{name}: load rejected (status={data.get('status')} error={data.get('error')!r})")


def run(driver: Driver) -> int:
    pe_dir = driver.pe_dir
    sums = {}
    for line in (pe_dir / "SHA256SUMS").read_text().splitlines():
        digest, name = line.split()
        sums[name] = digest
    for name, digest in sums.items():
        actual = hashlib.sha256((pe_dir / name).read_bytes()).hexdigest()
        driver.check(actual == digest, f"fixture hash {name}")

    reset_ref, cases_ref = parse_reference(pe_dir / "reference_stdout.txt")

    # 1. Module layout / identity at both bases.
    info_pref = driver.info(PREFERRED, "info-preferred")
    m = info_pref["module"]
    driver.check(m["kind"] == "pe32+amd64-dll", "module kind")
    driver.check(m["load_base"] == hex(PREFERRED), "preferred load base")
    driver.check(m["image_size"] == 0x6000, "SizeOfImage")
    driver.check(m["import_count"] == 1, "import count")
    driver.check(m["relocation_count"] == 1, "relocation count")
    driver.check(m["pdata_count"] == 9, "pdata count")
    driver.check(m["thunk_base"] == hex(PREFERRED + 0x6000), "thunk base")
    driver.check(len(info_pref["exports"]) == 9, "export count")
    driver.check(len(info_pref["sections"]) == 5, "section count")
    info_non = driver.info(NON_PREFERRED, "info-nonpreferred")
    driver.check(info_non["module"]["load_base"] == hex(NON_PREFERRED), "non-preferred load base")
    driver.check(info_non["module"]["thunk_base"] == hex(NON_PREFERRED + 0x6000), "non-preferred thunk base")
    driver.check(info_non["module"]["image_sha256"] != info_pref["module"]["image_sha256"],
                 "image identity changes with load base")

    # 2. JIT-only business + Windows reference comparison.
    driver.translate(PREFERRED, tag="all_pe")
    case_order = list(cases_ref)
    calls = []
    for name in case_order:
        expected = cases_ref[name]
        calls += [f"AevDllReset:{reset_ref['seed']}",
                  f"AevDllCompute:{expected['a']},{expected['b']},{expected['n']}",
                  "AevDllState", "AevDllCursorValue", "AevDllCursorAddress",
                  "AevDllCalls"]
    rc, data = driver.calls(PREFERRED, calls, "jit-reference")
    it = data["iteration_0"]
    flat = it["calls"]
    driver.check(len(flat) == 6 * len(case_order), "reference call count")
    for index, name in enumerate(case_order):
        expected = cases_ref[name]
        group = {c["export"]: c for c in flat[index * 6:(index + 1) * 6]}
        driver.check(group["AevDllCompute"]["status"] == 0, f"{name}: JIT compute status")
        driver.check(s32(group["AevDllCompute"]["rax"]) == expected["result"],
                     f"{name}: result {group['AevDllCompute']['rax']} == {expected['result']}")
        driver.check(s32(group["AevDllState"]["rax"]) == expected["state"], f"{name}: state")
        driver.check(s32(group["AevDllCursorValue"]["rax"]) == expected["cursor_value"],
                     f"{name}: cursor value")
        addr = group["AevDllCursorAddress"]["rax"]
        driver.check(addr - PREFERRED == 0x300C, f"{name}: cursor address RVA")
        driver.check(addr - 0x300C == PREFERRED, f"{name}: cursor relocation base")
        driver.check(group["AevDllCalls"]["rax"] == expected["calls"], f"{name}: calls")
    driver.check(it["host_import_calls"] == cases_ref[case_order[-1]]["helper"],
                 "reference helper count accumulation")
    win_reset = reset_ref
    oracle_reset = Oracle()
    oracle_reset.reset(win_reset["seed"])
    driver.check(win_reset["cursor_value"] == oracle_reset.table[2],
                 "Windows cursor value matches oracle table[2]")
    driver.check((win_reset["cursor_addr"] - 0x300C) % 0x10000 == 0,
                 "Windows cursor address is a 64K-based relocation target")

    # 3. AOT -> uncovered JIT -> trace -> supplement -> new-process AOT.
    all_manifest = json.loads((driver.art / "all_pe.json").read_text())
    all_blocks = sorted(b["rip"] for f in all_manifest["functions"] for b in f["blocks"])
    empty_dylib = driver.build_dylib(driver.translate(
        PREFERRED, excludes=all_blocks, tag="empty")[0], "pe_empty")
    prep_trace = driver.art / "prep_empty.jsonl"
    driver.calls(PREFERRED,
                 [f"AevDllReset:{reset_ref['seed']}", "AevDllCompute:11,4,8"],
                 "prep-empty", aot=empty_dylib,
                 extra=["--trace", str(prep_trace), "--scenario", "prep"])
    base_ll, base_manifest = driver.supplement(empty_dylib, [prep_trace], PREFERRED, "pe_base")
    base_dylib = driver.build_dylib(base_ll, "pe_base")
    base = load_artifact(base_dylib)
    driver.check(base.module_kind == "pe32+amd64-dll", "base artifact kind")
    driver.check(base.load_base == PREFERRED, "base artifact load base")
    driver.check(len(base.selected_blocks) == len(base_manifest["selected_blocks"]),
                 "base selected table")
    driver.check(len(base.all_blocks) == len(all_blocks), "base all-block table")

    hidden_trace = driver.art / "hidden_base.jsonl"
    rc, hidden = driver.calls(PREFERRED,
                              [f"AevDllReset:{reset_ref['seed']}", "AevDllCompute:-9,2,17"],
                              "hidden-base", aot=base_dylib,
                              extra=["--trace", str(hidden_trace), "--scenario", "hidden"])
    hc = {c["export"]: c for c in hidden["iteration_0"]["calls"]}["AevDllCompute"]
    driver.check(hc["status"] == 0 and hc["rax"] == cases_ref["hidden"]["result"],
                 "hidden base business")
    driver.check(hc["jit_compiles"] >= 1, "hidden base has real JIT compile")
    events = [json.loads(x) for x in hidden_trace.read_text().splitlines() if x.strip()]
    jit_rips = {int(e["rip"], 16) for e in events if e.get("record") == "jit_compile"}
    base_selected = set(base_manifest["selected_blocks"])
    promoted = sorted(jit_rips - base_selected)
    driver.check(bool(promoted), "hidden base has uncovered promoted block")

    prep_base_trace = driver.art / "prep_base.jsonl"
    driver.calls(PREFERRED,
                 [f"AevDllReset:{reset_ref['seed']}", "AevDllCompute:11,4,8"],
                 "prep-base", aot=base_dylib,
                 extra=["--trace", str(prep_base_trace), "--scenario", "prep"])
    supp_ll, supp_manifest = driver.supplement(
        base_dylib, [prep_base_trace, hidden_trace], PREFERRED, "pe_supp")
    driver.check(sorted(supp_manifest["promoted_blocks"]) == promoted,
                 "supplement promoted set matches actual JIT set")
    driver.check(supp_manifest["demoted_blocks"] == [], "supplement demoted set empty")
    supp_dylib = driver.build_dylib(supp_ll, "pe_supp")
    supp_art = load_artifact(supp_dylib)
    driver.check(set(supp_art.selected_blocks) == set(supp_manifest["selected_blocks"]),
                 "supplement selected table")

    supp_trace = driver.art / "hidden_supp.jsonl"
    rc, supp = driver.calls(PREFERRED,
                            [f"AevDllReset:{reset_ref['seed']}", "AevDllCompute:-9,2,17"],
                            "hidden-supp", aot=supp_dylib,
                            extra=["--trace", str(supp_trace), "--scenario", "hidden"])
    sc = {c["export"]: c for c in supp["iteration_0"]["calls"]}["AevDllCompute"]
    driver.check(sc["status"] == 0 and sc["rax"] == cases_ref["hidden"]["result"],
                 "supplemented hidden business")
    driver.check(sc["jit_compiles"] == 0 and sc["jit_execs"] == 0,
                 "supplemented hidden zero JIT")
    events = [json.loads(x) for x in supp_trace.read_text().splitlines() if x.strip()]
    driver.check(any(e.get("record") == "aot" and int(e["rip"], 16) in promoted
                     for e in events), "promoted block executes as AOT")
    driver.check(not any(e.get("record") in ("jit_compile", "jit_exec") and
                          int(e["rip"], 16) in promoted for e in events),
                 "promoted block has no JIT after supplement")

    # Publisher must validate the actual base/new dylibs and provenance.
    pub_tmp = driver.art / "pe_publish_tmp.dylib"
    shutil.copyfile(supp_dylib, pub_tmp)
    pub_out = driver.art / "libpe_published.dylib"
    pub_manifest = driver.art / "pe_published.json"
    proc = driver.run([
        sys.executable, str(driver.source / "scripts" / "publish_aot.py"),
        "--dylib-tmp", str(pub_tmp), "--dylib-out", str(pub_out),
        "--base-aot", str(base_dylib), "--manifest-in",
        str(driver.art / "pe_supp.json"), "--manifest-out", str(pub_manifest),
        "--expected-table", str(len(supp_manifest["selected_blocks"])),
        "--promoted-rip", hex(promoted[0]),
    ], "pe-publish")
    driver.check(proc.returncode == 0, f"publisher accepted PE provenance: {proc.stderr}")
    if proc.returncode == 0:
        pub_art = load_artifact(pub_out)
        driver.check(set(pub_art.selected_blocks) == set(supp_manifest["selected_blocks"]),
                     "published artifact selected table")
        pub_trace = driver.art / "hidden_pubbed.jsonl"
        rc, pubbed = driver.calls(
            PREFERRED, [f"AevDllReset:{reset_ref['seed']}",
                        "AevDllCompute:-9,2,17"],
            "hidden-pubbed", aot=pub_out,
            extra=["--trace", str(pub_trace), "--scenario", "hidden"])
        pc = {c["export"]: c for c in pubbed["iteration_0"]["calls"]}["AevDllCompute"]
        driver.check(pc["status"] == 0 and pc["jit_compiles"] == 0,
                     "published artifact zero JIT hidden")

    base_manifest_text = (driver.art / "pe_supp.json").read_text()
    base_manifest_obj = json.loads(base_manifest_text)
    for name, mutate in (
        ("pub-wrong-base-sha", lambda m: m.__setitem__("base_aot_sha256", "0" * 64)),
        ("pub-wrong-base-selected", lambda m: m.__setitem__("base_selected_blocks", m["base_selected_blocks"][:-1])),
        ("pub-fake-promoted", lambda m: m.__setitem__("promoted_blocks", m["base_selected_blocks"][:1])),
    ):
        variant = json.loads(base_manifest_text)
        mutate(variant)
        variant_path = driver.art / f"{name}.json"
        variant_path.write_text(json.dumps(variant))
        proc = driver.run([
            sys.executable, str(driver.source / "scripts" / "publish_aot.py"),
            "--dylib-tmp", str(pub_tmp), "--dylib-out", str(driver.art / f"{name}.dylib"),
            "--base-aot", str(base_dylib), "--manifest-in", str(variant_path),
            "--manifest-out", str(driver.art / f"{name}.out.json"),
            "--expected-table", str(len(supp_manifest["selected_blocks"])),
        ], name)
        driver.check(proc.returncode != 0, f"publisher rejected {name}")
    proc = driver.run([
        sys.executable, str(driver.source / "scripts" / "publish_aot.py"),
        "--dylib-tmp", str(pub_tmp), "--dylib-out", str(driver.art / "pub-missing-base.dylib"),
        "--base-aot", str(driver.art / "does_not_exist.dylib"),
        "--manifest-in", str(driver.art / "pe_supp.json"),
        "--manifest-out", str(driver.art / "pub-missing-base.json"),
        "--expected-table", str(len(supp_manifest["selected_blocks"])),
    ], "pub-missing-base")
    driver.check(proc.returncode != 0, "publisher rejected missing base")

    # 4. Full non-preferred AOT + business comparison.
    ll190, _, man190 = driver.translate(NON_PREFERRED, tag="all190")
    dylib190 = driver.build_dylib(ll190, "pe_all190")
    art190 = load_artifact(dylib190)
    driver.check(art190.load_base == NON_PREFERRED, "non-preferred artifact load base")
    driver.check(art190.module_file_size == (pe_dir / "aevguest.dll").stat().st_size,
                 "non-preferred artifact file size")
    calls = [f"AevDllReset:{reset_ref['seed']}"]
    for name in ("prep", "hidden", "empty", "wrap"):
        e = cases_ref[name]
        calls += [f"AevDllCompute:{e['a']},{e['b']},{e['n']}", "AevDllState",
                  "AevDllCursorValue", "AevDllCursorAddress", "AevDllCalls"]
    rc, data = driver.calls(NON_PREFERRED, calls, "aot190", aot=dylib190)
    results = data["iteration_0"]["calls"]
    driver.check(all(c["status"] == 0 for c in results), "non-preferred AOT calls OK")
    driver.check(results[-1]["rax"] == 4, "non-preferred AOT call count")
    driver.check(results[-2]["rax"] - NON_PREFERRED == 0x300C, "non-preferred cursor address")

    # 5. Negative paths.
    expect_load_error(driver, "neg-wrong-base-a", pe_dir / "aevguest.dll",
                      contains="module identity", aot=base_dylib, load_base=NON_PREFERRED)
    expect_load_error(driver, "neg-wrong-base-b", pe_dir / "aevguest.dll",
                      contains="module identity", aot=dylib190, load_base=PREFERRED)
    bad_registry = driver.art / "empty.aevimports"
    bad_registry.write_text("# empty\n")
    rc, data = driver.runner_json(
        ["--dll", str(pe_dir / "aevguest.dll"), "--registry", str(bad_registry),
         "--load-base", hex(PREFERRED), "--entry-export", "AevDllCompute",
         "--call", "AevDllCompute:1,2,3"], "neg-unknown-import", expect_error=True)
    driver.check(data.get("status") == 3 and "unknown import" in data.get("error", ""),
                 "unknown import rejected")

    truncated = make_variant(driver, "truncated.dll", lambda d: d.__delitem__(slice(100, None)))
    expect_load_error(driver, "neg-truncated", truncated, contains="invalid")

    def wrong_machine(d):
        hdr = pe_headers(bytes(d))
        d[hdr["pe"] + 4:hdr["pe"] + 6] = (0x14C).to_bytes(2, "little")
    bad_machine = make_variant(driver, "badmachine.dll", wrong_machine)
    expect_load_error(driver, "neg-wrong-machine", bad_machine, contains="AMD64")

    def bad_reloc(d):
        hdr = pe_headers(bytes(d))
        va, _, raw_ptr, _ = hdr["sections"][".reloc"]
        entry = int.from_bytes(d[raw_ptr + 8:raw_ptr + 10], "little")
        entry = (3 << 12) | (entry & 0x0FFF)
        d[raw_ptr + 8:raw_ptr + 10] = entry.to_bytes(2, "little")
    bad_reloc_dll = make_variant(driver, "badreloc.dll", bad_reloc)
    expect_load_error(driver, "neg-bad-reloc", bad_reloc_dll, contains="relocation")

    def bad_export(d):
        hdr = pe_headers(bytes(d))
        exp_off = rva_to_file(hdr, hdr["export_rva"])
        num_funcs = int.from_bytes(d[exp_off + 20:exp_off + 24], "little")
        funcs_rva = int.from_bytes(d[exp_off + 28:exp_off + 32], "little")
        funcs_off = rva_to_file(hdr, funcs_rva)
        # Ordinal 2 is AevDllCalls; point it into .rdata.
        d[funcs_off + 2 * 4:funcs_off + 3 * 4] = (0x2000).to_bytes(4, "little")
        if num_funcs < 3:
            raise Failure("unexpected export count")
    bad_export_dll = make_variant(driver, "badexport.dll", bad_export)
    expect_load_error(driver, "neg-bad-export", bad_export_dll, contains="executable")

    rc, ro = driver.calls(PREFERRED,
                          ["AevDllReadAt:0x180002000", "AevDllWriteAt:0x180002000,1",
                           "AevDllReadAt:0x180002000"], "neg-readonly", expect_error=True)
    driver.check(ro["iteration_0"]["calls"][1]["status"] == 6 and
                 ro["iteration_0"]["calls"][1]["mem_fault_kind"] == 3,
                 "read-only write rejected with write fault")
    driver.check(ro["iteration_0"]["calls"][0]["rax"] == ro["iteration_0"]["calls"][2]["rax"],
                 "read-only write did not change memory")
    driver.check(ro.get("status") == 6 and ro.get("expected_error") is True and
                 ro.get("failure_status") == 6,
                 "expect-error keeps first failure in JSON")
    raw = driver.run([str(driver.runner),
                      "--dll", str(pe_dir / "aevguest.dll"),
                      "--registry", str(pe_dir / "aevimports.txt"),
                      "--load-base", hex(PREFERRED),
                      "--entry-export", "AevDllCompute",
                      "--call", "AevDllWriteAt:0x180002000,1",
                      "--call", "AevDllReadAt:0x180002000"], "neg-noexpect")
    raw_json = json.loads(raw.stdout)
    driver.check(raw.returncode == 6 and raw_json.get("status") == 6 and
                 raw_json.get("expected_error") is False,
                 "failure is non-zero without expect-error")
    rc, nx = driver.calls(PREFERRED, ["AevDllCallPtr:0x180002000"],
                          "neg-nonexec", expect_error=True)
    driver.check(nx["iteration_0"]["calls"][0]["mem_fault_kind"] == 4,
                 "non-executable fetch rejected")
    rc, um = driver.calls(PREFERRED, ["AevDllReadAt:0x180300000"],
                          "neg-unmapped", expect_error=True)
    driver.check(um["iteration_0"]["calls"][0]["mem_fault_kind"] == 1,
                 "unmapped read rejected")
    rc, cb = driver.calls(PREFERRED, ["AevDllReadAt:0x180005FFE"],
                          "neg-cross-boundary", expect_error=True)
    driver.check(cb["iteration_0"]["calls"][0]["mem_fault_kind"] == 5,
                 "cross-boundary read rejected")

    def flip_data(d):
        hdr = pe_headers(bytes(d))
        va, vsize, raw_ptr, raw_size = hdr["sections"][".data"]
        d[raw_ptr + 1] ^= 0xAA
    mutated = make_variant(driver, "mutated.dll", flip_data)
    expect_load_error(driver, "neg-module-sha", mutated, contains="identity",
                      aot=base_dylib)

    aot_runtime = (driver.source / "src" / "aot_runtime.c").read_text()
    bad_abi_src = driver.art / "aot_runtime_bad_abi.c"
    bad_abi_src.write_text(aot_runtime.replace(
        "const uint64_t aev_aot_abi_version = AEV_GUEST_ABI_VERSION;",
        "const uint64_t aev_aot_abi_version = 0;"))
    bad_abi_obj = driver.art / "aot_runtime_bad_abi.o"
    proc = driver.run([driver.clang, "--target=arm64-apple-macos14.0", "-O2",
                       "-I", str(driver.source / "include"), "-c", str(bad_abi_src),
                       "-o", str(bad_abi_obj)], "neg-bad-abi.build")
    driver.require(proc.returncode == 0, "build bad ABI runtime")
    bad_abi_dylib = driver.art / "libbad_abi.dylib"
    proc = driver.run([driver.clang, "-arch", "arm64", "-dynamiclib", "-o",
                       str(bad_abi_dylib), str(driver.art / "pe_supp.o"),
                       str(bad_abi_obj), str(driver.mem_obj)], "neg-bad-abi.link")
    driver.require(proc.returncode == 0, "link bad ABI artifact")
    expect_load_error(driver, "neg-bad-abi", pe_dir / "aevguest.dll",
                      contains="ABI", aot=bad_abi_dylib)

    coff_dylib = driver.build / "generated" / "libcallback_aot.dylib"
    if coff_dylib.is_file():
        expect_load_error(driver, "neg-coff-artifact", pe_dir / "aevguest.dll",
                          contains="module kind", aot=coff_dylib)

    rc, reload = driver.runner_json(
        ["--dll", str(pe_dir / "aevguest.dll"),
         "--registry", str(pe_dir / "aevimports.txt"),
         "--load-base", hex(PREFERRED), "--entry-export", "AevDllCompute",
         "--reload", "3", "--call", f"AevDllReset:{reset_ref['seed']}",
         "--call", "AevDllState", "--call", "AevDllCalls"], "reload-state")
    driver.check(reload.get("stale_handle_status") == 9, "stale handle rejected")
    for i in range(3):
        it = reload.get(f"iteration_{i}", {})
        vals = [c["rax"] for c in it.get("calls", [])]
        driver.check(vals == [reset_ref["seed"], 0, 0], f"reload iteration {i} state isolation")

    # 6. 100 calls against the Python oracle.
    oracle = Oracle()
    oracle.reset(reset_ref["seed"])
    calls = [f"AevDllReset:{reset_ref['seed']}"]
    expected_results = []
    for i in range(100):
        a = (i % 7) + 1
        b = i % 5
        n = i % 9
        expected_results.append(oracle.compute(a, b, n))
        calls.append(f"AevDllCompute:{a},{b},{n}")
    calls += ["AevDllState", "AevDllCalls"]
    rc, hundred = driver.calls(PREFERRED, calls, "hundred")
    got = hundred["iteration_0"]["calls"]
    driver.check(len(got) == 103, "100-call result count")
    for i, expected in enumerate(expected_results):
        if s32(got[i + 1]["rax"]) != expected:
            driver.check(False, f"100-call result {i} {got[i+1]['rax']} == {expected}")
            break
    else:
        driver.check(True, "100-call results match oracle")
    driver.check(s32(got[-2]["rax"]) == oracle.state(), "100-call final state")
    driver.check(got[-1]["rax"] == 100, "100-call call counter")
    driver.check(hundred["iteration_0"]["host_import_calls"] == 100,
                 "100-call host import count")

    total = len(driver.checks)
    passed = total - driver.failures
    print(f"PE_DLL_CHECKS {passed}/{total} passed, {driver.failures} failed")
    (driver.results / "checks.json").write_text(json.dumps(
        [{"ok": ok, "message": msg} for ok, msg in driver.checks], indent=2))
    return 0 if driver.failures == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--source", required=True)
    ap.add_argument("--results", required=True)
    args = ap.parse_args()
    driver = Driver(Path(args.build), Path(args.source), Path(args.results))
    try:
        return run(driver)
    except Failure as exc:
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
