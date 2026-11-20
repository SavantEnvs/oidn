#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

TEST_BIN="$SRC/build-test"
[ -x "$TEST_BIN/oidnTest" ] && [ -x "$TEST_BIN/oidnDenoise" ] || { echo "test binaries missing: run mayhem/build.sh" >&2; emit_ctrf "oidn" 0 1 0; exit 1; }
work="$(mktemp -d "${TMPDIR:-/tmp}/oidn-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT

passed=0; failed=0

# (1) Upstream's own unit tests (Catch2): device, parameters, error callback, buffer create /
#     read / write / properties. Filter tests are compiled out without built-in weights.
(cd "$work" && "$TEST_BIN/oidnTest" --reporter junit --out "$work/junit.xml") || true
if [ -s "$work/junit.xml" ]; then
  read -r p f < <(python3 -I - "$work/junit.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
r = ET.parse(sys.argv[1]).getroot()
p = f = 0
for tc in r.iter('testcase'):
    if tc.find('failure') is not None or tc.find('error') is not None: f += 1
    else: p += 1
print(p, f)
PY
)
  [ "$p" -gt 0 ] || f=$((f + 1))
  passed=$((passed + p)); failed=$((failed + f))
else
  echo "oidnTest produced no report" >&2; failed=$((failed + 1))
fi

# (2) End-to-end denoising through oidnDenoise with user weights (TZA parser, UNet, CPU kernels,
#     PFM loader/saver): a known-answer check on two synthetic HDR images.
python3 -I - "$work" <<'PY'
import random, struct, sys
d = sys.argv[1]
def write_pfm(path, w, h, px):
    with open(path, 'wb') as f:
        f.write(b'PF\n%d %d\n-1.0\n' % (w, h))
        f.write(struct.pack('<%df' % len(px), *px))
W = H = 64
write_pfm(d + '/const.pfm', W, H, [0.5] * (W * H * 3))
rng = random.Random(1234)
write_pfm(d + '/noisy.pfm', W, H, [max(0.0, 0.5 + rng.uniform(-0.4, 0.4)) for _ in range(W * H * 3)])
PY
for img in const noisy; do
  if (cd "$work" && "$TEST_BIN/oidnDenoise" -d cpu --hdr "$img.pfm" -w "$SRC/mayhem/weights/rt_hdr_small.tza" -o "$img.out.pfm") >"$work/$img.log" 2>&1 \
     && python3 -I - "$work/$img.pfm" "$work/$img.out.pfm" "$img" <<'PY'
import struct, sys
def read_pfm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos+1].isspace(): pos += 1
        s = pos
        while not data[pos:pos+1].isspace(): pos += 1
        parts.append(data[s:pos].decode())
    pos += 1
    assert parts[0] == 'PF', parts
    w, h, scale = int(parts[1]), int(parts[2]), float(parts[3])
    assert scale < 0
    px = struct.unpack('<%df' % (w * h * 3), data[pos:pos + w * h * 12])
    return w, h, px
w0, h0, a = read_pfm(sys.argv[1]); w1, h1, b = read_pfm(sys.argv[2])
assert (w0, h0) == (w1, h1), ((w0, h0), (w1, h1))
mean = lambda v: sum(v) / len(v)
var = lambda v: sum((x - mean(v)) ** 2 for x in v) / len(v)
ma, mb = mean(a), mean(b)
assert abs(mb - ma) < 0.05 * ma, ('mean drift', ma, mb)
if sys.argv[3] == 'const':
    err = mean([abs(x - 0.5) for x in b])
    print('const: mean abs error', err, 'max', max(abs(x - 0.5) for x in b))
    assert err < 0.03, ('constant image not preserved', err)
else:
    assert var(b) < var(a) / 4, ('noise not reduced', var(a), var(b))
print(sys.argv[3], 'ok: mean', ma, '->', mb, 'var', var(a), '->', var(b))
PY
  then passed=$((passed + 1)); echo "PASS denoise-$img"
  else failed=$((failed + 1)); echo "FAIL denoise-$img"; cat "$work/$img.log"
  fi
done

emit_ctrf "oidn" "$passed" "$failed" 0
