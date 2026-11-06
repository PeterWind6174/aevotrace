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
    --expected-aot-table 4 >"$OUT/callback_base.json" 2>"$OUT/callback_base.stderr"
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
    --entry-addr 0x140001000 --trace "$OUT/callback_base.jsonl" \
    --out-ll "$OUT/callback_supp.ll" --out-header "$OUT/callback_supp.h" \
    --out-manifest "$OUT/callback_supp.json" >"$OUT/callback_supp.stdout"
clang --target=arm64-apple-macos14.0 -O2 -c "$OUT/callback_supp.ll" -o "$OUT/callback_supp.o"
clang -arch arm64 -dynamiclib -o "$OUT/libcallback_aot_supplemented.dylib" \
    "$OUT/callback_supp.o" "$RT_OBJ" "$MEM_OBJ"
"$PY" - "$OUT/callback_supp.json" "$OUT/libcallback_aot_supplemented.dylib" <<'PY'
import hashlib,json,sys
p=sys.argv[1]; lib=sys.argv[2]
d=json.load(open(p))
h=hashlib.sha256(open(lib,'rb').read()).hexdigest()
d['aot_sha256']=h
d['aot_path']=lib
open(p,'w').write(json.dumps(d,indent=2)+'\n')
print('CALLBACK_SUPP_SHA', h, 'selected', d.get('selected_blocks'))
PY
"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$CB_COFF" \
    --aot "$OUT/libcallback_aot_supplemented.dylib" --entry 0x140001000 \
    --function aev_guest_callback --fixture callback --mode supplemented \
    --trace "$OUT/callback_supp_trace.jsonl" --scenario A \
    --expected-aot-table 5 >"$OUT/callback_supp.json.out" 2>"$OUT/callback_supp.stderr"
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
    --expected-aot-table 4 >"$OUT/trace_base_A.json" 2>"$OUT/trace_base_A.stderr"
"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$TR_COFF" --aot "$TR_BASE" \
    --entry 0x140001000 --function aev_trace_guest --fixture trace \
    --mode base --case B --trace "$OUT/trace_base_B.jsonl" --scenario B \
    --expected-aot-table 4 >"$OUT/trace_base_B.json" 2>"$OUT/trace_base_B.stderr"
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
    --trace "$OUT/trace_base_A.jsonl" --trace "$OUT/trace_base_B.jsonl" \
    --out-ll "$OUT/trace_supp.ll" --out-header "$OUT/trace_supp.h" \
    --out-manifest "$OUT/trace_supp.json" >"$OUT/trace_supp.stdout"
clang --target=arm64-apple-macos14.0 -O2 -c "$OUT/trace_supp.ll" -o "$OUT/trace_supp.o"
clang -arch arm64 -dynamiclib -o "$OUT/libtrace_aot_supplemented.dylib" \
    "$OUT/trace_supp.o" "$RT_OBJ" "$MEM_OBJ"
"$PY" - "$OUT/trace_supp.json" "$OUT/libtrace_aot_supplemented.dylib" <<'PY'
import hashlib,json,sys
p=sys.argv[1]; lib=sys.argv[2]
d=json.load(open(p)); d['aot_sha256']=hashlib.sha256(open(lib,'rb').read()).hexdigest(); d['aot_path']=lib
open(p,'w').write(json.dumps(d,indent=2)+'\n')
print('TRACE_SUPP_SHA', d['aot_sha256'], 'selected', d.get('selected_blocks'))
PY
for c in A B; do
    "$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$TR_COFF" \
        --aot "$OUT/libtrace_aot_supplemented.dylib" --entry 0x140001000 \
        --function aev_trace_guest --fixture trace --mode supplemented --case "$c" \
        --trace "$OUT/trace_supp_${c}.jsonl" --scenario "$c" \
        --expected-aot-table 5 >"$OUT/trace_supp_${c}.json" 2>"$OUT/trace_supp_${c}.stderr"
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
write("bad_block.jsonl", lambda xs: next(x for x in xs if x.get("record")=="aot").__setitem__("block_sha256","00"*32))
write("bad_all.jsonl", lambda xs: xs[0].__setitem__("all_blocks", xs[0]["all_blocks"][1:]))
write("bad_unsorted.jsonl", lambda xs: xs[0].__setitem__("all_blocks", list(reversed(xs[0]["all_blocks"]))))
write("bad_abi.jsonl", lambda xs: xs[0].__setitem__("translator_abi", 9))
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
PY
for bad in bad_sha bad_block bad_all bad_unsorted bad_abi bad_host_args bad_trunc bad_failed; do
    rm -f "$OUT/${bad}_out.ll" "$OUT/${bad}_out.json"
    if "$PY" "$SUPPLEMENT" --translator "$TRANSLATOR" --input "$CB_COFF" \
        --function aev_guest_callback --entry aev_guest_callback \
        --entry-addr 0x140001000 --trace "$OUT/$bad.jsonl" \
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
import pathlib,sys
src=pathlib.Path(sys.argv[1]).read_text(); out=pathlib.Path(sys.argv[2])
wrong_all=src.replace("@aev_all_blocks = constant [5 x i64] [i64 5368713216,",
                      "@aev_all_blocks = constant [5 x i64] [i64 5368719999,", 1)
wrong_fn=src.replace("ptr @aev_blk_140001000", "ptr @aev_blk_14000101c", 1)
if wrong_all==src or wrong_fn==src:
    raise SystemExit("could not mutate AOT tables")
(out/"wrong_all.ll").write_text(wrong_all)
(out/"wrong_fn.ll").write_text(wrong_fn)
PY
for kind in wrong_all wrong_fn; do
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
done

echo "== old artifact preservation =="
base_hash_before=$(shasum -a 256 "$CB_BASE" | awk '{print $1}')
# The base artifact must still load after the supplement build.
"$PY" "$TRACE_TEST" --lib "$BRIDGE" --coff "$CB_COFF" --aot "$CB_BASE" \
    --entry 0x140001000 --function aev_guest_callback --fixture callback \
    --mode base --expected-aot-table 4 >"$OUT/base_recheck.json" 2>"$OUT/base_recheck.stderr"
base_hash_after=$(shasum -a 256 "$CB_BASE" | awk '{print $1}')
[ "$base_hash_before" = "$base_hash_after" ] || fail "base artifact was overwritten"
pass "base artifact unchanged and re-runnable"

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "ALL TRACE/AOT CHECKS PASSED"
    echo "results: $OUT"
    exit 0
fi
echo "$FAILURES TRACE/AOT CHECK(S) FAILED"
echo "results: $OUT"
exit 1
