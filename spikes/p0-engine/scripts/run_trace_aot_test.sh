#!/usr/bin/env bash
# P0-TRACE-AOT-01: base trace -> offline supplement -> new-process AOT verification.
set -euo pipefail

BUILD_DIR=${1:?build dir}
SRC_DIR=${2:?source dir}
OUT=${AEV_RESULTS_DIR:-"$BUILD_DIR/results-trace-aot"}
PY=${PYTHON:-python3}
mkdir -p "$OUT"
FAILURES=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILURES=$((FAILURES + 1)); }

BRIDGE="$BUILD_DIR/libaev_bridge.dylib"
TRANSLATOR="$BUILD_DIR/aev-translate"
TRACE_TEST="$SRC_DIR/scripts/trace_aot_test.py"
SUPPLEMENT="$SRC_DIR/scripts/aot_supplement.py"
RT_OBJ="$BUILD_DIR/generated/aot_runtime.o"
MEM_OBJ="$BUILD_DIR/generated/aev_mem_aot.o"
CB_COFF="$SRC_DIR/testdata/callback/aev_callback_fixture.obj"
TR_COFF="$SRC_DIR/testdata/trace/aev_trace_fixture.obj"
CB_BASE="$BUILD_DIR/generated/libcallback_aot.dylib"
TR_BASE="$BUILD_DIR/generated/libtrace_aot.dylib"
CB_REF="$SRC_DIR/testdata/callback/reference_stdout.txt"
TR_REF="$SRC_DIR/testdata/trace/reference_stdout.txt"

echo "== environment =="
uname -a
cmake --version | head -1
clang --version | head -1
sw_vers 2>/dev/null || true
shasum -a 256 "$CB_COFF" "$TR_COFF" "$CB_BASE" "$TR_BASE" "$BRIDGE" | tee "$OUT/hashes.txt"

echo "== base callback trace =="
"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$CB_COFF" --aot "$CB_BASE" \
    --entry 0x140001000 --function aev_guest_callback --fixture callback \
    --mode base --trace "$OUT/callback_base.jsonl" --scenario A \
    --expected-aot-table 4 --promoted-rip 0x140001020 \
    >"$OUT/callback_base.json" 2>"$OUT/callback_base.stderr"
"$PY" - "$OUT/callback_base.json" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
r=d['results'][0]
assert r['status']==0 and r['guest_rax']==3828036654, r
assert r['jit_compiles']==1, r
assert d['artifact_info']['aot_table_count']==4, d['artifact_info']
print('CALLBACK_BASE_OK', r)
PY
grep -q '"record":"jit_compile"' "$OUT/callback_base.jsonl" || fail "callback base has no JIT compile record"
grep -q '"rip":"0x140001020"' "$OUT/callback_base.jsonl" || fail "callback JIT target missing"

echo "== trace API lifecycle =="
"$PY" "$SRC_DIR/scripts/trace_api_test.py" --lib "$BRIDGE" --coff "$CB_COFF" \
    --aot "$CB_BASE" --out "$OUT/trace-api"
pass "trace API lifecycle checks"

echo "== callback supplement =="
"$PY" "$SUPPLEMENT" --translator "$TRANSLATOR" --input "$CB_COFF" \
    --function aev_guest_callback --entry aev_guest_callback \
    --entry-addr 0x140001000 --base-aot "$CB_BASE" \
    --trace "$OUT/callback_base.jsonl" \
    --out-ll "$OUT/callback_supp.ll" --out-header "$OUT/callback_supp.h" \
    --out-manifest "$OUT/callback_supp.json" >"$OUT/callback_supp.stdout"
clang --target=arm64-apple-macos14.0 -O2 -c "$OUT/callback_supp.ll" -o "$OUT/callback_supp.o"
CALLBACK_TMP="$OUT/.callback_supp.$$.dylib"
clang -arch arm64 -dynamiclib -o "$CALLBACK_TMP" \
    "$OUT/callback_supp.o" "$RT_OBJ" "$MEM_OBJ"
"$PY" "$SRC_DIR/scripts/publish_aot.py" --dylib-tmp "$CALLBACK_TMP" \
    --dylib-out "$OUT/libcallback_aot_supplemented.dylib" \
    --manifest-in "$OUT/callback_supp.json" \
    --manifest-out "$OUT/callback_supp.json" \
    --promoted-rip 0x140001020 --expected-table 5
rm -f "$CALLBACK_TMP"
CALLBACK_PUBLISHED_BEFORE=$(shasum -a 256 "$OUT/libcallback_aot_supplemented.dylib" | awk '{print $1}')
cp "$OUT/libcallback_aot_supplemented.dylib" "$OUT/.callback_bad_publish.$$.dylib"
if "$PY" "$SRC_DIR/scripts/publish_aot.py" \
    --dylib-tmp "$OUT/.callback_bad_publish.$$.dylib" \
    --dylib-out "$OUT/libcallback_aot_supplemented.dylib" \
    --manifest-in "$OUT/callback_supp.json" \
    --manifest-out "$OUT/callback_supp.json" \
    --promoted-rip 0xdead --expected-table 5 >"$OUT/bad_publish.stdout" 2>"$OUT/bad_publish.stderr"; then
    fail "publisher accepted a dylib without the promoted RIP"
else
    pass "publisher rejected missing promoted RIP"
fi
rm -f "$OUT/.callback_bad_publish.$$.dylib"
CALLBACK_PUBLISHED_AFTER=$(shasum -a 256 "$OUT/libcallback_aot_supplemented.dylib" | awk '{print $1}')
if [ "$CALLBACK_PUBLISHED_BEFORE" = "$CALLBACK_PUBLISHED_AFTER" ]; then
    pass "failed publish preserved the previous dylib"
else
    fail "failed publish changed the previous dylib"
fi
"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$CB_COFF" \
    --aot "$OUT/libcallback_aot_supplemented.dylib" --entry 0x140001000 \
    --function aev_guest_callback --fixture callback --mode supplemented \
    --trace "$OUT/callback_supp_trace.jsonl" --scenario A \
    --expected-aot-table 5 --promoted-rip 0x140001020 --expect-promoted-selected \
    >"$OUT/callback_supp.json.out" 2>"$OUT/callback_supp.stderr"
"$PY" - "$OUT/callback_supp.json.out" "$OUT/callback_supp_trace.jsonl" <<'PY'
import json,sys
d=json.load(open(sys.argv[1])); r=d['results'][0]
assert r['status']==0 and r['guest_rax']==3828036654, r
assert r['jit_compiles']==0, r
assert d['artifact_info']['aot_table_count']==5, d['artifact_info']
events=[json.loads(x) for x in open(sys.argv[2]) if x.strip()]
assert any(e.get('record')=='aot' and e.get('rip')=='0x140001020' for e in events), 'promoted block not AOT'
assert not any(e.get('record')=='jit_compile' and e.get('rip')=='0x140001020' for e in events), 'promoted block still JIT'
print('CALLBACK_SUPP_OK', r)
PY

echo "== base trace fixture A/B =="
"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$TR_COFF" --aot "$TR_BASE" \
    --entry 0x140001000 --function aev_trace_guest --fixture trace \
    --mode base --case A --trace "$OUT/trace_base_A.jsonl" --scenario A \
    --expected-aot-table 4 --promoted-rip 0x140001015 \
    >"$OUT/trace_base_A.json" 2>"$OUT/trace_base_A.stderr"
"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$TR_COFF" --aot "$TR_BASE" \
    --entry 0x140001000 --function aev_trace_guest --fixture trace \
    --mode base --case B --trace "$OUT/trace_base_B.jsonl" --scenario B \
    --expected-aot-table 4 --promoted-rip 0x140001015 \
    >"$OUT/trace_base_B.json" 2>"$OUT/trace_base_B.stderr"
"$PY" - "$OUT/trace_base_A.json" "$OUT/trace_base_B.json" <<'PY'
import json,sys
a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2]))
ra=a['results'][0]; rb=b['results'][0]
assert ra['status']==0 and ra['guest_rax']==3922, ra
assert rb['status']==0 and rb['guest_rax']==45320, rb
assert ra['jit_compiles']==0, ra
assert rb['jit_compiles']>=1, rb
assert a['artifact_info']['aot_table_count']==4
print('TRACE_BASE_OK', ra, rb)
PY
"$PY" - "$OUT/trace_base_A.jsonl" "$OUT/trace_base_B.jsonl" <<'PY'
import json,sys
def events(path):
    return [json.loads(x) for x in open(path) if x.strip()]
a=events(sys.argv[1]); b=events(sys.argv[2])
def executed(es):
    return [e for e in es if e.get('record') in ('aot','jit_compile','jit_exec')]
if any(e.get('rip')=='0x140001015' for e in executed(a)):
    raise SystemExit('B-only block appeared in A executed trace')
if not any(e.get('record')=='jit_compile' and e.get('rip')=='0x140001015' for e in b):
    raise SystemExit('B block did not JIT compile in base trace')
print('TRACE_BASE_EXEC_EVENTS_OK')
PY

echo "== trace fixture supplement =="
"$PY" "$SUPPLEMENT" --translator "$TRANSLATOR" --input "$TR_COFF" \
    --function aev_trace_guest --entry aev_trace_guest --entry-addr 0x140001000 \
    --base-aot "$TR_BASE" \
    --trace "$OUT/trace_base_A.jsonl" --trace "$OUT/trace_base_B.jsonl" \
    --out-ll "$OUT/trace_supp.ll" --out-header "$OUT/trace_supp.h" \
    --out-manifest "$OUT/trace_supp.json" >"$OUT/trace_supp.stdout"
clang --target=arm64-apple-macos14.0 -O2 -c "$OUT/trace_supp.ll" -o "$OUT/trace_supp.o"
TRACE_TMP="$OUT/.trace_supp.$$.dylib"
clang -arch arm64 -dynamiclib -o "$TRACE_TMP" \
    "$OUT/trace_supp.o" "$RT_OBJ" "$MEM_OBJ"
"$PY" "$SRC_DIR/scripts/publish_aot.py" --dylib-tmp "$TRACE_TMP" \
    --dylib-out "$OUT/libtrace_aot_supplemented.dylib" \
    --manifest-in "$OUT/trace_supp.json" \
    --manifest-out "$OUT/trace_supp.json" \
    --promoted-rip 0x140001015 --expected-table 5
rm -f "$TRACE_TMP"
for c in A B; do
    "$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$TR_COFF" \
        --aot "$OUT/libtrace_aot_supplemented.dylib" --entry 0x140001000 \
        --function aev_trace_guest --fixture trace --mode supplemented --case "$c" \
        --trace "$OUT/trace_supp_${c}.jsonl" --scenario "$c" \
        --expected-aot-table 5 --promoted-rip 0x140001015 --expect-promoted-selected \
        >"$OUT/trace_supp_${c}.json" 2>"$OUT/trace_supp_${c}.stderr"
done
"$PY" - "$OUT/trace_supp_A.json" "$OUT/trace_supp_B.json" "$OUT/trace_supp_B.jsonl" <<'PY'
import json,sys
a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2]))
ra=a['results'][0]; rb=b['results'][0]
assert ra['status']==0 and ra['guest_rax']==3922, ra
assert rb['status']==0 and rb['guest_rax']==45320, rb
assert ra['jit_compiles']==0 and rb['jit_compiles']==0, (ra,rb)
assert a['artifact_info']['aot_table_count']==5 and b['artifact_info']['aot_table_count']==5
events=[json.loads(x) for x in open(sys.argv[3]) if x.strip()]
assert any(e.get('record')=='aot' and e.get('rip')=='0x140001015' for e in events), 'B block not AOT'
assert not any(e.get('record')=='jit_compile' and e.get('rip')=='0x140001015' for e in events), 'B block still JIT'
print('TRACE_SUPP_OK', ra, rb)
PY

echo "== B-only supplement (actual executed union) =="
"$PY" "$SUPPLEMENT" --translator "$TRANSLATOR" --input "$TR_COFF" \
    --function aev_trace_guest --entry aev_trace_guest --entry-addr 0x140001000 \
    --base-aot "$TR_BASE" --trace "$OUT/trace_base_B.jsonl" \
    --out-ll "$OUT/trace_B_only.ll" --out-header "$OUT/trace_B_only.h" \
    --out-manifest "$OUT/trace_B_only.json" >"$OUT/trace_B_only.stdout"
clang --target=arm64-apple-macos14.0 -O2 -c "$OUT/trace_B_only.ll" -o "$OUT/trace_B_only.o"
TRACE_B_TMP="$OUT/.trace_B_only.$$.dylib"
clang -arch arm64 -dynamiclib -o "$TRACE_B_TMP" \
    "$OUT/trace_B_only.o" "$RT_OBJ" "$MEM_OBJ"
"$PY" "$SRC_DIR/scripts/publish_aot.py" --dylib-tmp "$TRACE_B_TMP" \
    --dylib-out "$OUT/libtrace_aot_B_only.dylib" \
    --manifest-in "$OUT/trace_B_only.json" \
    --manifest-out "$OUT/trace_B_only.json" \
    --promoted-rip 0x140001015 --expected-table 4
rm -f "$TRACE_B_TMP"
"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$TR_COFF" \
    --aot "$OUT/libtrace_aot_B_only.dylib" --entry 0x140001000 \
    --function aev_trace_guest --fixture trace --mode supplemented --case B \
    --trace "$OUT/trace_B_only_run.jsonl" --scenario B \
    --expected-aot-table 4 --promoted-rip 0x140001015 --expect-promoted-selected \
    >"$OUT/trace_B_only_run.json" 2>"$OUT/trace_B_only_run.stderr"
"$PY" - "$OUT/trace_B_only_run.json" "$OUT/trace_B_only_run.jsonl" <<'PY'
import json,sys
d=json.load(open(sys.argv[1])); r=d['results'][0]
assert r['status']==0 and r['guest_rax']==45320, r
assert r['jit_compiles']==0 and r['jit_execs']==0, r
assert d['artifact_info']['aot_table_count']==4, d['artifact_info']
events=[json.loads(x) for x in open(sys.argv[2]) if x.strip()]
assert any(e.get('record')=='aot' and e.get('rip')=='0x140001015' for e in events), 'B block not AOT'
assert not any(e.get('record')=='jit_compile' and e.get('rip')=='0x140001015' for e in events), 'B block still JIT'
print('TRACE_B_ONLY_OK', r)
PY

echo "== promotion provenance F1 regressions =="
# The full-AOT artifact is authoritative.  A fresh trace must be taken on it;
# reusing the four-block trace would be rejected by the exact-selected check.
"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$TR_COFF" \
    --aot "$OUT/libtrace_aot_supplemented.dylib" --entry 0x140001000 \
    --function aev_trace_guest --fixture trace --mode supplemented --case AB \
    --trace "$OUT/trace_full5_fresh.jsonl" --scenario AB \
    --expected-aot-table 5 --promoted-rip 0x140001015 --expect-promoted-selected \
    >"$OUT/trace_full5_fresh.json" 2>"$OUT/trace_full5_fresh.stderr"
"$PY" "$SUPPLEMENT" --translator "$TRANSLATOR" --input "$TR_COFF" \
    --function aev_trace_guest --entry aev_trace_guest --entry-addr 0x140001000 \
    --base-aot "$OUT/libtrace_aot_supplemented.dylib" \
    --trace "$OUT/trace_full5_fresh.jsonl" \
    --out-ll "$OUT/trace_full5_resupp.ll" --out-header "$OUT/trace_full5_resupp.h" \
    --out-manifest "$OUT/trace_full5_resupp.json" >"$OUT/trace_full5_resupp.stdout"
if "$PY" - "$OUT/trace_full5_resupp.json" "$OUT/libtrace_aot_supplemented.dylib" <<'PY'
import hashlib, json, sys
m=json.load(open(sys.argv[1]))
base=[0x140001000,0x140001015,0x140001026,0x14000102f,0x140001038]
actual_base=m.get("base_selected_blocks")
actual_new=m.get("selected_blocks")
actual_promoted=m.get("promoted_blocks")
actual_demoted=m.get("demoted_blocks")
print("FULL5 base/new/promoted/demoted", actual_base, actual_new,
      actual_promoted, actual_demoted)
assert m.get("base_aot_sha256")==hashlib.sha256(open(sys.argv[2],"rb").read()).hexdigest(), m.get("base_aot_sha256")
assert actual_base==base, actual_base
assert actual_new==base, actual_new
assert actual_promoted==[], actual_promoted
assert actual_demoted==[], actual_demoted
PY
then
    pass "full-AOT re-supplement empty promotion"
else
    fail "full-AOT re-supplement promoted/demoted provenance"
fi

"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$TR_COFF" \
    --aot "$OUT/libtrace_aot_B_only.dylib" --entry 0x140001000 \
    --function aev_trace_guest --fixture trace --mode supplemented --case AB \
    --trace "$OUT/trace_B_only_AB.jsonl" --scenario AB \
    --expected-aot-table 4 --promoted-rip 0x140001026 \
    >"$OUT/trace_B_only_AB.json" 2>"$OUT/trace_B_only_AB.stderr"
"$PY" "$SUPPLEMENT" --translator "$TRANSLATOR" --input "$TR_COFF" \
    --function aev_trace_guest --entry aev_trace_guest --entry-addr 0x140001000 \
    --base-aot "$OUT/libtrace_aot_B_only.dylib" \
    --trace "$OUT/trace_B_only_AB.jsonl" \
    --out-ll "$OUT/trace_B_only_AB_supp.ll" --out-header "$OUT/trace_B_only_AB_supp.h" \
    --out-manifest "$OUT/trace_B_only_AB_supp.json" >"$OUT/trace_B_only_AB_supp.stdout"
if "$PY" - "$OUT/trace_B_only_AB_supp.json" "$OUT/libtrace_aot_B_only.dylib" <<'PY'
import hashlib, json, sys
m=json.load(open(sys.argv[1]))
base=[0x140001000,0x140001015,0x14000102f,0x140001038]
new=[0x140001000,0x140001015,0x140001026,0x14000102f,0x140001038]
actual_base=m.get("base_selected_blocks")
actual_new=m.get("selected_blocks")
actual_promoted=m.get("promoted_blocks")
actual_demoted=m.get("demoted_blocks")
print("B-ONLY base/new/promoted/demoted", actual_base, actual_new,
      actual_promoted, actual_demoted)
assert m.get("base_aot_sha256")==hashlib.sha256(open(sys.argv[2],"rb").read()).hexdigest(), m.get("base_aot_sha256")
assert actual_base==base, actual_base
assert actual_new==new, actual_new
assert actual_promoted==[0x140001026], actual_promoted
assert actual_demoted==[], actual_demoted
PY
then
    pass "B-only A+B promotes only A body"
else
    fail "B-only A+B promotion provenance"
fi

echo "== no-JIT supplement (same coverage) =="
"$PY" "$SUPPLEMENT" --translator "$TRANSLATOR" --input "$TR_COFF" \
    --function aev_trace_guest --entry aev_trace_guest --entry-addr 0x140001000 \
    --base-aot "$TR_BASE" --trace "$OUT/trace_base_A.jsonl" \
    --out-ll "$OUT/trace_nojit.ll" --out-header "$OUT/trace_nojit.h" \
    --out-manifest "$OUT/trace_nojit.json" >"$OUT/trace_nojit.stdout"
clang --target=arm64-apple-macos14.0 -O2 -c "$OUT/trace_nojit.ll" -o "$OUT/trace_nojit.o"
TRACE_NJ_TMP="$OUT/.trace_nojit.$$.dylib"
clang -arch arm64 -dynamiclib -o "$TRACE_NJ_TMP" \
    "$OUT/trace_nojit.o" "$RT_OBJ" "$MEM_OBJ"
"$PY" "$SRC_DIR/scripts/publish_aot.py" --dylib-tmp "$TRACE_NJ_TMP" \
    --dylib-out "$OUT/libtrace_aot_nojit.dylib" \
    --manifest-in "$OUT/trace_nojit.json" \
    --manifest-out "$OUT/trace_nojit.json" \
    --promoted-rip 0x140001026 --expected-table 4
rm -f "$TRACE_NJ_TMP"
"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$TR_COFF" \
    --aot "$OUT/libtrace_aot_nojit.dylib" --entry 0x140001000 \
    --function aev_trace_guest --fixture trace --mode supplemented --case A \
    --trace "$OUT/trace_nojit_run.jsonl" --scenario A \
    --expected-aot-table 4 --promoted-rip 0x140001026 --expect-promoted-selected \
    >"$OUT/trace_nojit_run.json" 2>"$OUT/trace_nojit_run.stderr"
"$PY" - "$OUT/trace_nojit_run.json" "$OUT/trace_nojit.json" <<'PY'
import json,sys
d=json.load(open(sys.argv[1])); r=d['results'][0]
m=json.load(open(sys.argv[2]))
assert r['status']==0 and r['guest_rax']==3922, r
assert r['jit_compiles']==0 and r['jit_execs']==0, r
assert d['artifact_info']['aot_table_count']==4, d['artifact_info']
assert m.get('promoted_blocks')==[], m.get('promoted_blocks')
print('TRACE_NOJIT_OK', r)
PY

echo "== negative trace/artifact cases =="
"$PY" - "$OUT/callback_base.jsonl" "$OUT" <<'PY'
import json, pathlib, sys
src=pathlib.Path(sys.argv[1]); out=pathlib.Path(sys.argv[2])
lines=[json.loads(x) for x in src.read_text().splitlines() if x.strip()]
header=lines[0]
def write(name, mutate):
    copy=[dict(x) for x in lines]
    mutate(copy)
    (out/name).write_text("\n".join(json.dumps(x) for x in copy)+"\n")
write("bad_sha.jsonl", lambda xs: xs[0].__setitem__("input_sha256","00"*32))
write("bad_aot_sha.jsonl", lambda xs: xs[0].__setitem__("aot_sha256","0"*64))
def bad_aot_missing(xs):
    xs[0].pop("aot_sha256", None)
write("bad_aot_missing.jsonl", bad_aot_missing)
write("bad_block.jsonl", lambda xs: next(x for x in xs if x.get("record")=="aot").__setitem__("block_sha256","00"*32))
write("bad_all.jsonl", lambda xs: xs[0].__setitem__("all_blocks", xs[0]["all_blocks"][1:]))
write("bad_unsorted.jsonl", lambda xs: xs[0].__setitem__("all_blocks", list(reversed(xs[0]["all_blocks"]))))
write("bad_all_duplicate.jsonl", lambda xs: xs[0].__setitem__(
    "all_blocks", [xs[0]["all_blocks"][0], xs[0]["all_blocks"][0]] + xs[0]["all_blocks"][1:]))
write("bad_abi.jsonl", lambda xs: xs[0].__setitem__("translator_abi", 9))
write("bad_entry.jsonl", lambda xs: xs[0].__setitem__("entry", "0x140009999"))
write("bad_function.jsonl", lambda xs: xs[0].__setitem__("function", "not_the_entry"))
write("bad_target.jsonl", lambda xs: xs[0].__setitem__("target", "arm64-apple-macosx13.0.0"))
write("bad_artifact_abi.jsonl", lambda xs: xs[0].__setitem__("aot_artifact_abi", 2))
write("bad_callback_abi.jsonl", lambda xs: xs[0].__setitem__("callback_abi", 1))
write("bad_guest_abi.jsonl", lambda xs: xs[0].__setitem__("guest_abi", 2))
write("bad_selected_duplicate.jsonl", lambda xs: xs[0].__setitem__(
    "aot_selected", [xs[0]["aot_selected"][0], xs[0]["aot_selected"][0]] + xs[0]["aot_selected"][1:]))
def bad_aot_unselected(xs):
    xs[0]["aot_selected"] = [x for x in xs[0]["aot_selected"] if x != "0x140001000"]
write("bad_aot_unselected.jsonl", bad_aot_unselected)
def bad_jit_selected(xs):
    xs[0]["aot_selected"] = sorted(set(xs[0]["aot_selected"] + ["0x140001020"]))
write("bad_jit_selected.jsonl", bad_jit_selected)
def bad_jit_identity(xs):
    for x in xs:
        if x.get("record") == "jit_exec":
            x.pop("offset", None)
            break
write("bad_jit_identity.jsonl", bad_jit_identity)
def bad_interleaved(xs):
    header = xs[0]
    events = xs[1:]
    call1 = events[:next(i for i, e in enumerate(events) if e.get("record") == "call_end") + 1]
    call2 = [dict(e, call=2) for e in call1]
    xs[:] = [header, call1[0], call2[0]] + call1[1:] + call2[1:]
write("bad_interleaved.jsonl", bad_interleaved)
write("bad_trunc.jsonl", lambda xs: xs.clear() or xs.append(header))
def bad_host_args(xs):
    for x in xs:
        if x.get("record") == "host_import":
            x["a0"] = 1
            break
write("bad_host_args.jsonl", bad_host_args)
def bad_failed(xs):
    for x in xs:
        if x.get("record")=="call_end":
            x["status"]=1
write("bad_failed.jsonl", bad_failed)
def bad_call_end_missing(xs):
    for x in xs:
        if x.get("record")=="call_end":
            x.pop("guest_error", None)
            break
write("bad_call_end_missing.jsonl", bad_call_end_missing)
def bad_call_end_status_string(xs):
    for x in xs:
        if x.get("record")=="call_end":
            x["status"]="0"
            break
write("bad_call_end_status_string.jsonl", bad_call_end_status_string)
def bad_call_bool(xs):
    for x in xs[1:]:
        x["call"]=True
        break
write("bad_call_bool.jsonl", bad_call_bool)
def bad_status_bool(xs):
    for x in xs:
        if x.get("record")=="call_end":
            x["status"]=False
            break
write("bad_status_bool.jsonl", bad_status_bool)
def bad_offset_float(xs):
    for x in xs:
        if x.get("record")=="aot":
            x["offset"]=1.5
            break
write("bad_offset_float.jsonl", bad_offset_float)
def bad_guest_error_bool(xs):
    for x in xs:
        if x.get("record")=="call_end":
            x["guest_error"]=False
            break
write("bad_guest_error_bool.jsonl", bad_guest_error_bool)
write("bad_json_array.jsonl", lambda xs: xs.append([]))
(out/"bad_malformed.jsonl").write_text(json.dumps(header)+"\n{not json\n")
PY
for bad in bad_sha bad_aot_sha bad_aot_missing bad_block bad_all bad_unsorted \
           bad_all_duplicate bad_abi bad_entry bad_function bad_target \
           bad_artifact_abi bad_callback_abi bad_guest_abi \
           bad_selected_duplicate bad_aot_unselected bad_jit_selected \
           bad_jit_identity bad_interleaved bad_host_args bad_trunc bad_failed \
           bad_call_end_missing bad_call_end_status_string bad_call_bool \
           bad_status_bool bad_offset_float bad_guest_error_bool bad_json_array \
           bad_malformed; do
    rm -f "$OUT/${bad}_out.ll" "$OUT/${bad}_out.json"
    if "$PY" "$SUPPLEMENT" --translator "$TRANSLATOR" --input "$CB_COFF" \
        --function aev_guest_callback --entry aev_guest_callback \
        --entry-addr 0x140001000 --base-aot "$CB_BASE" --trace "$OUT/$bad.jsonl" \
        --out-ll "$OUT/${bad}_out.ll" --out-header "$OUT/${bad}_out.h" \
        --out-manifest "$OUT/${bad}_out.json" >"$OUT/${bad}.stdout" 2>"$OUT/${bad}.stderr"; then
        fail "$bad trace unexpectedly accepted"
    else
        pass "$bad trace rejected"
    fi
    if [ -e "$OUT/${bad}_out.ll" ] || [ -e "$OUT/${bad}_out.json" ]; then
        fail "$bad produced partial output"
    fi
done
if "$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$CB_COFF" --aot "$CB_BASE" \
    --entry 0x140001000 --function aev_guest_callback --fixture callback \
    --mode supplemented --expected-aot-table 5 >"$OUT/wrong_profile.json" 2>"$OUT/wrong_profile.stderr"; then
    fail "base artifact accepted under supplemented profile"
else
    pass "base artifact rejected under supplemented profile"
fi
if "$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$CB_COFF" \
    --aot "$BUILD_DIR/generated/libcallback_aot_mutated.dylib" \
    --entry 0x140001000 --function aev_guest_callback --fixture callback \
    --mode base --expected-aot-table 4 >"$OUT/mutated_aot.json" 2>"$OUT/mutated_aot.stderr"; then
    fail "mutated input AOT accepted"
else
    pass "mutated input AOT rejected"
fi

"$PY" - "$OUT/callback_supp.ll" "$OUT" <<'PY'
import pathlib,re,sys
src=pathlib.Path(sys.argv[1]).read_text(); out=pathlib.Path(sys.argv[2])
wrong_all=src.replace("@aev_all_blocks = constant [5 x i64] [i64 5368713216,",
                      "@aev_all_blocks = constant [5 x i64] [i64 5368719999,", 1)
wrong_fn=src.replace("ptr @aev_blk_140001000", "ptr @aev_blk_14000101c", 1)
wrong_duplicate=src.replace(
    "%BlockEntry { i64 5368713216, ptr @aev_blk_140001000 }, %BlockEntry { i64 5368713244, ptr @aev_blk_14000101c }",
    "%BlockEntry { i64 5368713216, ptr @aev_blk_140001000 }, %BlockEntry { i64 5368713216, ptr @aev_blk_140001000 }",
    1)
def arr(name,text):
    data=text.encode()+b"\0"
    return f"@{name} = constant [{len(data)} x i8] [" + ", ".join(f"i8 {b}" for b in data) + "]\n"
wrong_target=re.sub(r"@aev_target_triple = constant \[[^\n]*\n",
                    arr("aev_target_triple","arm64-apple-macosx13.0.0"), src, count=1)
wrong_count=src.replace("@aev_block_count = constant i64 5",
                        "@aev_block_count = constant i64 6", 1)
if wrong_all==src or wrong_fn==src or wrong_duplicate==src or wrong_target==src or wrong_count==src:
    raise SystemExit("could not mutate AOT tables")
(out/"wrong_all.ll").write_text(wrong_all)
(out/"wrong_fn.ll").write_text(wrong_fn)
(out/"wrong_duplicate.ll").write_text(wrong_duplicate)
(out/"wrong_target.ll").write_text(wrong_target)
(out/"wrong_count.ll").write_text(wrong_count)
PY
for kind in wrong_all wrong_fn wrong_duplicate wrong_target wrong_count; do
    clang --target=arm64-apple-macos14.0 -O2 -c "$OUT/${kind}.ll" -o "$OUT/${kind}.o"
    clang -arch arm64 -dynamiclib -o "$OUT/lib${kind}.dylib" "$OUT/${kind}.o" "$RT_OBJ" "$MEM_OBJ"
    if "$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$CB_COFF" \
        --aot "$OUT/lib${kind}.dylib" --entry 0x140001000 \
        --function aev_guest_callback --fixture callback --mode base \
        --expected-aot-table 5 >"$OUT/${kind}.json" 2>"$OUT/${kind}.stderr"; then
        fail "$kind AOT table accepted"
    else
        pass "$kind AOT table rejected"
    fi
    PUB_OUT="$OUT/.published_${kind}.dylib"
    PUB_MAN="$OUT/.published_${kind}.json"
    rm -f "$PUB_OUT" "$PUB_MAN"
    if "$PY" "$SRC_DIR/scripts/publish_aot.py" \
        --dylib-tmp "$OUT/lib${kind}.dylib" --dylib-out "$PUB_OUT" \
        --manifest-in "$OUT/callback_supp.json" --manifest-out "$PUB_MAN" \
        --promoted-rip 0x140001020 --expected-table 5 \
        >"$OUT/publish_${kind}.stdout" 2>"$OUT/publish_${kind}.stderr"; then
        fail "$kind publisher accepted a bad dylib"
    else
        pass "$kind publisher rejected a bad dylib"
    fi
    if [ -e "$PUB_OUT" ] || [ -e "$PUB_MAN" ]; then
        fail "$kind publisher left partial output"
    fi
done

sed 's/const uint64_t aev_artifact_abi_version = AEV_ARTIFACT_ABI_VERSION;/const uint64_t aev_artifact_abi_version = 2;/' \
    "$SRC_DIR/src/aot_runtime.c" >"$OUT/aot_runtime_bad_artifact.c"
sed 's/const uint64_t aev_aot_abi_version = AEV_GUEST_ABI_VERSION;/const uint64_t aev_aot_abi_version = 0;/' \
    "$SRC_DIR/src/aot_runtime.c" >"$OUT/aot_runtime_bad_guest.c"
for kind in bad_artifact bad_guest; do
    clang --target=arm64-apple-macos14.0 -O2 -I "$SRC_DIR/include" \
        -c "$OUT/aot_runtime_${kind}.c" -o "$OUT/aot_runtime_${kind}.o"
    clang -arch arm64 -dynamiclib -o "$OUT/libwrong_${kind}_abi.dylib" \
        "$OUT/callback_supp.o" "$OUT/aot_runtime_${kind}.o" "$MEM_OBJ"
    if "$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$CB_COFF" \
        --aot "$OUT/libwrong_${kind}_abi.dylib" --entry 0x140001000 \
        --function aev_guest_callback --fixture callback --mode base \
        --expected-aot-table 5 >"$OUT/wrong_${kind}_abi.json" 2>"$OUT/wrong_${kind}_abi.stderr"; then
        fail "wrong $kind ABI accepted"
    else
        pass "wrong $kind ABI rejected"
    fi
done

echo "== old artifact preservation =="
base_hash_before=$(shasum -a 256 "$CB_BASE" | awk '{print $1}')
# The base artifact must still load after the supplement build.
"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$CB_COFF" --aot "$CB_BASE" \
    --entry 0x140001000 --function aev_guest_callback --fixture callback \
    --mode base --expected-aot-table 4 >"$OUT/base_recheck.json" 2>"$OUT/base_recheck.stderr"
base_hash_after=$(shasum -a 256 "$CB_BASE" | awk '{print $1}')
if [ "$base_hash_before" = "$base_hash_after" ]; then
    pass "base artifact unchanged and re-runnable"
else
    fail "base artifact was overwritten"
fi

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "ALL TRACE/AOT CHECKS PASSED"
    echo "results: $OUT"
    exit 0
fi
echo "$FAILURES TRACE/AOT CHECK(S) FAILED"
echo "results: $OUT"
exit 1
